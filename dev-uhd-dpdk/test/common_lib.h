/**
 * common_lib.h
 * 
 * Stub header file for OpenAirInterface RF API structures.
 * This is a simplified version for testing flexsdr_lib.cpp independently.
 * 
 * In production, this would come from:
 * openairinterface5g/radio/COMMON/common_lib.h
 */

#ifndef COMMON_LIB_H
#define COMMON_LIB_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

// ========================================
// Type Definitions
// ========================================

// Timestamp type used by OAI
typedef uint64_t openair0_timestamp;

// Maximum number of channels
#define MAX_CHANNELS 4

// Device types
typedef enum {
    NONE_DEV = 0,
    USRP_B200_DEV,
    USRP_X300_DEV,
    USRP_N300_DEV,
    BLADERF_DEV,
    LMSSDR_DEV,
    IRIS_DEV,
    ADRV9371_ZC706_DEV,
    SIMU_DEV,
    FLEXSDR_DEV,  // FlexSDR device type
    MAX_RF_DEV_TYPE
} dev_type_t;

// Host types
typedef enum {
    BBU_HOST = 0,
    RRH_HOST,
    RAU_HOST
} host_type_t;

// ========================================
// Configuration Structure
// ========================================

typedef struct {
    // Sample rate and timing
    double sample_rate;                    // Sampling rate in Hz
    double samples_per_frame;              // Samples per frame (10ms)
    
    // TX Configuration
    int tx_num_channels;                   // Number of TX channels
    double tx_freq[MAX_CHANNELS];          // TX center frequencies (Hz)
    double tx_gain[MAX_CHANNELS];          // TX gains (dB)
    double tx_bw;                          // TX bandwidth (Hz)
    
    // RX Configuration
    int rx_num_channels;                   // Number of RX channels
    double rx_freq[MAX_CHANNELS];          // RX center frequencies (Hz)
    double rx_gain[MAX_CHANNELS];          // RX gains (dB)
    double rx_bw;                          // RX bandwidth (Hz)
    
    // Wide RX Configuration (for satellite/NTN)
    int wrx_num_channels;                  // Number of wide RX channels
    double wrx_freq[MAX_CHANNELS];         // Wide RX center frequencies (Hz)
    double wrx_gain[MAX_CHANNELS];         // Wide RX gains (dB)
    
    // Additional parameters
    int clock_source;                      // 0=internal, 1=external, 2=gpsdo
    double rx_gain_offset[MAX_CHANNELS];   // RX gain offset for calibration
    double tx_gain_offset[MAX_CHANNELS];   // TX gain offset for calibration
    
    // Timing
    int tx_sample_advance;                 // TX sample advance
    int rx_sample_offset;                  // RX sample offset
    
    // Device-specific
    char *configFilename;                  // Optional config file
    void *dev_private;                     // Device private data
    
} openair0_config_t;

// ========================================
// Device Structure (Forward declaration)
// ========================================

struct openair0_device_t;

// Function pointer types for device operations
typedef int (*trx_start_func_t)(struct openair0_device_t *device);
typedef void (*trx_stop_func_t)(struct openair0_device_t *device);
typedef int (*trx_write_func_t)(struct openair0_device_t *device,
                                openair0_timestamp timestamp,
                                void **buff,
                                int nsamps,
                                int flags,
                                int cc);
typedef int (*trx_read_func_t)(struct openair0_device_t *device,
                               openair0_timestamp *timestamp,
                               void **buff,
                               int nsamps,
                               int cc);
typedef int (*trx_set_freq_func_t)(struct openair0_device_t *device,
                                   openair0_config_t *openair0_cfg);
typedef int (*trx_set_gains_func_t)(struct openair0_device_t *device,
                                    openair0_config_t *openair0_cfg);
typedef void (*trx_end_func_t)(struct openair0_device_t *device);

// ========================================
// Device Structure
// ========================================

typedef struct openair0_device_t {
    // Device identification
    dev_type_t type;                       // Device type
    host_type_t host_type;                 // Host type
    
    // Configuration
    openair0_config_t *openair0_cfg;       // Pointer to configuration
    
    // Device private data
    void *priv;                            // Private data for device implementation
    
    // Function pointers for device operations
    trx_start_func_t trx_start_func;       // Start streaming
    trx_stop_func_t trx_stop_func;         // Stop streaming
    trx_write_func_t trx_write_func;       // Write TX samples
    trx_read_func_t trx_read_func;         // Read RX samples
    trx_set_freq_func_t trx_set_freq_func; // Set frequencies
    trx_set_gains_func_t trx_set_gains_func; // Set gains
    trx_end_func_t trx_end_func;           // Cleanup
    
    // Statistics (optional, for monitoring)
    uint64_t tx_count;                     // Total TX samples
    uint64_t rx_count;                     // Total RX samples
    uint64_t tx_errors;                    // TX errors
    uint64_t rx_errors;                    // RX errors
    
} openair0_device_t;

// ========================================
// Logging Macros
// ========================================

// Log levels (simplified)
typedef enum {
    LOG_EMERG = 0,
    LOG_ALERT,
    LOG_CRIT,
    LOG_ERR,
    LOG_WARNING,
    LOG_NOTICE,
    LOG_INFO,
    LOG_DEBUG,
    LOG_TRACE
} log_level_t;

// Component identifiers
typedef enum {
    HW = 0,    // Hardware
    PHY,       // Physical layer
    MAC,       // MAC layer
    RLC,       // RLC layer
    PDCP,      // PDCP layer
    RRC,       // RRC layer
    NAS,       // NAS layer
    MAX_LOG_COMPONENT
} log_component_t;

// Logging macros (simplified for testing)
#define LOG_E(component, format, ...) \
    fprintf(stderr, "[ERROR][%s] " format "\n", #component, ##__VA_ARGS__)

#define LOG_W(component, format, ...) \
    fprintf(stderr, "[WARN][%s] " format "\n", #component, ##__VA_ARGS__)

#define LOG_I(component, format, ...) \
    fprintf(stdout, "[INFO][%s] " format "\n", #component, ##__VA_ARGS__)

#define LOG_D(component, format, ...) \
    fprintf(stdout, "[DEBUG][%s] " format "\n", #component, ##__VA_ARGS__)

// Assert macro with fatal error
#define AssertFatal(condition, format, ...) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "[FATAL] Assertion failed: %s\n", #condition); \
            fprintf(stderr, "[FATAL] " format "\n", ##__VA_ARGS__); \
            fprintf(stderr, "[FATAL] at %s:%d in %s()\n", __FILE__, __LINE__, __func__); \
            abort(); \
        } \
    } while(0)

// Non-fatal assert (just warning)
#define AssertError(condition, format, ...) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "[ERROR] Assertion failed: %s\n", #condition); \
            fprintf(stderr, "[ERROR] " format "\n", ##__VA_ARGS__); \
            fprintf(stderr, "[ERROR] at %s:%d in %s()\n", __FILE__, __LINE__, __func__); \
        } \
    } while(0)

// ========================================
// RF Control Constants (for FlexSDR)
// ========================================

// These match the FlexSDR channel mapping
namespace rfcontrol {
    enum Channel {
        TX1  = 0,
        TX2  = 1,
        RX1  = 2,
        RX2  = 3,
        WRX1 = 4,
        WRX2 = 5
    };
}

// ========================================
// Device Registration API
// ========================================

// Function to register a device (called by device_init in flexsdr_lib.cpp)
typedef int (*device_init_func_t)(openair0_device_t *device, openair0_config_t *openair0_cfg);

// ========================================
// Helper Functions (for testing)
// ========================================

// Initialize a default configuration for testing
static inline void init_default_config(openair0_config_t *cfg) {
    if (!cfg) return;
    
    // Zero out the structure
    memset(cfg, 0, sizeof(openair0_config_t));
    
    // Set default values
    cfg->sample_rate = 30.72e6;  // 30.72 MHz
    cfg->samples_per_frame = 307200;
    
    // TX defaults
    cfg->tx_num_channels = 1;
    cfg->tx_freq[0] = 3.5e9;
    cfg->tx_gain[0] = 90.0;
    cfg->tx_bw = 30e6;
    
    // RX defaults
    cfg->rx_num_channels = 2;
    cfg->rx_freq[0] = 3.5e9;
    cfg->rx_freq[1] = 3.5e9;
    cfg->rx_gain[0] = 60.0;
    cfg->rx_gain[1] = 60.0;
    cfg->rx_bw = 30e6;
    
    // WRX defaults
    cfg->wrx_num_channels = 2;
    cfg->wrx_freq[0] = 2.0e9;
    cfg->wrx_freq[1] = 2.0e9;
    cfg->wrx_gain[0] = 40.0;
    cfg->wrx_gain[1] = 40.0;
    
    // Timing defaults
    cfg->clock_source = 0;  // Internal
    cfg->tx_sample_advance = 166;  // For 30.72 MHz
}

// Print configuration (for debugging)
static inline void print_config(const openair0_config_t *cfg) {
    if (!cfg) return;
    
    printf("\n=== OpenAir0 Configuration ===\n");
    printf("Sample rate: %.2f MHz\n", cfg->sample_rate / 1e6);
    printf("Samples per frame: %.0f\n", cfg->samples_per_frame);
    
    printf("\nTX Configuration:\n");
    printf("  Channels: %d\n", cfg->tx_num_channels);
    for (int i = 0; i < cfg->tx_num_channels; i++) {
        printf("  CH%d: Freq=%.2f GHz, Gain=%.1f dB\n", 
               i, cfg->tx_freq[i] / 1e9, cfg->tx_gain[i]);
    }
    
    printf("\nRX Configuration:\n");
    printf("  Channels: %d\n", cfg->rx_num_channels);
    for (int i = 0; i < cfg->rx_num_channels; i++) {
        printf("  CH%d: Freq=%.2f GHz, Gain=%.1f dB\n", 
               i, cfg->rx_freq[i] / 1e9, cfg->rx_gain[i]);
    }
    
    if (cfg->wrx_num_channels > 0) {
        printf("\nWide RX Configuration:\n");
        printf("  Channels: %d\n", cfg->wrx_num_channels);
        for (int i = 0; i < cfg->wrx_num_channels; i++) {
            printf("  WRX%d: Freq=%.2f GHz, Gain=%.1f dB\n", 
                   i, cfg->wrx_freq[i] / 1e9, cfg->wrx_gain[i]);
        }
    }
    
    printf("===============================\n\n");
}

#ifdef __cplusplus
}
#endif

#endif // COMMON_LIB_H
