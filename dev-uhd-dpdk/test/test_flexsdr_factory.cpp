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
    std::string mode = "tx";  // tx, rx, or both
    int hold_secs = 30;
};

static void usage(const char* prog) {
    std::cout
      << "Usage: " << prog << " [OPTIONS]\n"
      << "Options:\n"
      << "  --cfg <yaml>      Configuration file (default: conf/configurations-ue.yaml)\n"
      << "  --args <uhd_args> UHD device args (default: type=flexsdr,addr=127.0.0.1,port=50051)\n"
      << "  --mode <mode>     Test mode: tx, rx, or both (default: tx)\n"
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
        } else if (a == "--mode" && i+1 < argc) {
            cli.mode = argv[++i];
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

// TX TEST
void test_tx_transmission(uhd::tx_streamer::sptr tx_stream, int max_bursts) {
    std::cout << "\n========================================\n";
    std::cout << "TX TEST: Transmitting IQ samples\n";
    std::cout << "========================================\n";
    
    const size_t num_channels = tx_stream->get_num_channels();
    const size_t samps_per_buff = 1024;
    
    std::cout << "[TX] Channels: " << num_channels << "\n";
    std::cout << "[TX] Max bursts: " << max_bursts << "\n";
    std::cout << "[TX] Samples per burst: " << samps_per_buff << "\n\n";
    
    // Allocate buffers and generate test tone (complex sinusoid)
    std::vector<std::vector<std::complex<int16_t>>> buffs(num_channels);
    std::vector<const void*> buff_ptrs(num_channels);
    
    // Generate different tone for each channel
    for (size_t ch = 0; ch < num_channels; ch++) {
        buffs[ch].resize(samps_per_buff);
        const double freq_norm = 0.1 + ch * 0.05;  // Normalized frequency
        const double amplitude = 8000.0;  // Scale for int16
        
        for (size_t i = 0; i < samps_per_buff; i++) {
            double phase = 2.0 * M_PI * freq_norm * i;
            buffs[ch][i] = std::complex<int16_t>(
                static_cast<int16_t>(amplitude * std::cos(phase)),
                static_cast<int16_t>(amplitude * std::sin(phase))
            );
        }
        buff_ptrs[ch] = buffs[ch].data();
        
        std::cout << "[TX] CH" << ch << ": Generated tone at normalized freq " 
                  << std::fixed << std::setprecision(3) << freq_norm << "\n";
    }
    std::cout << "\n";
    
    // Statistics
    uint64_t total_samples = 0;
    uint64_t total_bursts = 0;
    uint64_t send_failures = 0;
    
    auto start_time = std::chrono::steady_clock::now();
    auto last_stats = start_time;
    
    // Transmit loop
    uhd::tx_metadata_t md;
    md.start_of_burst = true;
    md.end_of_burst = false;
    md.has_time_spec = false;
    
    while (!g_shutdown_requested.load() && total_bursts < static_cast<uint64_t>(max_bursts)) {
        // Send burst
        size_t n = tx_stream->send(buff_ptrs, samps_per_buff, md, 0.1);
        
        if (n == samps_per_buff) {
            total_samples += n;
            total_bursts++;
            
            // Display progress
            if (total_bursts <= 3 || total_bursts % 20 == 0) {
                std::cout << "[TX] Burst " << total_bursts << ": " << n << " samples sent\n";
            }
        } else {
            send_failures++;
            if (send_failures % 100 == 1) {
                std::cerr << "[TX] WARNING: Partial send (" << n << "/" << samps_per_buff << ")\n";
            }
        }
        
        // Clear start_of_burst after first packet
        if (md.start_of_burst) {
            md.start_of_burst = false;
        }
        
        // Small delay to avoid overwhelming
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    
    // Send end of burst
    md.end_of_burst = true;
    tx_stream->send(buff_ptrs, 0, md, 0.1);
    
    auto total_time = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start_time).count();
    
    std::cout << "\n========================================\n";
    std::cout << "TX TEST SUMMARY\n";
    std::cout << "Duration: " << std::fixed << std::setprecision(2) << total_time << " s\n";
    std::cout << "Samples: " << total_samples << "\n";
    std::cout << "Bursts: " << total_bursts << "\n";
    std::cout << "Failures: " << send_failures << "\n";
    std::cout << "Throughput: " << std::fixed << std::setprecision(2) 
              << (total_samples / 1e6) / total_time << " Msps\n";
    std::cout << "========================================\n\n";
}

// RX TEST
void test_rx_reception(uhd::rx_streamer::sptr rx_stream, int max_bursts) {
    std::cout << "\n========================================\n";
    std::cout << "RX TEST: Receiving IQ samples\n";
    std::cout << "========================================\n";
    
    const size_t num_channels = rx_stream->get_num_channels();
    const size_t samps_per_buff = 4096;
    
    std::cout << "[RX] Channels: " << num_channels << "\n";
    std::cout << "[RX] Max bursts: " << max_bursts << "\n\n";
    
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
    while (!g_shutdown_requested.load() && total_bursts < static_cast<uint64_t>(max_bursts)) {
        uhd::rx_metadata_t md;
        size_t n = rx_stream->recv(buff_ptrs, samps_per_buff, md, 1.0);
        
        if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) {
            timeout_count++;
            continue;
        }
        
        if (n > 0) {
            total_samples += n;
            total_bursts++;
            
            // Display progress
            if (total_bursts <= 3 || total_bursts % 20 == 0) {
                std::cout << "[RX] Burst " << total_bursts << ": " << n << " samples received\n";
                if (total_bursts <= 3) {
                    std::cout << "[RX] First 4 samples CH0: ";
                    for (size_t i = 0; i < std::min(size_t(4), n); i++) {
                        std::cout << "(" << buffs[0][i].real() << "," << buffs[0][i].imag() << ") ";
                    }
                    std::cout << "\n";
                }
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
        
        // Get rings/pools from secondary
        ctx->ue_in = secondary->rx_ring_for_queue(0);
        ctx->ue_tx0 = secondary->tx_ring_for_queue(0);
        ctx->ue_mp = secondary->pool_for_queue(0);
        
        // IMPORTANT: Attach secondary as TxBackend provider
        ctx->secondary = secondary.get();
        
        fdev->attach_dpdk_context(ctx, flexsdr::Role::UE);
        std::cout << "[UHD] Device created\n\n";

        // Create streams
        uhd::stream_args_t rx_args("sc16", "sc16");
        rx_args.channels = {0,1,2,3};
        auto rx = fdev->get_rx_stream(rx_args);
        
        uhd::stream_args_t tx_args("sc16", "sc16");
        tx_args.channels = {0};  // Single channel for TX test
        auto tx = fdev->get_tx_stream(tx_args);
        
        std::cout << "[STREAMS] RX: " << rx->get_num_channels() << " channels\n";
        std::cout << "[STREAMS] TX: " << tx->get_num_channels() << " channels\n\n";

        // Run test based on mode
        if (cli.mode == "tx") {
            std::cout << "[INFO] Running TX test - sending 60 bursts to primary\n\n";
            test_tx_transmission(tx, 60);
            std::cout << "\n[INFO] TX test complete.\n";
        } else if (cli.mode == "rx") {
            std::cout << "[INFO] Running RX test - receiving 60 bursts from primary\n\n";
            test_rx_reception(rx, 60);
            std::cout << "\n[INFO] RX test complete.\n";
        } else if (cli.mode == "both") {
            std::cout << "[INFO] Running both TX and RX tests\n\n";
            test_tx_transmission(tx, 60);
            std::cout << "\n[INFO] Waiting 1 second...\n";
            std::this_thread::sleep_for(std::chrono::seconds(1));
            test_rx_reception(rx, 60);
            std::cout << "\n[INFO] Both tests complete.\n";
        } else {
            std::cerr << "[ERROR] Invalid mode: " << cli.mode << " (use tx, rx, or both)\n";
            return 5;
        }

    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << "\n";
        return 4;
    }

    std::cout << "[DONE] Test completed\n";
    return 0;
}
