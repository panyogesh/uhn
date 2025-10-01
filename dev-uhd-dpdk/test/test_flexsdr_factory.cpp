#include <uhd/version.hpp>
#include <uhd/device.hpp>
#include <uhd/types/device_addr.hpp>
#include <uhd/types/stream_cmd.hpp>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <cmath>
#include <atomic>
#include <csignal>

// Validate generated headers wiring:
#include "flexsdr.grpc.pb.h"

// From registry.cpp
extern "C" void flexsdr_register_with_uhd();

// DPDK
extern "C" {
#include <rte_config.h>
#include <rte_eal.h>
#include <rte_errno.h>
}

#include "transport/flexsdr_secondary.hpp"
#include "transport/eal_bootstrap.hpp"
#include "device/flexsdr_device.hpp"

// Global shutdown flag
static std::atomic<bool> g_shutdown_requested{false};

static void signal_handler(int signum) {
  std::fprintf(stderr, "\n[SIGNAL] Caught signal %d, shutting down...\n", signum);
  g_shutdown_requested.store(true);
}

// CLI
struct Cli {
    std::string cfg  = "conf/configurations-ue.yaml";
    std::string args = "type=flexsdr,addr=127.0.0.1,port=50051";
    int hold_secs = 30;
};

static void usage(const char* prog) {
    std::cout
      << "Usage: " << prog << " [OPTIONS]\n"
      << "Options:\n"
      << "  --cfg <yaml>      Configuration file (default: conf/configurations-ue.yaml)\n"
      << "  --args <uhd_args> UHD device args (default: type=flexsdr,addr=127.0.0.1,port=50051)\n"
      << "  --hold <seconds>  How long to run test (default: 30)\n"
      << "  -h, --help       Show this help\n";
}

static bool parse_cli(int argc, char** argv, Cli& cli) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--cfg" && i+1 < argc) {
            cli.cfg = argv[++i];
        } else if (a == "--args" && i+1 < argc) {
            cli.args = argv[++i];
        } else if (a == "--hold" && i+1 < argc) {
            cli.hold_secs = std::stoi(argv[++i]);
        } else if (a == "-h" || a == "--help") {
            usage(argv[0]);
            std::exit(0);
        } else {
            std::cerr << "Unknown option: " << a << "\n";
            return false;
        }
    }
    return true;
}

// RX TEST
void test_rx_reception(uhd::rx_streamer::sptr rx_stream, int duration_sec) {
    std::cout << "\n========================================\n";
    std::cout << "RX TEST: Receiving IQ samples\n";
    std::cout << "========================================\n";
    
    const size_t num_channels = rx_stream->get_num_channels();
    const size_t samps_per_buff = 4096;
    
    std::cout << "[RX] Channels: " << num_channels << "\n";
    std::cout << "[RX] Duration: " << duration_sec << " seconds\n\n";
    
    // Allocate buffers
    std::vector<std::vector<std::complex<int16_t>>> buffs(num_channels);
    std::vector<void*> buff_ptrs(num_channels);
    for (size_t ch = 0; ch < num_channels; ch++) {
        buffs[ch].resize(samps_per_buff);
        buff_ptrs[ch] = buffs[ch].data();
    }
    
    // Start streaming
    uhd::stream_cmd_t cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
    cmd.stream_now = true;
    rx_stream->issue_stream_cmd(cmd);
    std::cout << "[RX] Stream started\n\n";
    
    // Statistics
    uint64_t total_samples = 0;
    uint64_t total_bursts = 0;
    uint64_t timeout_count = 0;
    bool first_displayed = false;
    
    auto start_time = std::chrono::steady_clock::now();
    auto last_stats = start_time;
    
    // Receive loop
    while (!g_shutdown_requested.load()) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
        
        if (elapsed >= duration_sec) break;
        
        uhd::rx_metadata_t md;
        size_t n = rx_stream->recv(buff_ptrs, samps_per_buff, md, 0.1);
        
        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) {
            timeout_count++;
            continue;
        }
        
        if (n > 0) {
            total_samples += n;
            total_bursts++;
            
            // Display first 3 bursts with sample details
            if (total_bursts <= 3) {
                std::cout << "[RX] Burst " << total_bursts << ": " << n << " samples received\n";
                std::cout << "[RX] First 8 samples from each channel:\n";
                for (size_t ch = 0; ch < num_channels; ch++) {
                    std::cout << "  CH" << ch << ": ";
                    for (size_t i = 0; i < std::min(size_t(8), n); i++) {
                        std::cout << "(" << buffs[ch][i].real() << "," << buffs[ch][i].imag() << ") ";
                    }
                    std::cout << "\n";
                }
                std::cout << "\n";
                first_displayed = true;
            }
            
            auto stats_elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_stats).count();
            if (stats_elapsed >= 5) {
                std::cout << "[RX] @ " << elapsed << "s: " << total_samples 
                          << " samples, " << total_bursts << " bursts\n";
                last_stats = now;
            }
        }
    }
    
    // Stop
    cmd.stream_mode = uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS;
    rx_stream->issue_stream_cmd(cmd);
    
    auto total_time = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start_time).count();
    
    std::cout << "\n========================================\n";
    std::cout << "RX TEST SUMMARY\n";
    std::cout << "Duration: " << std::fixed << std::setprecision(2) << total_time << " s\n";
    std::cout << "Samples: " << total_samples << "\n";
    std::cout << "Bursts: " << total_bursts << "\n";
    std::cout << "Throughput: " << std::fixed << std::setprecision(2) 
              << (total_samples / 1e6) / total_time << " Msps\n";
    std::cout << "========================================\n\n";
}

int main(int argc, char** argv) {
    std::cout << "========================================\n";
    std::cout << "FlexSDR Factory Test\n";
    std::cout << "UHD: " << uhd::get_version_string() << "\n";
    std::cout << "========================================\n\n";

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    Cli cli;
    if (!parse_cli(argc, argv, cli)) {
        usage(argv[0]);
        return 1;
    }

    std::cout << "[CONFIG] YAML: " << cli.cfg << "\n";
    std::cout << "[CONFIG] Duration: " << cli.hold_secs << " seconds\n\n";

    // Initialize EAL first (as secondary process)
    std::cout << "[DPDK] Initializing EAL as secondary process...\n";
    
    try {
        // Load config
        flexsdr::conf::PrimaryConfig cfg;
        if (flexsdr::conf::load_from_yaml(cli.cfg.c_str(), cfg) != 0) {
            std::cerr << "[ERROR] Failed to load YAML config\n";
            return 2;
        }
        
        // Initialize DPDK EAL in secondary mode
        flexsdr::EalBootstrap eal(cfg, "flexsdr-test");
        eal.build_args({"--proc-type=secondary"});
        
        int eal_rc = eal.init();
        if (eal_rc < 0) {
            std::cerr << "[ERROR] EAL init failed: " << rte_strerror(rte_errno) << "\n";
            return 2;
        }
        std::cout << "[DPDK] EAL initialized (consumed " << eal_rc << " args)\n";
        
        // Now create FlexSDRSecondary and lookup resources
        auto secondary = std::make_shared<flexsdr::FlexSDRSecondary>(cli.cfg);
        
        if (secondary->init_resources() != 0) {
            std::cerr << "[ERROR] Failed to lookup secondary resources\n";
            return 2;
        }
        
        std::cout << "[DPDK] Secondary initialized successfully\n";
        std::cout << "[DPDK] RX rings: " << secondary->num_rx_queues() << "\n";
        std::cout << "[DPDK] TX rings: " << secondary->num_tx_queues() << "\n";
        std::cout << "[DPDK] Pools: " << secondary->num_pools() << "\n\n";

        // Register FlexSDR with UHD
        flexsdr_register_with_uhd();

        // Create UHD device
        std::cout << "[UHD] Creating device...\n";
        uhd::device_addr_t dev_args(cli.args);
        auto device = uhd::device::make(dev_args);
        
        auto fdev = std::dynamic_pointer_cast<flexsdr::flexsdr_device>(device);
        if (!fdev) {
            std::cerr << "[ERROR] Not a flexsdr_device\n";
            return 3;
        }
        
        // Create DPDK context and attach
        auto ctx = std::make_shared<flexsdr::DpdkContext>();
        
        // Get first RX ring from secondary
        ctx->ue_in = secondary->rx_ring_for_queue(0);
        ctx->ue_tx0 = secondary->tx_ring_for_queue(0);
        ctx->ue_mp = secondary->pool_for_queue(0);
        
        fdev->attach_dpdk_context(ctx, flexsdr::Role::UE);
        std::cout << "[UHD] Device created\n\n";

        // Create RX stream
        uhd::stream_args_t sargs("sc16", "sc16");
        sargs.channels = {0,1,2,3};
        auto rx = fdev->get_rx_stream(sargs);
        
        std::cout << "[STREAMS] RX: " << rx->get_num_channels() << " channels\n\n";

        // Run RX test
        test_rx_reception(rx, cli.hold_secs);

    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << "\n";
        return 4;
    }

    std::cout << "[DONE] Test completed\n";
    return 0;
}
