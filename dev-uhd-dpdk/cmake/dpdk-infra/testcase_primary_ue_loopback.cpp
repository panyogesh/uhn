/**
 * @file testcase_primary_ue_loopback.cpp
 * @brief Primary-UE loopback process for test_flexsdr_factory
 * 
 * This program runs as a primary process and provides packet loopback:
 * - Receives packets from secondary-UE via ue_tx_ch1
 * - Echoes them back to secondary-UE via ue_inbound_ring
 * 
 * This allows test_flexsdr_factory (secondary-UE) to do bidirectional testing.
 * 
 * Usage:
 *   1. Start this primary-UE loopback first
 *   2. Then run test_flexsdr_factory --mode both
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
  std::fprintf(stderr, "\n[primary-ue-loopback] Caught signal %d, shutting down...\n", signum);
  g_shutdown_requested.store(true);
}

static void setup_signal_handlers() {
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);
}

int main(int argc, char** argv) {
  std::fprintf(stderr, "========================================\n");
  std::fprintf(stderr, "FlexSDR Primary-UE Loopback\n");
  std::fprintf(stderr, "PID: %d\n", getpid());
  std::fprintf(stderr, "========================================\n\n");

  if (argc < 2) {
    std::fprintf(stderr, "Usage: %s <config.yaml>\n", argv[0]);
    std::fprintf(stderr, "Example: %s dev-uhd-dpdk/conf/configurations-ue.yaml\n", argv[0]);
    return 2;
  }

  std::string cfg_path = argv[1];
  std::fprintf(stderr, "[primary-ue-loopback] Loading config from: %s\n", cfg_path.c_str());

  setup_signal_handlers();

  // Load config for EAL initialization
  flexsdr::conf::PrimaryConfig cfg;
  int cfg_rc = flexsdr::conf::load_from_yaml(cfg_path.c_str(), cfg);
  if (cfg_rc) {
    std::fprintf(stderr, "[primary-ue-loopback] ERROR: Failed to load config (rc=%d)\n", cfg_rc);
    return 1;
  }

  // Initialize DPDK EAL as primary process
  std::fprintf(stderr, "[primary-ue-loopback] Initializing DPDK EAL...\n");
  flexsdr::EalBootstrap eal(cfg, "flexsdr-app");  // Use same prefix as secondary
  eal.build_args({"--proc-type=primary"});
  
  std::fprintf(stderr, "[primary-ue-loopback] EAL arguments: %s\n", eal.args_as_cmdline().c_str());
  
  int eal_rc = eal.init();
  if (eal_rc < 0) {
    std::fprintf(stderr, "[primary-ue-loopback] ERROR: EAL initialization failed (rc=%d)\n", eal_rc);
    return 1;
  }
  std::fprintf(stderr, "[primary-ue-loopback] EAL initialized successfully (consumed %d args)\n", eal_rc);

  // Create FlexSDRPrimary instance (after EAL is initialized)
  flexsdr::FlexSDRPrimary primary_app(cfg_path);
  std::fprintf(stderr, "[primary-ue-loopback] FlexSDRPrimary constructed\n");

  // Initialize DPDK resources (pools, rings)
  std::fprintf(stderr, "[primary-ue-loopback] Initializing resources (pools, rings)...\n");
  int rc = primary_app.init_resources();
  if (rc) {
    std::fprintf(stderr, "[primary-ue-loopback] ERROR: Resource initialization failed (rc=%d)\n", rc);
    return 1;
  }

  // Get actual resources created
  const auto& pools = primary_app.pools();
  const auto& tx_rings = primary_app.tx_rings();
  const auto& rx_rings = primary_app.rx_rings();
  
  std::fprintf(stderr, "\n[primary-ue-loopback] ✓ All resources initialized successfully!\n");
  std::fprintf(stderr, "[primary-ue-loopback] Resources created:\n");
  std::fprintf(stderr, "  - %zu Memory pool(s)\n", pools.size());
  std::fprintf(stderr, "  - %zu TX ring(s)\n", tx_rings.size());
  std::fprintf(stderr, "  - %zu RX ring(s)\n", rx_rings.size());

  // Find required rings
  rte_ring* ue_tx_ch1 = nullptr;
  rte_ring* ue_inbound_ring = nullptr;
  
  for (const auto& ring : tx_rings) {
    if (std::string(ring->name) == "ue_tx_ch1") {
      ue_tx_ch1 = ring;
      std::fprintf(stderr, "  - Found ue_tx_ch1 (size=%u)\n", rte_ring_get_size(ring));
    }
  }
  
  for (const auto& ring : rx_rings) {
    if (std::string(ring->name) == "ue_inbound_ring") {
      ue_inbound_ring = ring;
      std::fprintf(stderr, "  - Found ue_inbound_ring (size=%u)\n", rte_ring_get_size(ring));
    }
  }
  
  // Verify required rings exist
  if (!ue_tx_ch1) {
    std::fprintf(stderr, "[primary-ue-loopback] ERROR: ue_tx_ch1 ring not found!\n");
    return 1;
  }
  if (!ue_inbound_ring) {
    std::fprintf(stderr, "[primary-ue-loopback] ERROR: ue_inbound_ring not found!\n");
    return 1;
  }
  
  std::fprintf(stderr, "\n========================================\n");
  std::fprintf(stderr, "Primary-UE Loopback Running\n");
  std::fprintf(stderr, "========================================\n");
  std::fprintf(stderr, "Packet Flow:\n");
  std::fprintf(stderr, "  Secondary-UE TX → ue_tx_ch1 → ue_inbound_ring → Secondary-UE RX\n");
  std::fprintf(stderr, "========================================\n");
  std::fprintf(stderr, "Ready for secondary-UE to connect.\n");
  std::fprintf(stderr, "Press Ctrl+C to shutdown...\n\n");
  
  uint64_t total_looped = 0;
  uint64_t loop_count = 0;
  
  // Main loopback loop - runs continuously until interrupted
  while (!g_shutdown_requested.load()) {
    loop_count++;
    
    // Dequeue from ue_tx_ch1 (packets from secondary-UE)
    const unsigned batch_size = 32;
    void* mbufs[batch_size];
    
    unsigned n = rte_ring_dequeue_burst(ue_tx_ch1, mbufs, batch_size, nullptr);
    if (n > 0) {
      // Forward to ue_inbound_ring (deliver back to secondary-UE)
      unsigned enqueued = rte_ring_enqueue_burst(ue_inbound_ring, mbufs, n, nullptr);
      if (enqueued > 0) {
        total_looped += enqueued;
        
        // Log progress
        if (total_looped <= 3 || (total_looped % 100 == 0)) {
          rte_mbuf* m = static_cast<rte_mbuf*>(mbufs[0]);
          int16_t* data = rte_pktmbuf_mtod(m, int16_t*);
          std::fprintf(stderr, "[primary-ue-loopback] Looped %u packets (total=%lu) | Sample: I=%d, Q=%d\n",
                      enqueued, total_looped, data[0], data[1]);
        }
      }
      
      // Free any packets that couldn't be enqueued
      for (unsigned i = enqueued; i < n; i++) {
        rte_pktmbuf_free(static_cast<rte_mbuf*>(mbufs[i]));
      }
    }
    
    // Print periodic status
    if (loop_count % 10000 == 0) {
      std::fprintf(stderr, "[primary-ue-loopback] Status: %lu packets looped\n", total_looped);
    }
    
    // Small sleep to avoid busy-waiting when no traffic
    if (n == 0) {
      usleep(100);  // 100us sleep if no traffic
    }
  }
  
  std::fprintf(stderr, "\n========================================\n");
  std::fprintf(stderr, "Primary-UE Loopback Shutting Down\n");
  std::fprintf(stderr, "========================================\n");
  std::fprintf(stderr, "Final Statistics:\n");
  std::fprintf(stderr, "  - Total packets looped: %lu\n", total_looped);
  std::fprintf(stderr, "========================================\n");
  
  std::fprintf(stderr, "\n[primary-ue-loopback] Shutdown complete.\n");
  
  return 0;
}
