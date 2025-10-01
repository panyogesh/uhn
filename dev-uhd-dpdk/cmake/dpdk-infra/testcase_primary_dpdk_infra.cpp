#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <csignal>
#include <atomic>
#include <unistd.h>

#include <rte_mbuf.h>
#include <rte_ring.h>

#include "conf/config_params.hpp"
#include "transport/flexsdr_primary.hpp"
#include "transport/eal_bootstrap.hpp"

// Global flag for graceful shutdown
static std::atomic<bool> g_shutdown_requested{false};

static void signal_handler(int signum) {
  std::fprintf(stderr, "\n[primary-ue] caught signal %d, requesting shutdown...\n", signum);
  g_shutdown_requested.store(true);
}

static void setup_signal_handlers() {
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);
}

int main(int argc, char** argv) {
  std::fprintf(stderr, "========================================\n");
  std::fprintf(stderr, "FlexSDR Primary-UE DPDK Infrastructure Test\n");
  std::fprintf(stderr, "PID: %d\n", getpid());
  std::fprintf(stderr, "========================================\n\n");

  if (argc < 2) {
    std::fprintf(stderr, "Usage: %s <config.yaml>\n", argv[0]);
    std::fprintf(stderr, "Example: %s conf/configurations-ue.yaml\n", argv[0]);
    return 2;
  }

  std::string cfg_path = argv[1];
  std::fprintf(stderr, "[primary-ue] Loading config from: %s\n", cfg_path.c_str());

  setup_signal_handlers();

  // Enable verbose DPDK logging
  setenv("RTE_LOG_LEVEL", "8", 1);

  // Load config for EAL initialization
  flexsdr::conf::PrimaryConfig cfg;
  int cfg_rc = flexsdr::conf::load_from_yaml(cfg_path.c_str(), cfg);
  if (cfg_rc) {
    std::fprintf(stderr, "[primary-ue] ERROR: Failed to load config (rc=%d)\n", cfg_rc);
    return 1;
  }

  // Initialize DPDK EAL as primary process
  std::fprintf(stderr, "[primary-ue] Initializing DPDK EAL...\n");
  flexsdr::EalBootstrap eal(cfg, "flexsdr-primary-ue");
  eal.build_args({"--proc-type=primary"});
  
  std::fprintf(stderr, "[primary-ue] EAL arguments: %s\n", eal.args_as_cmdline().c_str());
  
  int eal_rc = eal.init();
  if (eal_rc < 0) {
    std::fprintf(stderr, "[primary-ue] ERROR: EAL initialization failed (rc=%d)\n", eal_rc);
    return 1;
  }
  std::fprintf(stderr, "[primary-ue] EAL initialized successfully (consumed %d args)\n", eal_rc);

  // Create FlexSDRPrimary instance (after EAL is initialized)
  flexsdr::FlexSDRPrimary primary_app(cfg_path);
  std::fprintf(stderr, "[primary-ue] FlexSDRPrimary constructed\n");

  // Initialize DPDK resources (pools, rings)
  std::fprintf(stderr, "[primary-ue] Initializing resources (pools, rings)...\n");
  int rc = primary_app.init_resources();
  if (rc) {
    std::fprintf(stderr, "[primary-ue] ERROR: Resource initialization failed (rc=%d)\n", rc);
    return 1;
  }

  // Get actual resources created
  const auto& pools = primary_app.pools();
  const auto& tx_rings = primary_app.tx_rings();
  const auto& rx_rings = primary_app.rx_rings();
  
  std::fprintf(stderr, "\n[primary-ue] ✓ All resources initialized successfully!\n");
  std::fprintf(stderr, "[primary-ue] Resources created:\n");
  std::fprintf(stderr, "  - %zu Memory pool(s):\n", pools.size());
  for (const auto& pool : pools) {
    std::fprintf(stderr, "    * %s\n", pool->name);
  }
  std::fprintf(stderr, "  - %zu TX ring(s):\n", tx_rings.size());
  for (const auto& ring : tx_rings) {
    std::fprintf(stderr, "    * %s (size=%u)\n", ring->name, rte_ring_get_size(ring));
  }
  std::fprintf(stderr, "  - %zu RX ring(s):\n", rx_rings.size());
  for (const auto& ring : rx_rings) {
    std::fprintf(stderr, "    * %s (size=%u)\n", ring->name, rte_ring_get_size(ring));
  }
  std::fprintf(stderr, "\n[primary-ue] Ready for secondary processes to attach.\n");
  std::fprintf(stderr, "[primary-ue] Receiving IQ samples from secondary...\n");
  std::fprintf(stderr, "[primary-ue] Press Ctrl+C to shutdown gracefully...\n\n");

  
  if (tx_rings.empty()) {
    std::fprintf(stderr, "[primary-ue] ERROR: No TX rings available\n");
    return 1;
  }

  std::fprintf(stderr, "[primary-ue] Monitoring %zu TX ring(s) for incoming IQ samples...\n\n", 
               tx_rings.size());

  // Get pool for sending response packets
  rte_mempool* send_pool = pools.empty() ? nullptr : pools[0];
  
  // Receive loop - dequeue IQ samples from rings and send responses
  uint64_t total_samples_received = 0;
  uint64_t total_bursts_received = 0;
  uint64_t total_samples_sent = 0;
  uint64_t total_bursts_sent = 0;
  
  while (!g_shutdown_requested.load()) {
    bool received_any = false;
    
    // Check each TX ring for data from secondary
    for (size_t ring_idx = 0; ring_idx < tx_rings.size(); ring_idx++) {
      rte_ring* ring = tx_rings[ring_idx];
      const unsigned batch_size = 32;
      void* mbufs[batch_size];
      
      // Try to dequeue a burst of mbufs
      unsigned n = rte_ring_dequeue_burst(ring, mbufs, batch_size, nullptr);
      if (n > 0) {
        received_any = true;
        total_bursts_received++;
        
        // Process each mbuf
        for (unsigned i = 0; i < n; i++) {
          rte_mbuf* m = static_cast<rte_mbuf*>(mbufs[i]);
          
          // Validate mbuf
          if (!m) {
            std::fprintf(stderr, "[primary-ue] ERROR: NULL mbuf at index %u\n", i);
            continue;
          }
          
          // Validate buf_addr BEFORE trying to access data
          if (!m->buf_addr) {
            std::fprintf(stderr, "[primary-ue] ERROR: mbuf %u has NULL buf_addr\n", i);
            std::fprintf(stderr, "[primary-ue] ERROR: Pool may not be properly initialized\n");
            std::fprintf(stderr, "[primary-ue] ERROR: mbuf pool=%p, data_off=%u, data_len=%u\n",
                        m->pool, m->data_off, m->data_len);
            rte_pktmbuf_free(m);
            continue;
          }
          
          // Get IQ sample data from mbuf
          uint8_t* data = rte_pktmbuf_mtod(m, uint8_t*);
          
          uint16_t data_len = rte_pktmbuf_data_len(m);
          
          // IQ samples are int16_t pairs (I, Q)
          size_t num_samples = data_len / sizeof(int16_t) / 2;
          total_samples_received += num_samples;
          
          // Print first few samples from first burst for verification
          if (total_bursts_received == 1 && i == 0 && num_samples >= 4) {
            int16_t* iq = reinterpret_cast<int16_t*>(data);
            std::fprintf(stderr, "[primary-ue] Ring %zu: First mbuf contains %zu IQ samples\n",
                        ring_idx, num_samples);
            std::fprintf(stderr, "[primary-ue] Ring %zu: First 4 samples: ", ring_idx);
            std::fprintf(stderr, "(%d,%d) ", iq[0], iq[1]);
            std::fprintf(stderr, "(%d,%d) ", iq[2], iq[3]);
            std::fprintf(stderr, "(%d,%d) ", iq[4], iq[5]);
            std::fprintf(stderr, "(%d,%d) ", iq[6], iq[7]);
            std::fprintf(stderr, "\n");
          }
          
          // Free mbuf back to pool
          rte_pktmbuf_free(m);
        }
        
        if (total_bursts_received % 100 == 0) {
          std::fprintf(stderr, "[primary-ue] Received %lu IQ samples in %lu bursts\n",
                      total_samples_received, total_bursts_received);
        }
      }
    }
    
    // Send response packets back to secondary via RX rings (every 10th receive)
    if (received_any && send_pool && !rx_rings.empty() && (total_bursts_received % 10 == 0)) {
      rte_ring* rx_ring = rx_rings[0];
      const unsigned response_batch = 4;  // Send 4 response mbufs
      rte_mbuf* response_mbufs[response_batch];
      
      // Allocate response mbufs
      if (rte_pktmbuf_alloc_bulk(send_pool, response_mbufs, response_batch) == 0) {
        // Fill with simple response pattern
        for (unsigned i = 0; i < response_batch; i++) {
          rte_mbuf* m = response_mbufs[i];
          int16_t* data = rte_pktmbuf_mtod(m, int16_t*);
          
          // Simple pattern: [burst_num, sample_count, ...]
          data[0] = static_cast<int16_t>(total_bursts_sent & 0xFFFF);
          data[1] = static_cast<int16_t>((total_samples_received >> 16) & 0xFFFF);
          for (int j = 2; j < 64; j++) {
            data[j] = static_cast<int16_t>(j * 100);
          }
          
          m->data_len = 128;  // 64 int16_t values
          m->pkt_len = 128;
        }
        
        // Send to RX ring
        unsigned n_sent = rte_ring_enqueue_burst(rx_ring, reinterpret_cast<void**>(response_mbufs),
                                                  response_batch, nullptr);
        
        if (n_sent > 0) {
          total_bursts_sent++;
          total_samples_sent += n_sent * 64;
          
          if (total_bursts_sent % 10 == 0) {
            std::fprintf(stderr, "[primary-ue] Sent %lu response samples in %lu bursts\n",
                        total_samples_sent, total_bursts_sent);
          }
        }
        
        // Free unsent mbufs
        for (unsigned i = n_sent; i < response_batch; i++) {
          rte_pktmbuf_free(response_mbufs[i]);
        }
      }
    }
    
    // Small delay if no data received to avoid busy-wait
    if (!received_any) {
      usleep(1000); // 1ms
    }
  }
  
  std::fprintf(stderr, "\n[primary-ue] Total IQ samples received: %lu\n", total_samples_received);
  std::fprintf(stderr, "[primary-ue] Total bursts received: %lu\n", total_bursts_received);
  std::fprintf(stderr, "[primary-ue] Total response samples sent: %lu\n", total_samples_sent);
  std::fprintf(stderr, "[primary-ue] Total response bursts sent: %lu\n", total_bursts_sent);

  std::fprintf(stderr, "\n[primary-ue] Shutting down...\n");
  std::fprintf(stderr, "[primary-ue] Test completed successfully.\n");
  
  return 0;
}
