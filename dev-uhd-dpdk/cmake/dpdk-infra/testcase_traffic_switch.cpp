/**
 * @file testcase_traffic_switch.cpp
 * @brief Test case for traffic switching between GNB and UE rings
 * 
 * This testcase demonstrates traffic switching in a single primary process:
 * - Creates both GNB and UE rings (gnb_tx_ch1, ue_tx_ch1, gnb_inbound_ring, ue_inbound_ring)
 * - Switches traffic: gnb_tx_ch1 → ue_inbound_ring
 * - Switches traffic: ue_tx_ch1 → gnb_inbound_ring
 * 
 * This simulates the interconnect between GNB and UE without requiring separate processes.
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
  std::fprintf(stderr, "\n[traffic_switch] caught signal %d, requesting shutdown...\n", signum);
  g_shutdown_requested.store(true);
}

static void setup_signal_handlers() {
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);
}

int main(int argc, char** argv) {
  std::fprintf(stderr, "========================================\n");
  std::fprintf(stderr, "FlexSDR Traffic Switching Test\n");
  std::fprintf(stderr, "Single Primary Process\n");
  std::fprintf(stderr, "PID: %d\n", getpid());
  std::fprintf(stderr, "========================================\n\n");

  if (argc < 2) {
    std::fprintf(stderr, "Usage: %s <config.yaml>\n", argv[0]);
    std::fprintf(stderr, "Example: %s conf/configurations-unified.yaml\n", argv[0]);
    return 2;
  }

  std::string cfg_path = argv[1];
  std::fprintf(stderr, "[traffic_switch] Loading config from: %s\n", cfg_path.c_str());

  setup_signal_handlers();

  // Enable verbose DPDK logging
  setenv("RTE_LOG_LEVEL", "8", 1);

  // Load config for EAL initialization
  flexsdr::conf::PrimaryConfig cfg;
  int cfg_rc = flexsdr::conf::load_from_yaml(cfg_path.c_str(), cfg);
  if (cfg_rc) {
    std::fprintf(stderr, "[traffic_switch] ERROR: Failed to load config (rc=%d)\n", cfg_rc);
    return 1;
  }

  // Initialize DPDK EAL as primary process
  std::fprintf(stderr, "[traffic_switch] Initializing DPDK EAL...\n");
  flexsdr::EalBootstrap eal(cfg, "flexsdr-unified-primary");
  eal.build_args({"--proc-type=primary"});
  
  std::fprintf(stderr, "[traffic_switch] EAL arguments: %s\n", eal.args_as_cmdline().c_str());
  
  int eal_rc = eal.init();
  if (eal_rc < 0) {
    std::fprintf(stderr, "[traffic_switch] ERROR: EAL initialization failed (rc=%d)\n", eal_rc);
    return 1;
  }
  std::fprintf(stderr, "[traffic_switch] EAL initialized successfully (consumed %d args)\n", eal_rc);

  // Create FlexSDRPrimary instance (after EAL is initialized)
  flexsdr::FlexSDRPrimary primary_app(cfg_path);
  std::fprintf(stderr, "[traffic_switch] FlexSDRPrimary constructed\n");

  // Initialize DPDK resources (pools, rings)
  std::fprintf(stderr, "[traffic_switch] Initializing resources (pools, rings)...\n");
  int rc = primary_app.init_resources();
  if (rc) {
    std::fprintf(stderr, "[traffic_switch] ERROR: Resource initialization failed (rc=%d)\n", rc);
    return 1;
  }

  // Get actual resources created
  const auto& pools = primary_app.pools();
  const auto& tx_rings = primary_app.tx_rings();
  const auto& rx_rings = primary_app.rx_rings();
  
  std::fprintf(stderr, "\n[traffic_switch] ✓ All resources initialized successfully!\n");
  std::fprintf(stderr, "[traffic_switch] Resources created:\n");
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

  // Find required rings
  rte_ring* gnb_tx_ch1 = nullptr;
  rte_ring* ue_tx_ch1 = nullptr;
  rte_ring* gnb_inbound_ring = nullptr;
  rte_ring* ue_inbound_ring = nullptr;
  
  for (const auto& ring : tx_rings) {
    if (std::string(ring->name) == "gnb_tx_ch1") {
      gnb_tx_ch1 = ring;
    } else if (std::string(ring->name) == "ue_tx_ch1") {
      ue_tx_ch1 = ring;
    }
  }
  
  for (const auto& ring : rx_rings) {
    if (std::string(ring->name) == "gnb_inbound_ring") {
      gnb_inbound_ring = ring;
    } else if (std::string(ring->name) == "ue_inbound_ring") {
      ue_inbound_ring = ring;
    }
  }
  
  // Verify all rings exist
  if (!gnb_tx_ch1) {
    std::fprintf(stderr, "[traffic_switch] ERROR: gnb_tx_ch1 ring not found!\n");
    return 1;
  }
  if (!ue_tx_ch1) {
    std::fprintf(stderr, "[traffic_switch] ERROR: ue_tx_ch1 ring not found!\n");
    return 1;
  }
  if (!gnb_inbound_ring) {
    std::fprintf(stderr, "[traffic_switch] ERROR: gnb_inbound_ring not found!\n");
    return 1;
  }
  if (!ue_inbound_ring) {
    std::fprintf(stderr, "[traffic_switch] ERROR: ue_inbound_ring not found!\n");
    return 1;
  }
  
  std::fprintf(stderr, "\n[traffic_switch] ✓ All required rings found:\n");
  std::fprintf(stderr, "  - gnb_tx_ch1: %s\n", gnb_tx_ch1->name);
  std::fprintf(stderr, "  - ue_tx_ch1: %s\n", ue_tx_ch1->name);
  std::fprintf(stderr, "  - gnb_inbound_ring: %s\n", gnb_inbound_ring->name);
  std::fprintf(stderr, "  - ue_inbound_ring: %s\n", ue_inbound_ring->name);
  
  // Get memory pool for allocating mbufs
  if (pools.empty()) {
    std::fprintf(stderr, "[traffic_switch] ERROR: No memory pools available\n");
    return 1;
  }
  rte_mempool* pool = pools[0];
  
  std::fprintf(stderr, "\n========================================\n");
  std::fprintf(stderr, "Traffic Switcher Running\n");
  std::fprintf(stderr, "========================================\n");
  std::fprintf(stderr, "Waiting for traffic from secondary processes...\n");
  std::fprintf(stderr, "Traffic flow:\n");
  std::fprintf(stderr, "  1. GNB → UE: gnb_tx_ch1 → ue_inbound_ring\n");
  std::fprintf(stderr, "  2. UE → GNB: ue_tx_ch1 → gnb_inbound_ring\n");
  std::fprintf(stderr, "========================================\n");
  std::fprintf(stderr, "Ready for secondary-gnb and secondary-ue to connect.\n");
  std::fprintf(stderr, "Press Ctrl+C to shutdown...\n\n");
  
  uint64_t total_gnb_to_ue = 0;
  uint64_t total_ue_to_gnb = 0;
  uint64_t loop_count = 0;
  
  // Main traffic switching loop - runs continuously until interrupted
  while (!g_shutdown_requested.load()) {
    loop_count++;
    bool switched_traffic = false;
    
    // ===== Part 1: Switch GNB → UE traffic =====
    const unsigned batch_size = 32;
    void* gnb_mbufs[batch_size];
    
    // Dequeue from gnb_tx_ch1 (traffic from secondary GNB)
    unsigned n = rte_ring_dequeue_burst(gnb_tx_ch1, gnb_mbufs, batch_size, nullptr);
    if (n > 0) {
      // Forward to ue_inbound_ring (deliver to secondary UE)
      unsigned enqueued = rte_ring_enqueue_burst(ue_inbound_ring, gnb_mbufs, n, nullptr);
      if (enqueued > 0) {
        total_gnb_to_ue += enqueued;
        switched_traffic = true;
        
        // Log first packet in batch
        if (total_gnb_to_ue <= 3 || (total_gnb_to_ue % 100 == 0)) {
          rte_mbuf* m = static_cast<rte_mbuf*>(gnb_mbufs[0]);
          int16_t* data = rte_pktmbuf_mtod(m, int16_t*);
          std::fprintf(stderr, "[traffic_switch] GNB→UE: switched %u packets (total=%lu) | Sample: I=%d, Q=%d\n",
                      enqueued, total_gnb_to_ue, data[0], data[1]);
        }
      }
      
      // Free any packets that couldn't be enqueued
      for (unsigned i = enqueued; i < n; i++) {
        rte_pktmbuf_free(static_cast<rte_mbuf*>(gnb_mbufs[i]));
      }
    }
    
    // ===== Part 2: Switch UE → GNB traffic =====
    void* ue_mbufs[batch_size];
    
    // Dequeue from ue_tx_ch1 (traffic from secondary UE)
    n = rte_ring_dequeue_burst(ue_tx_ch1, ue_mbufs, batch_size, nullptr);
    if (n > 0) {
      // Forward to gnb_inbound_ring (deliver to secondary GNB)
      unsigned enqueued = rte_ring_enqueue_burst(gnb_inbound_ring, ue_mbufs, n, nullptr);
      if (enqueued > 0) {
        total_ue_to_gnb += enqueued;
        switched_traffic = true;
        
        // Log first packet in batch
        if (total_ue_to_gnb <= 3 || (total_ue_to_gnb % 100 == 0)) {
          rte_mbuf* m = static_cast<rte_mbuf*>(ue_mbufs[0]);
          int16_t* data = rte_pktmbuf_mtod(m, int16_t*);
          std::fprintf(stderr, "[traffic_switch] UE→GNB: switched %u packets (total=%lu) | Sample: I=%d, Q=%d\n",
                      enqueued, total_ue_to_gnb, data[0], data[1]);
        }
      }
      
      // Free any packets that couldn't be enqueued
      for (unsigned i = enqueued; i < n; i++) {
        rte_pktmbuf_free(static_cast<rte_mbuf*>(ue_mbufs[i]));
      }
    }
    
    // Print periodic status
    if (loop_count % 10000 == 0) {
      std::fprintf(stderr, "[traffic_switch] Status: GNB→UE=%lu, UE→GNB=%lu packets\n",
                  total_gnb_to_ue, total_ue_to_gnb);
    }
    
    // Small sleep to avoid busy-waiting when no traffic
    if (!switched_traffic) {
      usleep(100);  // 100us sleep if no traffic to switch
    }
  }
  
  std::fprintf(stderr, "\n========================================\n");
  std::fprintf(stderr, "Traffic Switcher Shutting Down\n");
  std::fprintf(stderr, "========================================\n");
  std::fprintf(stderr, "Final Statistics:\n");
  std::fprintf(stderr, "  - GNB→UE packets switched: %lu\n", total_gnb_to_ue);
  std::fprintf(stderr, "  - UE→GNB packets switched: %lu\n", total_ue_to_gnb);
  std::fprintf(stderr, "  - Total packets switched: %lu\n", total_gnb_to_ue + total_ue_to_gnb);
  std::fprintf(stderr, "========================================\n");
  
  std::fprintf(stderr, "\n[traffic_switch] Shutdown complete.\n");
  
  return 0;
}
