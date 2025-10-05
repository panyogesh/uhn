/**
 * @file testcase_interconnect_dpdk_infra.cpp
 * @brief Test case for INTERCONNECT_RINGS between GNB and UE
 * 
 * This testcase demonstrates INTERCONNECT_RINGS functionality:
 * - GNB mode: Creates INTERCONNECT_RINGS pools and rings, switches traffic from gnb_tx_ch1 to pg_to_pu
 * - UE mode: Checks if INTERCONNECT_RINGS are created and forwards traffic from ue_tx_ch1 to pu_to_pg
 */

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
  std::fprintf(stderr, "\n[interconnect] caught signal %d, requesting shutdown...\n", signum);
  g_shutdown_requested.store(true);
}

static void setup_signal_handlers() {
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);
}

// Determine device type from config file path
bool is_gnb_config(const std::string& cfg_path) {
  return cfg_path.find("gnb") != std::string::npos;
}

int main(int argc, char** argv) {
  std::fprintf(stderr, "========================================\n");
  std::fprintf(stderr, "FlexSDR INTERCONNECT_RINGS Test\n");
  std::fprintf(stderr, "PID: %d\n", getpid());
  std::fprintf(stderr, "========================================\n\n");

  if (argc < 2) {
    std::fprintf(stderr, "Usage: %s <config.yaml>\n", argv[0]);
    std::fprintf(stderr, "Example (GNB): %s conf/configurations-gnb.yaml\n", argv[0]);
    std::fprintf(stderr, "Example (UE):  %s conf/configurations-ue.yaml\n", argv[0]);
    std::fprintf(stderr, "\nNOTE: For UE test, GNB process must be running first!\n");
    return 2;
  }

  std::string cfg_path = argv[1];
  bool is_gnb = is_gnb_config(cfg_path);
  
  std::fprintf(stderr, "[interconnect] Loading config from: %s\n", cfg_path.c_str());
  std::fprintf(stderr, "[interconnect] Device type: %s\n", is_gnb ? "GNB" : "UE");

  setup_signal_handlers();

  // Enable verbose DPDK logging
  setenv("RTE_LOG_LEVEL", "8", 1);

  // Load config for EAL initialization
  flexsdr::conf::PrimaryConfig cfg;
  int cfg_rc = flexsdr::conf::load_from_yaml(cfg_path.c_str(), cfg);
  if (cfg_rc) {
    std::fprintf(stderr, "[interconnect] ERROR: Failed to load config (rc=%d)\n", cfg_rc);
    return 1;
  }

  // Initialize DPDK EAL as primary process
  std::fprintf(stderr, "[interconnect] Initializing DPDK EAL...\n");
  flexsdr::EalBootstrap eal(cfg, is_gnb ? "flexsdr-primary-gnb" : "flexsdr-primary-ue");
  eal.build_args({"--proc-type=primary"});
  
  std::fprintf(stderr, "[interconnect] EAL arguments: %s\n", eal.args_as_cmdline().c_str());
  
  int eal_rc = eal.init();
  if (eal_rc < 0) {
    std::fprintf(stderr, "[interconnect] ERROR: EAL initialization failed (rc=%d)\n", eal_rc);
    return 1;
  }
  std::fprintf(stderr, "[interconnect] EAL initialized successfully (consumed %d args)\n", eal_rc);

  // Create FlexSDRPrimary instance (after EAL is initialized)
  flexsdr::FlexSDRPrimary primary_app(cfg_path);
  std::fprintf(stderr, "[interconnect] FlexSDRPrimary constructed\n");

  // Initialize DPDK resources (pools, rings, interconnect)
  std::fprintf(stderr, "[interconnect] Initializing resources (pools, rings, interconnect)...\n");
  int rc = primary_app.init_resources();
  if (rc) {
    std::fprintf(stderr, "[interconnect] ERROR: Resource initialization failed (rc=%d)\n", rc);
    return 1;
  }

  // Get actual resources created
  const auto& pools = primary_app.pools();
  const auto& tx_rings = primary_app.tx_rings();
  const auto& rx_rings = primary_app.rx_rings();
  const auto& ic_tx_rings = primary_app.ic_tx_rings();
  const auto& ic_rx_rings = primary_app.ic_rx_rings();
  
  std::fprintf(stderr, "\n[interconnect] ✓ All resources initialized successfully!\n");
  std::fprintf(stderr, "[interconnect] Resources created:\n");
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
  std::fprintf(stderr, "  - %zu Interconnect TX ring(s):\n", ic_tx_rings.size());
  for (const auto& ring : ic_tx_rings) {
    std::fprintf(stderr, "    * %s (size=%u)\n", ring->name, rte_ring_get_size(ring));
  }
  std::fprintf(stderr, "  - %zu Interconnect RX ring(s):\n", ic_rx_rings.size());
  for (const auto& ring : ic_rx_rings) {
    std::fprintf(stderr, "    * %s (size=%u)\n", ring->name, rte_ring_get_size(ring));
  }

  if (is_gnb) {
    // ========== GNB MODE ==========
    std::fprintf(stderr, "\n========================================\n");
    std::fprintf(stderr, "GNB MODE: Traffic Switching Test\n");
    std::fprintf(stderr, "========================================\n");
    
    // Verify interconnect rings were created
    if (ic_tx_rings.empty()) {
      std::fprintf(stderr, "[interconnect] ERROR: GNB should have created interconnect TX rings!\n");
      return 1;
    }
    
    // Find gnb_tx_ch1 and pg_to_pu rings
    rte_ring* gnb_tx_ch1 = nullptr;
    for (const auto& ring : tx_rings) {
      if (std::string(ring->name) == "gnb_tx_ch1") {
        gnb_tx_ch1 = ring;
        break;
      }
    }
    
    rte_ring* pg_to_pu = nullptr;
    for (const auto& ring : ic_tx_rings) {
      if (std::string(ring->name) == "pg_to_pu") {
        pg_to_pu = ring;
        break;
      }
    }
    
    if (!gnb_tx_ch1) {
      std::fprintf(stderr, "[interconnect] ERROR: gnb_tx_ch1 ring not found!\n");
      return 1;
    }
    
    if (!pg_to_pu) {
      std::fprintf(stderr, "[interconnect] ERROR: pg_to_pu interconnect ring not found!\n");
      return 1;
    }
    
    std::fprintf(stderr, "[interconnect] Found gnb_tx_ch1: %s\n", gnb_tx_ch1->name);
    std::fprintf(stderr, "[interconnect] Found pg_to_pu: %s\n", pg_to_pu->name);
    
    // Get memory pool for allocating mbufs
    if (pools.empty()) {
      std::fprintf(stderr, "[interconnect] ERROR: No memory pools available\n");
      return 1;
    }
    rte_mempool* pool = pools[0];
    
    std::fprintf(stderr, "\n[interconnect] Starting traffic switch from gnb_tx_ch1 to pg_to_pu...\n");
    std::fprintf(stderr, "[interconnect] Sending 50 bursts...\n\n");
    
    uint64_t total_sent = 0;
    const uint64_t max_bursts = 50;
    
    for (uint64_t burst = 1; burst <= max_bursts && !g_shutdown_requested.load(); burst++) {
      // Allocate mbuf
      rte_mbuf* m = rte_pktmbuf_alloc(pool);
      if (!m) {
        std::fprintf(stderr, "[interconnect] ERROR: Failed to allocate mbuf\n");
        break;
      }
      
      // Fill with test pattern (IQ samples)
      int16_t* data = rte_pktmbuf_mtod(m, int16_t*);
      for (int i = 0; i < 512; i++) {
        data[i*2] = (int16_t)(burst * 100 + i);      // I component
        data[i*2+1] = (int16_t)(burst * 100 + i + 1); // Q component
      }
      m->data_len = 2048;  // 512 IQ samples
      m->pkt_len = 2048;
      
      // First enqueue to gnb_tx_ch1 (simulating transmission)
      unsigned n = rte_ring_enqueue_burst(gnb_tx_ch1, reinterpret_cast<void**>(&m), 1, nullptr);
      if (n == 0) {
        std::fprintf(stderr, "[interconnect] WARNING: gnb_tx_ch1 full, burst %lu\n", burst);
        rte_pktmbuf_free(m);
        continue;
      }
      
      // Immediately dequeue from gnb_tx_ch1 (simulating traffic switch)
      void* mbuf_ptr;
      n = rte_ring_dequeue_burst(gnb_tx_ch1, &mbuf_ptr, 1, nullptr);
      if (n == 0) {
        std::fprintf(stderr, "[interconnect] ERROR: Failed to dequeue from gnb_tx_ch1\n");
        continue;
      }
      
      rte_mbuf* switched_m = static_cast<rte_mbuf*>(mbuf_ptr);
      
      // Enqueue to pg_to_pu (interconnect ring)
      n = rte_ring_enqueue_burst(pg_to_pu, reinterpret_cast<void**>(&switched_m), 1, nullptr);
      if (n > 0) {
        total_sent++;
        if (burst <= 3 || burst % 10 == 0) {
          std::fprintf(stderr, "[interconnect] Switched burst %lu: gnb_tx_ch1 -> pg_to_pu\n", burst);
        }
      } else {
        rte_pktmbuf_free(switched_m);
        std::fprintf(stderr, "[interconnect] WARNING: pg_to_pu full, burst %lu\n", burst);
      }
      
      usleep(1000);  // 1ms delay between bursts
    }
    
    std::fprintf(stderr, "\n========================================\n");
    std::fprintf(stderr, "GNB MODE TEST COMPLETE\n");
    std::fprintf(stderr, "========================================\n");
    std::fprintf(stderr, "Total bursts switched: %lu\n", total_sent);
    std::fprintf(stderr, "Interconnect rings ready for UE to consume\n");
    std::fprintf(stderr, "========================================\n");
    
    std::fprintf(stderr, "\n[interconnect] Press Ctrl+C to shutdown...\n");
    std::fprintf(stderr, "[interconnect] (UE process can now connect and read from pg_to_pu)\n\n");
    
    // Keep running so UE can connect
    while (!g_shutdown_requested.load()) {
      sleep(1);
    }
    
  } else {
    // ========== UE MODE ==========
    std::fprintf(stderr, "\n========================================\n");
    std::fprintf(stderr, "UE MODE: Traffic Forwarding Test\n");
    std::fprintf(stderr, "========================================\n");
    
    // Verify interconnect rings were found (not created)
    if (ic_rx_rings.empty() || ic_tx_rings.empty()) {
      std::fprintf(stderr, "[interconnect] ERROR: UE should have found interconnect rings!\n");
      std::fprintf(stderr, "[interconnect] Make sure GNB process is running first!\n");
      return 1;
    }
    
    // Find ue_tx_ch1, pg_to_pu (IC RX), and pu_to_pg (IC TX) rings
    rte_ring* ue_tx_ch1 = nullptr;
    for (const auto& ring : tx_rings) {
      if (std::string(ring->name) == "ue_tx_ch1") {
        ue_tx_ch1 = ring;
        break;
      }
    }
    
    rte_ring* pg_to_pu = nullptr;
    for (const auto& ring : ic_rx_rings) {
      if (std::string(ring->name) == "pg_to_pu") {
        pg_to_pu = ring;
        break;
      }
    }
    
    rte_ring* pu_to_pg = nullptr;
    for (const auto& ring : ic_tx_rings) {
      if (std::string(ring->name) == "pu_to_pg") {
        pu_to_pg = ring;
        break;
      }
    }
    
    if (!ue_tx_ch1) {
      std::fprintf(stderr, "[interconnect] ERROR: ue_tx_ch1 ring not found!\n");
      return 1;
    }
    
    if (!pg_to_pu) {
      std::fprintf(stderr, "[interconnect] ERROR: pg_to_pu interconnect ring not found!\n");
      return 1;
    }
    
    if (!pu_to_pg) {
      std::fprintf(stderr, "[interconnect] ERROR: pu_to_pg interconnect ring not found!\n");
      return 1;
    }
    
    std::fprintf(stderr, "[interconnect] ✓ Found ue_tx_ch1: %s\n", ue_tx_ch1->name);
    std::fprintf(stderr, "[interconnect] ✓ Found pg_to_pu (IC RX): %s\n", pg_to_pu->name);
    std::fprintf(stderr, "[interconnect] ✓ Found pu_to_pg (IC TX): %s\n", pu_to_pg->name);
    
    // Get memory pool for allocating mbufs
    if (pools.empty()) {
      std::fprintf(stderr, "[interconnect] ERROR: No memory pools available\n");
      return 1;
    }
    rte_mempool* pool = pools[0];
    
    std::fprintf(stderr, "\n[interconnect] Starting traffic forwarding...\n");
    std::fprintf(stderr, "[interconnect] 1. Receiving from pg_to_pu (GNB -> UE)\n");
    std::fprintf(stderr, "[interconnect] 2. Forwarding via ue_tx_ch1 -> pu_to_pg (UE -> GNB)\n\n");
    
    uint64_t total_received = 0;
    uint64_t total_forwarded = 0;
    const uint64_t max_iterations = 100;
    uint64_t iteration = 0;
    
    while (!g_shutdown_requested.load() && iteration < max_iterations) {
      iteration++;
      
      // Step 1: Receive from pg_to_pu (data from GNB)
      const unsigned batch_size = 8;
      void* mbufs[batch_size];
      
      unsigned n = rte_ring_dequeue_burst(pg_to_pu, mbufs, batch_size, nullptr);
      if (n > 0) {
        total_received += n;
        
        for (unsigned i = 0; i < n; i++) {
          rte_mbuf* m = static_cast<rte_mbuf*>(mbufs[i]);
          
          if (!m || !m->buf_addr) {
            std::fprintf(stderr, "[interconnect] ERROR: Invalid mbuf received\n");
            if (m) rte_pktmbuf_free(m);
            continue;
          }
          
          // Verify data (first burst only)
          if (total_received <= n && i == 0) {
            int16_t* data = rte_pktmbuf_mtod(m, int16_t*);
            std::fprintf(stderr, "[interconnect] Received from GNB: I=%d, Q=%d\n", 
                        data[0], data[1]);
          }
          
          // Step 2a: Enqueue to ue_tx_ch1 (simulating UE transmission)
          unsigned sent = rte_ring_enqueue_burst(ue_tx_ch1, reinterpret_cast<void**>(&m), 1, nullptr);
          if (sent == 0) {
            std::fprintf(stderr, "[interconnect] WARNING: ue_tx_ch1 full\n");
            rte_pktmbuf_free(m);
            continue;
          }
          
          // Step 2b: Dequeue from ue_tx_ch1 (traffic switch)
          void* mbuf_ptr;
          unsigned dequeued = rte_ring_dequeue_burst(ue_tx_ch1, &mbuf_ptr, 1, nullptr);
          if (dequeued == 0) {
            std::fprintf(stderr, "[interconnect] ERROR: Failed to dequeue from ue_tx_ch1\n");
            continue;
          }
          
          rte_mbuf* switched_m = static_cast<rte_mbuf*>(mbuf_ptr);
          
          // Step 2c: Forward to pu_to_pg (back to GNB)
          sent = rte_ring_enqueue_burst(pu_to_pg, reinterpret_cast<void**>(&switched_m), 1, nullptr);
          if (sent > 0) {
            total_forwarded++;
          } else {
            rte_pktmbuf_free(switched_m);
            std::fprintf(stderr, "[interconnect] WARNING: pu_to_pg full\n");
          }
        }
        
        if (total_received % 10 == 0 || total_received <= 3) {
          std::fprintf(stderr, "[interconnect] Progress: received=%lu, forwarded=%lu\n",
                      total_received, total_forwarded);
        }
      }
      
      usleep(10000);  // 10ms delay between iterations
    }
    
    std::fprintf(stderr, "\n========================================\n");
    std::fprintf(stderr, "UE MODE TEST COMPLETE\n");
    std::fprintf(stderr, "========================================\n");
    std::fprintf(stderr, "Total bursts received from GNB: %lu\n", total_received);
    std::fprintf(stderr, "Total bursts forwarded to GNB: %lu\n", total_forwarded);
    std::fprintf(stderr, "========================================\n");
  }

  std::fprintf(stderr, "\n[interconnect] Shutting down...\n");
  std::fprintf(stderr, "[interconnect] Test completed successfully.\n");
  
  return 0;
}
