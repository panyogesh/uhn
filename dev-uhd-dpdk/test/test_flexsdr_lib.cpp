/**
 * test_flexsdr_lib.cpp
 * 
 * Test harness for flexsdr_lib.cpp that simulates OAI usage patterns.
 * This test initializes the FlexSDR device using the device_init() function
 * and tests TX/RX streaming operations through the OAI wrapper functions.
 * 
 * Prerequisites:
 * - Primary process must be running (creates DPDK resources)
 * - conf/configurations-ue.yaml must exist
 * - gRPC server should be available
 */

#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <complex>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <iomanip>
#include <cmath>

// Include the common_lib.h header that defines openair0_device_t
#include "common_lib.h"

// Forward declaration of device_init from flexsdr_lib.cpp
extern "C" int device_init(openair0_device_t* device, openair0_config_t* cfg);

// Global shutdown flag
static std::atomic<bool> g_shutdown_requested{false};

static void signal_handler(int signum) {
    std::fprintf(stderr, "\n[SIGNAL] Caught signal %d, shutting down...\n", signum);
    g_shutdown_requested.store(true);
}

// Helper function to print device info
void print_device_info(openair0_device_t* device) {
    std::cout << "\n========================================\n";
    std::cout << "FlexSDR Device Information\n";
    std::cout << "========================================\n";
    std::cout << "Device type: " << device->type << "\n";
    std::cout << "Host type: " << device->host_type << "\n";
    std::cout << "Function pointers:\n";
    std::cout << "  - trx_start_func: " << (device->trx_start_func ? "✓" : "✗") << "\n";
    std::cout << "  - trx_write_func: " << (device->trx_write_func ? "✓" : "✗") << "\n";
    std::cout << "  - trx_read_func: " << (device->trx_read_func ? "✓" : "✗") << "\n";
    std::cout << "  - trx_set_freq_func: " << (device->trx_set_freq_func ? "✓" : "✗") << "\n";
    std::cout << "  - trx_set_gains_func: " << (device->trx_set_gains_func ? "✓" : "✗") << "\n";
    std::cout << "  - trx_end_func: " << (device->trx_end_func ? "✓" : "✗") << "\n";
    std::cout << "========================================\n\n";
}

// Test TX transmission
void test_tx_transmission(openair0_device_t* device, int num_bursts = 100) {
    std::cout << "\n========================================\n";
    std::cout << "TX TEST: Transmitting IQ samples\n";
    std::cout << "========================================\n";
    
    const int samps_per_burst = 1024;
    const int num_channels = 1;  // Single TX channel
    
    // Allocate buffers for TX
    std::vector<std::complex<int16_t>> tx_buffer(samps_per_burst);
    void* buffers[num_channels];
    buffers[0] = tx_buffer.data();
    
    // Generate test tone (complex sinusoid)
    const double freq_norm = 0.1;  // Normalized frequency
    const double amplitude = 8000.0;  // Scale for int16
    
    for (int i = 0; i < samps_per_burst; i++) {
        double phase = 2.0 * M_PI * freq_norm * i;
        tx_buffer[i] = std::complex<int16_t>(
            static_cast<int16_t>(amplitude * std::cos(phase)),
            static_cast<int16_t>(amplitude * std::sin(phase))
        );
    }
    
    std::cout << "[TX] Generated tone at normalized freq " << freq_norm << "\n";
    std::cout << "[TX] Transmitting " << num_bursts << " bursts...\n\n";
    
    // Statistics
    uint64_t total_samples = 0;
    uint64_t total_bursts = 0;
    uint64_t send_failures = 0;
    
    auto start_time = std::chrono::steady_clock::now();
    
    // TX loop
    openair0_timestamp current_ts = 0;
    for (int burst = 0; burst < num_bursts && !g_shutdown_requested.load(); burst++) {
        // Call trx_write_func
        int sent = device->trx_write_func(
            device,
            current_ts,
            buffers,
            samps_per_burst,
            0,  // flags
            1   // cc (component carrier)
        );
        
        if (sent == samps_per_burst) {
            total_samples += sent;
            total_bursts++;
            current_ts += samps_per_burst;
            
            // Display progress
            if (burst < 3 || burst % 20 == 0) {
                std::cout << "[TX] Burst " << burst << ": " << sent << " samples sent\n";
            }
        } else {
            send_failures++;
            std::cerr << "[TX] WARNING: Partial send (" << sent << "/" << samps_per_burst << ")\n";
        }
        
        // Small delay
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    
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

// Test RX reception
void test_rx_reception(openair0_device_t* device, int num_bursts = 100) {
    std::cout << "\n========================================\n";
    std::cout << "RX TEST: Receiving IQ samples\n";
    std::cout << "========================================\n";
    
    const int samps_per_burst = 4096;
    const int num_channels = 4;  // 4 RX channels
    
    // Allocate buffers for RX
    std::vector<std::vector<std::complex<int16_t>>> rx_buffers(num_channels);
    std::vector<void*> buffer_ptrs(num_channels);
    
    for (int ch = 0; ch < num_channels; ch++) {
        rx_buffers[ch].resize(samps_per_burst);
        buffer_ptrs[ch] = rx_buffers[ch].data();
    }
    
    std::cout << "[RX] Receiving " << num_bursts << " bursts...\n";
    std::cout << "[RX] Channels: " << num_channels << "\n";
    std::cout << "[RX] Samples per burst: " << samps_per_burst << "\n\n";
    
    // Statistics
    uint64_t total_samples = 0;
    uint64_t total_bursts = 0;
    uint64_t timeout_count = 0;
    
    auto start_time = std::chrono::steady_clock::now();
    
    // RX loop
    for (int burst = 0; burst < num_bursts && !g_shutdown_requested.load(); burst++) {
        openair0_timestamp rx_ts = 0;
        
        // Call trx_read_func
        int received = device->trx_read_func(
            device,
            &rx_ts,
            buffer_ptrs.data(),
            samps_per_burst,
            num_channels
        );
        
        if (received > 0) {
            total_samples += received;
            total_bursts++;
            
            // Display progress and first samples
            if (burst < 3 || burst % 20 == 0) {
                std::cout << "[RX] Burst " << burst << ": " << received << " samples received";
                std::cout << " (ts=" << rx_ts << ")\n";
                
                if (burst < 3) {
                    std::cout << "[RX] First 4 samples CH0: ";
                    for (int i = 0; i < std::min(4, received); i++) {
                        std::cout << "(" << rx_buffers[0][i].real() << "," 
                                  << rx_buffers[0][i].imag() << ") ";
                    }
                    std::cout << "\n";
                }
            }
        } else if (received == 0) {
            timeout_count++;
            if (timeout_count % 10 == 1) {
                std::cerr << "[RX] WARNING: No data received (timeout)\n";
            }
        }
    }
    
    auto total_time = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start_time).count();
    
    std::cout << "\n========================================\n";
    std::cout << "RX TEST SUMMARY\n";
    std::cout << "Duration: " << std::fixed << std::setprecision(2) << total_time << " s\n";
    std::cout << "Samples: " << total_samples << "\n";
    std::cout << "Bursts: " << total_bursts << "\n";
    std::cout << "Timeouts: " << timeout_count << "\n";
    std::cout << "Throughput: " << std::fixed << std::setprecision(2) 
              << (total_samples / 1e6) / total_time << " Msps\n";
    std::cout << "========================================\n\n";
}

// Test bidirectional TX/RX
void test_bidirectional(openair0_device_t* device, int num_packets = 50) {
    std::cout << "\n========================================\n";
    std::cout << "BIDIRECTIONAL TEST\n";
    std::cout << "========================================\n";
    std::cout << "[INFO] Running TX and RX simultaneously...\n\n";
    
    std::atomic<bool> tx_done{false};
    std::atomic<bool> rx_done{false};
    std::atomic<uint64_t> tx_count{0};
    std::atomic<uint64_t> rx_count{0};
    
    // Start RX in background thread
    std::thread rx_thread([&]() {
        const int samps_per_burst = 4096;
        const int num_channels = 4;
        
        std::vector<std::vector<std::complex<int16_t>>> rx_buffers(num_channels);
        std::vector<void*> buffer_ptrs(num_channels);
        
        for (int ch = 0; ch < num_channels; ch++) {
            rx_buffers[ch].resize(samps_per_burst);
            buffer_ptrs[ch] = rx_buffers[ch].data();
        }
        
        while (!rx_done.load() && rx_count.load() < static_cast<uint64_t>(num_packets)) {
            openair0_timestamp rx_ts = 0;
            int received = device->trx_read_func(
                device, &rx_ts, buffer_ptrs.data(), samps_per_burst, num_channels);
            
            if (received > 0) {
                rx_count++;
                if (rx_count.load() % 10 == 0) {
                    std::cout << "[RX] Progress: " << rx_count.load() 
                              << "/" << num_packets << " packets\n";
                }
            }
        }
        std::cout << "[RX] Thread complete: " << rx_count.load() << " packets received\n";
    });
    
    // Small delay to let RX start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Start TX in main thread
    const int samps_per_burst = 1024;
    std::vector<std::complex<int16_t>> tx_buffer(samps_per_burst);
    void* buffers[1];
    buffers[0] = tx_buffer.data();
    
    // Generate tone
    for (int i = 0; i < samps_per_burst; i++) {
        double phase = 2.0 * M_PI * 0.1 * i;
        tx_buffer[i] = std::complex<int16_t>(
            static_cast<int16_t>(8000.0 * std::cos(phase)),
            static_cast<int16_t>(8000.0 * std::sin(phase))
        );
    }
    
    openair0_timestamp current_ts = 0;
    while (tx_count.load() < static_cast<uint64_t>(num_packets) && !g_shutdown_requested.load()) {
        int sent = device->trx_write_func(device, current_ts, buffers, samps_per_burst, 0, 1);
        if (sent == samps_per_burst) {
            tx_count++;
            current_ts += samps_per_burst;
            if (tx_count.load() % 10 == 0) {
                std::cout << "[TX] Progress: " << tx_count.load() 
                          << "/" << num_packets << " packets\n";
            }
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    
    std::cout << "[TX] Complete: " << tx_count.load() << " packets sent\n";
    
    // Wait for RX to complete (with timeout)
    auto rx_start = std::chrono::steady_clock::now();
    while (rx_count.load() < static_cast<uint64_t>(num_packets)) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - rx_start).count();
        if (elapsed > 10) {
            std::cout << "[INFO] RX timeout after 10s, received " 
                      << rx_count.load() << "/" << num_packets << " packets\n";
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    rx_done.store(true);
    rx_thread.join();
    
    std::cout << "\n========================================\n";
    std::cout << "BIDIRECTIONAL TEST SUMMARY\n";
    std::cout << "TX packets sent: " << tx_count.load() << "/" << num_packets << "\n";
    std::cout << "RX packets received: " << rx_count.load() << "/" << num_packets << "\n";
    std::cout << "========================================\n\n";
}

int main(int argc, char** argv) {
    std::cout << "========================================\n";
    std::cout << "FlexSDR Library Test\n";
    std::cout << "Testing device_init() and OAI wrappers\n";
    std::cout << "========================================\n\n";
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Parse command line arguments
    std::string test_mode = "all";  // all, tx, rx, bidir
    int num_packets = 100;
    std::string config_file = "conf/configurations-ue.yaml";  // Default to UE
    std::string role = "ue";  // ue or gnb
    
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--mode" && i+1 < argc) {
            test_mode = argv[++i];
        } else if (arg == "--packets" && i+1 < argc) {
            num_packets = std::stoi(argv[++i]);
        } else if (arg == "--config" && i+1 < argc) {
            config_file = argv[++i];
        } else if (arg == "--role" && i+1 < argc) {
            role = argv[++i];
            // Set default config based on role if not explicitly set
            if (role == "gnb") {
                config_file = "conf/configurations-gnb.yaml";
            } else if (role == "ue") {
                config_file = "conf/configurations-ue.yaml";
            }
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [OPTIONS]\n";
            std::cout << "Options:\n";
            std::cout << "  --mode <mode>     Test mode: all, tx, rx, bidir (default: all)\n";
            std::cout << "  --packets <n>     Number of packets to send/receive (default: 100)\n";
            std::cout << "  --role <role>     Device role: ue or gnb (default: ue)\n";
            std::cout << "  --config <file>   Configuration file path (default: auto from role)\n";
            std::cout << "  -h, --help        Show this help\n";
            std::cout << "\nExamples:\n";
            std::cout << "  " << argv[0] << " --role ue --mode all\n";
            std::cout << "  " << argv[0] << " --role gnb --mode tx --packets 200\n";
            std::cout << "  " << argv[0] << " --config conf/configurations-ue.yaml --mode rx\n";
            return 0;
        }
    }
    
    std::cout << "[CONFIG] Role: " << role << "\n";
    std::cout << "[CONFIG] Config file: " << config_file << "\n";
    std::cout << "[CONFIG] Test mode: " << test_mode << "\n";
    std::cout << "[CONFIG] Packets: " << num_packets << "\n\n";
    
    // Store config file in environment variable for device_init to use
    setenv("FLEXSDR_CONFIG_FILE", config_file.c_str(), 1);
    
    // Step 1: Create openair0_device_t structure
    openair0_device_t device;
    memset(&device, 0, sizeof(device));
    
    // Step 2: Create openair0_config_t structure with realistic OAI settings
    openair0_config_t config;
    memset(&config, 0, sizeof(config));
    
    // Configure for NR TDD, n78 band (3.5 GHz), 30 MHz BW
    config.sample_rate = 30.72e6;  // 30.72 MHz for 30 MHz BW
    config.samples_per_frame = 307200;  // 10ms frame
    
    // TX configuration (1 antenna)
    config.tx_num_channels = 1;
    config.tx_freq[0] = 3.5e9;  // 3.5 GHz
    config.tx_gain[0] = 90.0;
    config.tx_bw = 30e6;
    
    // RX configuration (2 antennas)
    config.rx_num_channels = 2;
    config.rx_freq[0] = 3.5e9;  // 3.5 GHz
    config.rx_freq[1] = 3.5e9;
    config.rx_gain[0] = 60.0;
    config.rx_gain[1] = 60.0;
    config.rx_bw = 30e6;
    
    // WRX configuration (wide RX for satellite)
    config.wrx_num_channels = 2;
    config.wrx_freq[0] = 2.0e9;  // 2.0 GHz (satellite downlink)
    config.wrx_freq[1] = 2.0e9;
    config.wrx_gain[0] = 40.0;
    config.wrx_gain[1] = 40.0;
    
    device.openair0_cfg = &config;
    
    std::cout << "[CONFIG] Sample rate: " << config.sample_rate / 1e6 << " MHz\n";
    std::cout << "[CONFIG] TX: " << config.tx_num_channels << " channels @ " 
              << config.tx_freq[0] / 1e9 << " GHz\n";
    std::cout << "[CONFIG] RX: " << config.rx_num_channels << " channels @ " 
              << config.rx_freq[0] / 1e9 << " GHz\n";
    std::cout << "[CONFIG] WRX: " << config.wrx_num_channels << " channels @ " 
              << config.wrx_freq[0] / 1e9 << " GHz\n\n";
    
    // Step 3: Call device_init()
    std::cout << "Calling device_init()...\n";
    int rc = device_init(&device, &config);
    if (rc != 0) {
        std::cerr << "[ERROR] device_init() failed with code " << rc << "\n";
        return 1;
    }
    
    print_device_info(&device);
    
    // Step 4: Start streaming
    std::cout << "Starting streaming with trx_start_func()...\n";
    if (device.trx_start_func) {
        rc = device.trx_start_func(&device);
        if (rc != 0) {
            std::cerr << "[ERROR] trx_start_func() failed with code " << rc << "\n";
            device.trx_end_func(&device);
            return 2;
        }
        std::cout << "[SUCCESS] Streaming started\n\n";
    }
    
    // Small delay to let streams initialize
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // Step 5: Run tests based on mode
    try {
        if (test_mode == "all") {
            test_tx_transmission(&device, num_packets);
            test_rx_reception(&device, num_packets);
            test_bidirectional(&device, num_packets / 2);
        } else if (test_mode == "tx") {
            test_tx_transmission(&device, num_packets);
        } else if (test_mode == "rx") {
            test_rx_reception(&device, num_packets);
        } else if (test_mode == "bidir") {
            test_bidirectional(&device, num_packets);
        } else {
            std::cerr << "[ERROR] Invalid test mode: " << test_mode << "\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Test exception: " << e.what() << "\n";
    }
    
    // Step 6: Cleanup
    std::cout << "\nCleaning up with trx_end_func()...\n";
    if (device.trx_end_func) {
        device.trx_end_func(&device);
        std::cout << "[SUCCESS] Device cleanup complete\n";
    }
    
    std::cout << "\n========================================\n";
    std::cout << "Test completed successfully\n";
    std::cout << "========================================\n";
    
    return 0;
}
