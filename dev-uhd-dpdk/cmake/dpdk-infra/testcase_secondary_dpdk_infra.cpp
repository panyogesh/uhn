#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <csignal>
#include <atomic>
#include <unistd.h>
#include <cmath>

#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>

#include "conf/config_params.hpp"
#include "transport/flexsdr_secondary.hpp"
#include "transport/eal_bootstrap.hpp"

// Global flag for graceful shutdown
static std::atomic<bool> g_shutdown_requested{false};

static void signal_handler(int signum) {
  std::fprintf(stderr, "\n[ue] caught signal %d, requesting shutdown...\n", signum);
  g_shutdown_requested.store(true);
}

static void setup_signal_handlers() {
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);
}

static int wait_for_primary_ready(int max_retries = 30) {
  std::fprintf(stderr, "[ue] Waiting for primary process to initialize resources...\n");
  
  for (int i = 0; i < max_retries; i++) {
    // Check if DPDK shared memory is available
    // We'll do this by attempting EAL init in secondary mode
    std::fprintf(stderr, "[ue] Retry %d/%d...\n", i + 1, max_retries);
    sleep(1);
    
    // For now, just wait. In production, you'd check for a sync file or use IPC
    if (i >= 2) {  // Give primary at least 3 seconds
      return 0;
    }
  }
  
  std::fprintf(stderr, "[ue] WARNING: Max retries reached\n");
  return 0;  // Proceed anyway
}

int main(int argc, char** argv) {
  std::fprintf(stderr, "========================================\n");
  std::fprintf(stderr, "FlexSDR Secondary-UE DPDK Infrastructure Test\n");
  std::fprintf(stderr, "PID: %d\n", getpid());
  std::fprintf(stderr, "========================================\n\n");

  if (argc < 2) {
    std::fprintf(stderr, "Usage: %s <config.yaml>\n", argv[0]);
    std::fprintf(stderr, "Example: %s conf/configurations-ue.yaml\n", argv[0]);
    std::fprintf(stderr, "\nNOTE: Primary process must be running first!\n");
    return 2;
  }

  std::string cfg_path = argv[1];
  std::fprintf(stderr, "[ue] Loading config from: %s\n", cfg_path.c_str());

  setup_signal_handlers();

  // Wait for primary to be ready
  wait_for_primary_ready();

  // Enable verbose DPDK logging
  setenv("RTE_LOG_LEVEL", "8", 1);

  // Load config for EAL initialization
  flexsdr::conf::PrimaryConfig cfg;
  int cfg_rc = flexsdr::conf::load_from_yaml(cfg_path.c_str(), cfg);
  if (cfg_rc) {
    std::fprintf(stderr, "[ue] ERROR: Failed to load config (rc=%d)\n", cfg_rc);
    return 1;
  }

  // Initialize DPDK EAL as secondary process
  std::fprintf(stderr, "[ue] Initializing DPDK EAL in secondary mode...\n");
  flexsdr::EalBootstrap eal(cfg, "flexsdr-ue");
  eal.build_args({"--proc-type=secondary"});
  
  std::fprintf(stderr, "[ue] EAL arguments: %s\n", eal.args_as_cmdline().c_str());
  
  int eal_rc = eal.init();
  if (eal_rc < 0) {
    std::fprintf(stderr, "[ue] ERROR: EAL initialization failed (rc=%d)\n", eal_rc);
    std::fprintf(stderr, "[ue] Is the primary process running?\n");
    return 1;
  }
  std::fprintf(stderr, "[ue] EAL initialized successfully (consumed %d args)\n", eal_rc);

  // Create FlexSDRSecondary instance (after EAL is initialized)
  flexsdr::FlexSDRSecondary secondary_app(cfg_path);
  std::fprintf(stderr, "[ue] FlexSDRSecondary constructed\n");

  // Lookup shared rings created by primary
  std::fprintf(stderr, "[ue] Looking up shared rings from primary...\n");
  int rc = secondary_app.init_resources();
  if (rc) {
    std::fprintf(stderr, "[ue] ERROR: Resource lookup failed (rc=%d)\n", rc);
    std::fprintf(stderr, "[ue] Did primary create the rings?\n");
    return 1;
  }

  // Get actual resources found
  const auto& tx_rings = secondary_app.tx_rings();
  const auto& rx_rings = secondary_app.rx_rings();
  
  std::fprintf(stderr, "\n[ue] ✓ All resources found successfully!\n");
  std::fprintf(stderr, "[ue] Shared rings accessed:\n");
  std::fprintf(stderr, "  - %zu TX ring(s):\n", tx_rings.size());
  for (const auto& ring : tx_rings) {
    std::fprintf(stderr, "    * %s (size=%u)\n", ring->name, rte_ring_get_size(ring));
  }
  std::fprintf(stderr, "  - %zu RX ring(s):\n", rx_rings.size());
  for (const auto& ring : rx_rings) {
    std::fprintf(stderr, "    * %s (size=%u)\n", ring->name, rte_ring_get_size(ring));
  }
  std::fprintf(stderr, "\n[ue] Secondary process is ready!\n");
  std::fprintf(stderr, "[ue] Generating and sending IQ samples to primary...\n");
  std::fprintf(stderr, "[ue] Press Ctrl+C to shutdown gracefully...\n\n");

  // Lookup memory pool for allocating mbufs
  // Try UE pool first, then GNB pool
  rte_mempool* pool = rte_mempool_lookup("ue_outbound_pool");
  const char* pool_name = "ue_outbound_pool";
  
  if (!pool) {
    pool = rte_mempool_lookup("gnb_outbound_pool");
    pool_name = "gnb_outbound_pool";
  }
  
  if (!pool) {
    std::fprintf(stderr, "[ue] ERROR: Cannot find ue_outbound_pool or gnb_outbound_pool\n");
    return 1;
  }
  
  if (tx_rings.empty()) {
    std::fprintf(stderr, "[ue] ERROR: No TX rings available\n");
    return 1;
  }

  std::fprintf(stderr, "[ue] Using pool: %s\n", pool_name);
  std::fprintf(stderr, "[ue] Sending to %zu TX ring(s)\n\n", tx_rings.size());

  // IQ sample generation parameters
  const size_t samples_per_mbuf = 512;  // 512 IQ samples per mbuf
  const size_t mbuf_data_size = samples_per_mbuf * 2 * sizeof(int16_t); // I + Q
  const unsigned batch_size = 32;  // Send 32 mbufs at a time
  
  uint64_t total_samples_sent = 0;
  uint64_t total_bursts = 0;
  double phase = 0.0;
  const double frequency = 1000.0;  // 1 kHz test signal
  const double sample_rate = 30720000.0;  // 30.72 MHz (LTE standard)
  const double phase_increment = 2.0 * M_PI * frequency / sample_rate;
  const int16_t amplitude = 16000;  // Amplitude for int16 samples

  std::fprintf(stderr, "[ue] Generating %zu-sample sine wave IQ signal per mbuf\n", samples_per_mbuf);
  std::fprintf(stderr, "[ue] Signal: %.1f kHz @ %.2f MHz sample rate\n", 
               frequency/1000.0, sample_rate/1000000.0);

  // Get RX ring for receiving responses from primary
  rte_ring* rx_ring = rx_rings.empty() ? nullptr : rx_rings[0];
  uint64_t total_responses_received = 0;
  uint64_t response_bursts_received = 0;
  
  // Transmission loop
  while (!g_shutdown_requested.load()) {
    // Allocate a burst of mbufs
    rte_mbuf* mbufs[batch_size];
    int n_alloc = rte_pktmbuf_alloc_bulk(pool, mbufs, batch_size);
    
    if (n_alloc != 0) {
      std::fprintf(stderr, "[ue] WARNING: Failed to allocate mbuf burst (rc=%d)\n", n_alloc);
      usleep(10000);  // 10ms delay before retry
      continue;
    }

    // Fill each mbuf with IQ samples
    for (unsigned i = 0; i < batch_size; i++) {
      rte_mbuf* m = mbufs[i];
      
      // Validate mbuf
      if (!m) {
        std::fprintf(stderr, "[ue] ERROR: NULL mbuf at index %u\n", i);
        continue;
      }
      
      // Validate buf_addr BEFORE trying to access data
      if (!m->buf_addr) {
        std::fprintf(stderr, "[ue] ERROR: mbuf %u has NULL buf_addr!\n", i);
        std::fprintf(stderr, "[ue] ERROR: Pool '%s' did not properly allocate mbuf memory\n", pool_name);
        std::fprintf(stderr, "[ue] ERROR: This means rte_pktmbuf_pool_create() failed to set up mbufs correctly\n");
        std::fprintf(stderr, "[ue] ERROR: mbuf pool=%p, data_off=%u, buf_len=%u\n",
                    m->pool, m->data_off, m->buf_len);
        
        // Free all mbufs and exit - pool is broken
        for (unsigned j = i; j < batch_size; j++) {
          if (mbufs[j]) rte_pktmbuf_free(static_cast<rte_mbuf*>(mbufs[j]));
        }
        return 1;
      }
      
      // Get pointer to mbuf data area
      int16_t* iq_data = rte_pktmbuf_mtod(m, int16_t*);
      
      // Generate IQ samples (sine wave)
      for (size_t s = 0; s < samples_per_mbuf; s++) {
        double sample_val = std::sin(phase);
        int16_t i_sample = static_cast<int16_t>(amplitude * sample_val);
        int16_t q_sample = static_cast<int16_t>(amplitude * std::cos(phase));
        
        iq_data[s * 2] = i_sample;      // I component
        iq_data[s * 2 + 1] = q_sample;  // Q component
        
        phase += phase_increment;
        if (phase >= 2.0 * M_PI) {
          phase -= 2.0 * M_PI;
        }
      }
      
      // Set mbuf data length
      m->data_len = mbuf_data_size;
      m->pkt_len = mbuf_data_size;
    }

    // Send to first TX ring only (can't send same mbuf to multiple rings)
    // In production, you'd either:
    // 1. Clone mbufs for broadcast
    // 2. Use round-robin to different rings
    // 3. Send different data to each ring
    rte_ring* ring = tx_rings[0];  // Use first ring
    
    // Enqueue the burst of mbufs
    unsigned n_sent = rte_ring_enqueue_burst(ring, reinterpret_cast<void**>(mbufs), 
                                              batch_size, nullptr);
    
    if (n_sent < batch_size) {
      std::fprintf(stderr, "[ue] WARNING: Ring full, only sent %u/%u mbufs\n",
                  n_sent, batch_size);
      
      // Free unsent mbufs
      for (unsigned i = n_sent; i < batch_size; i++) {
        rte_pktmbuf_free(mbufs[i]);
      }
    }
    
    if (n_sent > 0) {
      total_samples_sent += n_sent * samples_per_mbuf;
      total_bursts++;
      
      if (total_bursts % 100 == 0) {
        std::fprintf(stderr, "[ue] Sent %lu IQ samples in %lu bursts (ring: %zu)\n",
                    total_samples_sent, total_bursts, size_t(0));
      }
    }
    
    // Check for response packets from primary
    if (rx_ring && (total_bursts % 5 == 0)) {  // Check every 5th send
      const unsigned response_batch = 8;
      void* response_mbufs[response_batch];
      
      unsigned n_recv = rte_ring_dequeue_burst(rx_ring, response_mbufs, response_batch, nullptr);
      if (n_recv > 0) {
        response_bursts_received++;
        
        for (unsigned i = 0; i < n_recv; i++) {
          rte_mbuf* m = static_cast<rte_mbuf*>(response_mbufs[i]);
          
          if (m && m->buf_addr) {
            int16_t* data = rte_pktmbuf_mtod(m, int16_t*);
            uint16_t data_len = rte_pktmbuf_data_len(m);
            size_t num_values = data_len / sizeof(int16_t);
            total_responses_received += num_values;
            
            // Print first response for verification
            if (response_bursts_received == 1 && i == 0 && num_values >= 2) {
              std::fprintf(stderr, "[ue] Received first response from primary:\n");
              std::fprintf(stderr, "[ue]   burst_num=%d, sample_count_high=%d\n", 
                          data[0], data[1]);
            }
            
            if (response_bursts_received % 10 == 0 && i == 0) {
              std::fprintf(stderr, "[ue] Received %lu response values in %lu bursts from primary\n",
                          total_responses_received, response_bursts_received);
            }
          }
          
          rte_pktmbuf_free(m);
        }
      }
    }
    
    // Rate limiting: send ~1000 bursts/sec = 1ms per burst
    usleep(1000);  // 1ms
  }
  
  std::fprintf(stderr, "\n[ue] Total IQ samples sent: %lu\n", total_samples_sent);
  std::fprintf(stderr, "[ue] Total bursts sent: %lu\n", total_bursts);
  std::fprintf(stderr, "[ue] Total response values received: %lu\n", total_responses_received);
  std::fprintf(stderr, "[ue] Total response bursts received: %lu\n", response_bursts_received);

  std::fprintf(stderr, "\n[ue] Shutting down...\n");
  std::fprintf(stderr, "[ue] Test completed successfully.\n");
  
  return 0;
}
