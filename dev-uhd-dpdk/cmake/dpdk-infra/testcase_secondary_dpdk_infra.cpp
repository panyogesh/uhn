#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <csignal>
#include <atomic>
#include <unistd.h>

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

  std::fprintf(stderr, "\n[ue] ✓ All resources found successfully!\n");
  std::fprintf(stderr, "[ue] Shared rings accessed:\n");
  std::fprintf(stderr, "  - TX rings: ue_tx_ch1, ue_tx_ch2\n");
  std::fprintf(stderr, "  - RX rings: ue_inbound_ring\n");
  std::fprintf(stderr, "\n[ue] Secondary process is ready!\n");
  std::fprintf(stderr, "[ue] Press Ctrl+C to shutdown gracefully...\n\n");

  // Idle loop - demonstrate secondary is running
  int heartbeat = 0;
  while (!g_shutdown_requested.load()) {
    sleep(5);
    std::fprintf(stderr, "[ue] Heartbeat: %d (still running)\n", ++heartbeat);
  }

  std::fprintf(stderr, "\n[ue] Shutting down...\n");
  std::fprintf(stderr, "[ue] Test completed successfully.\n");
  
  return 0;
}
