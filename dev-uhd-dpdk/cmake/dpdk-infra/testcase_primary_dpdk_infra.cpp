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
    std::fprintf(stderr, "Usage: %s <config.yaml> [mode]\n", argv[0]);
    std::fprintf(stderr, "  mode: rx (default) or tx\n");
    std::fprintf(stderr, "Example: %s conf/configurations-ue.yaml\n", argv[0]);
    std::fprintf(stderr, "Example: %s conf/configurations-ue.yaml tx\n", argv[0]);
    return 2;
  }

  std::string cfg_path = argv[1];
  std::string mode = (argc >= 3) ? argv[2] : "rx";
  
  std::fprintf(stderr, "[primary-ue] Loading config from: %s\n", cfg_path.c_str());
  std::fprintf(stderr, "[primary-ue] Mode: %s\n", mode.c_str());

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
  std::fprintf(stderr, "[primary-ue] Press Ctrl+C to shutdown gracefully...\n\n");

  if (mode == "tx") {
    // TX MODE: Send packets to secondary via RX rings
    if (rx_rings.empty() || pools.empty()) {
      std::fprintf(stderr, "[primary-ue] ERROR: No RX rings or pools available for TX mode\n");
      return 1;
    }
    
    std::fprintf(stderr, "[primary-ue] TX MODE: Sending 60 bursts to secondary...\n\n");
    
    rte_ring* rx_ring = rx_rings[0];
    rte_mempool* pool = pools[0];
    const uint64_t max_bursts = 60;
    uint64_t total_sent = 0;
    
    for (uint64_t burst = 1; burst <= max_bursts && !g_shutdown_requested.load(); burst++) {
      rte_mbuf* m = rte_pktmbuf_alloc(pool);
      if (!m) {
        std::fprintf(stderr, "[primary-ue] ERROR: Failed to allocate mbuf\n");
        break;
      }
      
      // Fill with test pattern
      int16_t* data = rte_pktmbuf_mtod(m, int16_t*);
      for (int i = 0; i < 512; i++) {
        data[i*2] = (int16_t)(burst * 100 + i);  // I
        data[i*2+1] = (int16_t)(burst * 100 + i + 1);  // Q
      }
      m->data_len = 2048;  // 512 IQ samples
      m->pkt_len = 2048;
      
      // Send to RX ring
      unsigned n = rte_ring_enqueue_burst(rx_ring, reinterpret_cast<void**>(&m), 1, nullptr);
      if (n > 0) {
        total_sent++;
        if (burst <= 3 || burst % 20 == 0) {
          std::fprintf(stderr, "[primary-ue] Sent burst %lu\n", burst);
        }
      } else {
        rte_pktmbuf_free(m);
        std::fprintf(stderr, "[primary-ue] WARNING: Ring full, failed to send burst %lu\n", burst);
      }
      
      usleep(1000);  // 1ms delay between bursts
    }
    
    std::fprintf(stderr, "\n========================================\n");
    std::fprintf(stderr, "[primary-ue] TX MODE COMPLETE\n");
    std::fprintf(stderr, "Total bursts sent: %lu\n", total_sent);
    std::fprintf(stderr, "========================================\n");
    
  } else {
    // RX MODE: Receive packets from secondary via TX rings (default)
    if (tx_rings.empty()) {
      std::fprintf(stderr, "[primary-ue] ERROR: No TX rings available\n");
      return 1;
    }

    std::fprintf(stderr, "[primary-ue] RX MODE: Monitoring %zu TX ring(s) for incoming IQ samples...\n", 
                 tx_rings.size());
    std::fprintf(stderr, "[primary-ue] Will receive up to 60 bursts then exit.\n\n");
  
  // Receive loop - count received packets and prepare to respond
  uint64_t total_samples_received = 0;
  uint64_t total_bursts_received = 0;
  const uint64_t max_bursts_to_receive = 60;
    while (!g_shutdown_requested.load() && total_bursts_received < max_bursts_to_receive) {
      // Check each TX ring for data from secondary
      for (size_t ring_idx = 0; ring_idx < tx_rings.size(); ring_idx++) {
      rte_ring* ring = tx_rings[ring_idx];
      const unsigned batch_size = 32;
      void* mbufs[batch_size];
      
      // Try to dequeue a burst of mbufs
      unsigned n = rte_ring_dequeue_burst(ring, mbufs, batch_size, nullptr);
      if (n > 0) {
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
        
        if (total_bursts_received <= 3 || total_bursts_received % 20 == 0) {
          std::fprintf(stderr, "[primary-ue] Received burst %lu (%lu IQ samples total)\n",
                      total_bursts_received, total_samples_received);
        }
        
        // Exit early if we hit the limit
        if (total_bursts_received >= max_bursts_to_receive) {
          break;
        }
        }
      }
      
      // Small delay to avoid busy-wait
      usleep(100); // 0.1ms
    }
    
    std::fprintf(stderr, "\n========================================\n");
    std::fprintf(stderr, "[primary-ue] VERIFICATION COMPLETE\n");
    std::fprintf(stderr, "========================================\n");
    std::fprintf(stderr, "Total IQ samples received: %lu\n", total_samples_received);
    std::fprintf(stderr, "Total bursts received: %lu\n", total_bursts_received);
    
    // Verify we received 60 packets
    if (total_bursts_received == 60) {
      std::fprintf(stderr, "[primary-ue] ✓ Verified: Received expected 60 packets\n");
    } else {
      std::fprintf(stderr, "[primary-ue] ✗ WARNING: Expected 60 packets but received %lu\n", 
                  total_bursts_received);
    }
    std::fprintf(stderr, "========================================\n\n");
    
    // Now send 65 packets back to secondary via RX rings
    if (rx_rings.empty() || pools.empty()) {
      std::fprintf(stderr, "[primary-ue] ERROR: No RX rings or pools available to send response\n");
      return 1;
    }
    
    std::fprintf(stderr, "[primary-ue] Sending 65 response packets to secondary...\n\n");
    
    rte_ring* rx_ring = rx_rings[0];
    rte_mempool* pool = pools[0];
    const uint64_t response_bursts = 65;
    uint64_t total_sent = 0;
    
    for (uint64_t burst = 1; burst <= response_bursts && !g_shutdown_requested.load(); burst++) {
      rte_mbuf* m = rte_pktmbuf_alloc(pool);
      if (!m) {
        std::fprintf(stderr, "[primary-ue] ERROR: Failed to allocate mbuf for response\n");
        break;
      }
      
      // Fill with test pattern (response data)
      int16_t* data = rte_pktmbuf_mtod(m, int16_t*);
      for (int i = 0; i < 512; i++) {
        data[i*2] = (int16_t)(burst * 200 + i);  // I (different pattern from received)
        data[i*2+1] = (int16_t)(burst * 200 + i + 1);  // Q
      }
      m->data_len = 2048;  // 512 IQ samples
      m->pkt_len = 2048;
      
      // Send to RX ring (which secondary reads from)
      unsigned n = rte_ring_enqueue_burst(rx_ring, reinterpret_cast<void**>(&m), 1, nullptr);
      if (n > 0) {
        total_sent++;
        if (burst <= 3 || burst % 20 == 0) {
          std::fprintf(stderr, "[primary-ue] Sent response burst %lu\n", burst);
        }
      } else {
        rte_pktmbuf_free(m);
        std::fprintf(stderr, "[primary-ue] WARNING: Ring full, failed to send response burst %lu\n", burst);
      }
      
      usleep(1000);  // 1ms delay between bursts
    }
    
    std::fprintf(stderr, "\n========================================\n");
    std::fprintf(stderr, "[primary-ue] RESPONSE SENDING COMPLETE\n");
    std::fprintf(stderr, "Total response bursts sent: %lu\n", total_sent);
    std::fprintf(stderr, "========================================\n");
  }

  std::fprintf(stderr, "\n[primary-ue] Shutting down...\n");
  std::fprintf(stderr, "[primary-ue] Test completed successfully.\n");
  
  return 0;
}
