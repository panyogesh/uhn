#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <csignal>
#include <atomic>
#include <unistd.h>

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

  std::fprintf(stderr, "\n[primary-ue] ✓ All resources initialized successfully!\n");
  std::fprintf(stderr, "[primary-ue] Resources created:\n");
  std::fprintf(stderr, "  - Memory pools for UE TX/RX\n");
  std::fprintf(stderr, "  - TX rings: ue_tx_ch1, ue_tx_ch2\n");
  std::fprintf(stderr, "  - RX rings: ue_inbound_ring\n");
  std::fprintf(stderr, "\n[primary-ue] Ready for secondary processes to attach.\n");
  std::fprintf(stderr, "[primary-ue] Press Ctrl+C to shutdown gracefully...\n\n");

  // Idle loop - keep primary process alive
  while (!g_shutdown_requested.load()) {
    sleep(1);
  }

  std::fprintf(stderr, "\n[primary-ue] Shutting down...\n");
  std::fprintf(stderr, "[primary-ue] Test completed successfully.\n");
  
  return 0;
}
