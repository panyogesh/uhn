#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <vector>
#include <string>
#include <memory>
#include <iostream>

// UHD headers
#include <uhd/device.hpp>
#include <uhd/types/device_addr.hpp>

// FlexSDR headers
#include "device/flexsdr_device.hpp"
#include "transport/flexsdr_secondary.hpp"
#include "transport/eal_bootstrap.hpp"
#include "conf/config_params.hpp"

// DPDK headers
extern "C" {
#include <rte_config.h>
#include <rte_eal.h>
#include <rte_errno.h>
}

#include "common_lib.h"

// Forward declaration for FlexSDR registration
extern "C" void flexsdr_register_with_uhd();

extern int flexsdr_tx_thread;

typedef struct {
  // --------------------------------
  // variables for USRP configuration
  // --------------------------------
  //! flexsdr device smart pointer
  std::shared_ptr<flexsdr::flexsdr_device> flexsdr;

  //! FlexSDR Secondary process for DPDK resource management
  std::shared_ptr<flexsdr::FlexSDRSecondary> secondary;

  //! DPDK context for device
  std::shared_ptr<flexsdr::DpdkContext> dpdk_ctx;

  //! USRP TX Stream
  uhd::tx_streamer::sptr tx_stream;
  //! USRP RX Stream
  uhd::rx_streamer::sptr rx_stream;

  //! USRP TX Metadata
  uhd::tx_metadata_t tx_md;
  //! USRP RX Metadata
  uhd::rx_metadata_t rx_md;

  //! Sampling rate
  double sample_rate;

  //! TX forward samples. We use usrp_time_offset to get this value
  int tx_forward_nsamps; //166 for 20Mhz

  //! gpio bank to use
  char *gpio_bank;

  //! Configuration file path
  std::string yaml_config_path;

  // --------------------------------
  // Debug and output control
  // --------------------------------
  int num_underflows;
  int num_overflows;
  int num_seq_errors;
  int64_t tx_count;
  int64_t rx_count;
  int wait_for_first_pps;
  int use_gps;
  //! timestamp of RX packet
  openair0_timestamp rx_timestamp;
} flexsdr_state_t;

// Start FlexSDR streaming (minimal start similar to USRP start)
static int trx_start_wrapper(openair0_device_t* device) {
  auto* s = static_cast<flexsdr_state_t*>(device->priv);
  if (!s) return -1;

  // Initialize basic state/counters
  s->wait_for_first_pps = 0;
  s->rx_count = 0;
  s->tx_count = 0;
  s->rx_timestamp = 0;

  // Cache sample rate from device cfg if available
  if (device->openair0_cfg) {
    s->sample_rate = device->openair0_cfg->sample_rate;
  }

  // Start RX streaming immediately (no PPS on FlexSDR device interface)
  if (s->rx_stream) {
    uhd::stream_cmd_t cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
    cmd.stream_now = true;
    s->rx_stream->issue_stream_cmd(cmd);
  }

  return 0;
}

static int trx_write_wrapper(openair0_device_t* device,
                             openair0_timestamp ts,
                             void **buffers,
                             int nsamps,
                             int flags,
                             int cc) {
    auto* s = static_cast<flexsdr_state_t*>(device->priv);
    if (!s || !s->tx_stream) return -1;

    uhd::tx_metadata_t md{};
    //md.start_of_burst = (flags & SOME_SOB_FLAG) != 0;
    //md.end_of_burst   = (flags & SOME_EOB_FLAG) != 0;
    md.has_time_spec  = true;
    md.time_spec = uhd::time_spec_t::from_ticks(ts, /*rate*/ s->sample_rate);

    // Convert buffers array to UHD ref_vector (buffers is void**, not the data itself!)
    const size_t num_chans = s->tx_stream->get_num_channels();
    std::vector<const void*> buff_ptrs;
    for (size_t i = 0; i < num_chans; i++) {
        buff_ptrs.push_back(buffers[i]);
    }

    size_t sent = s->tx_stream->send(buff_ptrs, nsamps, md, /*timeout*/ 0.1);
   
    return static_cast<int>(sent);
}

static int trx_read_wrapper(openair0_device_t* device,
                            openair0_timestamp* ts,
                            void **buffers,
                            int nsamps,
                            int num_antennas) {
    auto* s = static_cast<flexsdr_state_t*>(device->priv);
    if (!s || !s->rx_stream) return -1;
    if (!buffers || !buffers[0] || nsamps <= 0) return 0;

    // Our FlexSDR RX streamer currently supports a single channel.
    // Tolerate callers passing more antennas; we fill only supported channels.
    const size_t streamer_ch = s->rx_stream->get_num_channels();
    if (streamer_ch == 0) return 0;

    uhd::rx_metadata_t md{};
    // Clamp nsamps to a sane upper bound and the streamer's own limit
    const size_t req = static_cast<size_t>(nsamps);
    const size_t max_req = 1u << 20; // 1M complex samples
    size_t nsamps_clamped = std::min(req, max_req);
    nsamps_clamped = std::min(nsamps_clamped, s->rx_stream->get_max_num_samps());

    // Accumulate reads until the requested number of samples is returned
    size_t total_read = 0;
    // Build a working vector for the recv API
    std::vector<void*> buffv(streamer_ch);
    // bytes per complex sample for sc16
    constexpr size_t bytes_per_sample = sizeof(int16_t) * 2;

    while (total_read < nsamps_clamped) {
      // Advance per-channel pointers by total_read
      for (size_t ch = 0; ch < streamer_ch; ++ch) {
        // Use the provided buffers; if caller passed fewer than streamer_ch, reuse buffers[0]
        void* base = buffers[(num_antennas > (int)ch) ? ch : 0];
        buffv[ch] = static_cast<void*>(
            reinterpret_cast<uint8_t*>(base) + total_read * bytes_per_sample);
      }

      const size_t remaining = nsamps_clamped - total_read;
      const size_t got = s->rx_stream->recv(buffv, remaining, md, /*timeout*/ 0.2, /*one_packet*/ false);

      if (got == 0) {
        // Timeout or no data yet; try again to meet OAI's blocking semantics
        continue;
      }

      // Record timestamp from the first chunk if provided
      if (total_read == 0 && md.has_time_spec && ts && s->sample_rate > 0.0) {
        *ts = static_cast<openair0_timestamp>(md.time_spec.to_ticks(/*rate*/ s->sample_rate));
      }

      total_read += got;
    }

    return static_cast<int>(total_read);
}

//TODO CHECK
static int trx_set_freq_wrapper(openair0_device_t* device, openair0_config_t* cfg)
{
  flexsdr_state_t* s = (flexsdr_state_t*)device->priv;

  // TX channels
  for (int i = 0; i < cfg->tx_num_channels; i++) {
    printf("[FlexSDR] TX channel %d: freq = %f Hz\n", i, cfg->tx_freq[i]);
    //s->flexsdr->set_freq(cfg->tx_freq[i], i);
  }

  // RX channels
  for (int i = 0; i < cfg->rx_num_channels; i++) {
    printf("[FlexSDR] RX channel %d, freq = %f Hz\n", i, cfg->rx_freq[i]);
    //s->flexsdr->set_freq(cfg->rx_freq[i], rfcontrol::RX1 + i);
  }

  // WRX channels
  for (int i = 0; i < cfg->wrx_num_channels; i++) {
    printf("[FlexSDR] WRX channel %d, freq = %f Hz\n", i, cfg->wrx_freq[i]);
    //s->flexsdr->set_freq(cfg->wrx_freq[i], rfcontrol::WRX1 + i);
  }

  return 0;
}

static int trx_set_gains_wrapper(openair0_device_t* device, openair0_config_t* cfg)
{
  flexsdr_state_t* s = (flexsdr_state_t*)device->priv;

  // TX channels
  for (int i = 0; i < cfg->tx_num_channels; i++) {
    //s->flexsdr->set_gains(cfg->tx_gain[i], i);
  }

  // RX channels
  for (int i = 0; i < cfg->rx_num_channels; i++) {
    //s->flexsdr->set_gains(cfg->rx_gain[i], rfcontrol::RX1 + i);
  }

  // WRX channels
  for (int i = 0; i < cfg->wrx_num_channels; i++) {
    double requested_gain = cfg->wrx_gain[i];

    // Map OAI gain (0..110) to FlexSDR WRX valid range (-27..0)
    // double wrx_gain = std::max(-27.0, std::min(0.0, requested_gain - 100.0));
    //s->flexsdr->set_gains(requested_gain, rfcontrol::WRX1 + i);
  }

  return 0;
}

static void trx_end_wrapper(openair0_device_t* device) {
    auto* s = static_cast<flexsdr_state_t*>(device->priv);
    if (!s) return;

    // Clean up streams first
    s->tx_stream.reset();
    s->rx_stream.reset();

    // Clean up DPDK context
    s->dpdk_ctx.reset();

    // Clean up secondary process
    s->secondary.reset();

    // Clean up device (shared_ptr cleanup)
    s->flexsdr.reset();   // drops the reference; deletes object if refcount==0

    free(s);
}


extern "C" int device_init(openair0_device_t* device, openair0_config_t* cfg) {
    printf("******Initializing FlexSDR device...******\n");
  
    flexsdr_state_t *state;

    // Get configuration file from environment variable or use default
    const char* env_config = std::getenv("FLEXSDR_CONFIG_FILE");
    std::string yaml_config = env_config ? env_config : "conf/configurations-ue.yaml";
    
    // Get device address from environment or use default
    const char* env_addr = std::getenv("FLEXSDR_DEVICE_ADDR");
    std::string device_args = env_addr ? 
        std::string("type=flexsdr,addr=") + env_addr : 
        "type=flexsdr,addr=192.168.137.99:5555";

    if (device->priv == NULL) {
      state = (flexsdr_state_t *)calloc(1, sizeof(flexsdr_state_t));
      device->priv = state;
      AssertFatal(state != NULL, "FlexSDR device: memory allocation failure\n");
      
      // Store config path for secondary init
      state->yaml_config_path = yaml_config;
      
      printf("[FlexSDR] Using configuration: %s\n", yaml_config.c_str());
      printf("[FlexSDR] Device address: %s\n", device_args.c_str());
    } else {
      LOG_E(HW, "multiple device init detected\n");
      return 0;
    }

    // Assign wrappers to match OAI signatures
    device->trx_start_func     = trx_start_wrapper;
    device->trx_stop_func      = nullptr; // optional, implement if needed
    device->trx_write_func     = trx_write_wrapper;
    device->trx_read_func      = trx_read_wrapper;
    device->trx_set_freq_func  = trx_set_freq_wrapper;
    device->trx_set_gains_func = trx_set_gains_wrapper;
    device->trx_end_func       = trx_end_wrapper;

    device->type = FLEXSDR_DEV;

    try {
        // Step 1: Load YAML configuration
        printf("[FlexSDR] Loading configuration from %s\n", yaml_config.c_str());
        flexsdr::conf::PrimaryConfig primary_cfg;
        if (flexsdr::conf::load_from_yaml(yaml_config.c_str(), primary_cfg) != 0) {
            std::cerr << "[ERROR] Failed to load YAML config\n";
            return 2;
        }

        // Step 2: Initialize DPDK EAL as secondary process using EalBootstrap
        printf("[FlexSDR] Initializing DPDK EAL as secondary process...\n");
        flexsdr::EalBootstrap eal(primary_cfg, "oai_flexsdr_transport");
        eal.build_args({"--proc-type=secondary"});
        
        int eal_rc = eal.init();
        if (eal_rc < 0) {
            std::cerr << "[ERROR] EAL init failed: " << rte_strerror(rte_errno) << "\n";
            return 2;
        }
        printf("[FlexSDR] DPDK EAL initialized (consumed %d args)\n", eal_rc);

        // Step 3: Create FlexSDRSecondary and lookup resources
        printf("[FlexSDR] Creating FlexSDRSecondary and looking up resources...\n");
        state->secondary = std::make_shared<flexsdr::FlexSDRSecondary>(yaml_config);
        
        if (state->secondary->init_resources() != 0) {
            std::cerr << "[ERROR] Failed to lookup secondary resources\n";
            return 2;
        }
        
        printf("[FlexSDR] Secondary initialized successfully\n");
        printf("[FlexSDR] RX rings: %zu, TX rings: %zu, Pools: %zu\n",
               state->secondary->num_rx_queues(),
               state->secondary->num_tx_queues(),
               state->secondary->num_pools());

        // Step 4: Register FlexSDR with UHD
        printf("[FlexSDR] Registering FlexSDR with UHD...\n");
        flexsdr_register_with_uhd();

        // Step 5: Create UHD device
        printf("[FlexSDR] Creating UHD device...\n");
        uhd::device_addr_t dev_args(device_args);
        auto uhd_dev = uhd::device::make(dev_args);
        if (!uhd_dev) {
            std::cerr << "[ERROR] UHD device::make returned null\n";
            return 2;
        }

        state->flexsdr = std::dynamic_pointer_cast<flexsdr::flexsdr_device>(uhd_dev);
        if (!state->flexsdr) {
            std::cerr << "[ERROR] Device cast failed (not a flexsdr_device)\n";
            std::cerr << "[HINT] Ensure your finder/registration is compiled & loaded\n";
            return -1;
        }

        // Step 6: Create and populate DPDK context
        printf("[FlexSDR] Creating DPDK context and attaching to device...\n");
        state->dpdk_ctx = std::make_shared<flexsdr::DpdkContext>();
        
        // Get rings/pools from secondary (queue 0)
        state->dpdk_ctx->ue_in  = state->secondary->rx_ring_for_queue(0);
        state->dpdk_ctx->ue_tx0 = state->secondary->tx_ring_for_queue(0);
        state->dpdk_ctx->ue_mp  = state->secondary->pool_for_queue(0);
        
        // CRITICAL: Attach secondary as TxBackend provider
        state->dpdk_ctx->secondary = state->secondary.get();
        
        // Step 7: Attach DPDK context to device
        state->flexsdr->attach_dpdk_context(state->dpdk_ctx, flexsdr::Role::UE);
        printf("[FlexSDR] DPDK context attached successfully\n");

        // Initialize cached sample rate
        if (cfg) {
            state->sample_rate = cfg->sample_rate;
        }

        // Step 8: Configure all TX and RX channels
        printf("[FlexSDR] Configuring all TX/RX channels...\n");
        printf("[FlexSDR] tx_num_channels=%d rx_num_channels=%d wrx_num_channels=%d\n",
               cfg->tx_num_channels, cfg->rx_num_channels, cfg->wrx_num_channels);
        device->trx_set_freq_func(device, cfg);
        device->trx_set_gains_func(device, cfg);

        // Step 9: Create TX/RX streamers
        printf("[FlexSDR] Creating RX/TX streams...\n");
        
        // Create RX stream (4 channels for RX1, RX2, WRX1, WRX2)
        uhd::stream_args_t rx_args{"sc16", "sc16"};
        rx_args.channels = {0, 1, 2, 3};
        state->rx_stream = state->flexsdr->get_rx_stream(rx_args);

        // Create TX stream (single channel for now)
        uhd::stream_args_t tx_args{"sc16", "sc16"};
        tx_args.channels = {0};
        state->tx_stream = state->flexsdr->get_tx_stream(tx_args);

        printf("[FlexSDR] Streams created: RX=%zu channels, TX=%zu channels\n",
               state->rx_stream->get_num_channels(),
               state->tx_stream->get_num_channels());

        // Step 10: Set sample rates for all channels
        if (device->openair0_cfg) {
            double sample_rate = device->openair0_cfg->sample_rate;
            printf("[FlexSDR] Setting sample rate to %.2f Hz\n", sample_rate);
            
            // Set TX rate for all TX channels
            for (int i = 0; i < device->openair0_cfg->tx_num_channels; i++) {
                //state->flexsdr->set_tx_rate(sample_rate, i);
            }

            // Set RX rate for all RX channels
            for (int i = 0; i < device->openair0_cfg->rx_num_channels; i++) {
                //state->flexsdr->set_rx_rate(sample_rate, i);
            }

            // Set RX rate for all WRX channels (if applicable)
            for (int i = 0; i < device->openair0_cfg->wrx_num_channels; i++) {
                //state->flexsdr->set_rx_rate(sample_rate, i + device->openair0_cfg->rx_num_channels);
            }
        }

        printf("******FlexSDR device initialized successfully******\n");

    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Exception during initialization: " << e.what() << "\n";
        return -1;
    }

    return 0;
}
