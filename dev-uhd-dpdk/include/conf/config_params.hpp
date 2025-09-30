#pragma once
//
// config_params.hpp
//
// YAML-driven configuration for FlexSDR primary/secondary apps,
// plus helpers to create/validate DPDK rings & pools.
//
// Requires: yaml-cpp, DPDK (rte_eal, rte_ring, rte_mempool, rte_mbuf)
// C++17
//

#include <string>
#include <vector>
#include <cstdint>
#include <optional>

namespace flexsdr {

// -----------------------------
// Basic config structs
// -----------------------------

struct EALConfig {
  std::string file_prefix;
  std::string huge_dir;
  std::string socket_mem;
  bool        no_pci = true;
  std::string iova; // "va" or "PA"
};

struct StreamConfig {
  // "ring" in YAML can be scalar or sequence; we store normalized as a list.
  std::vector<std::string> rings;
  std::string mode;            // "Planar" | "interleaved"
  uint32_t    spp           = 0;
  uint32_t    num_channels  = 0;
  bool        allow_partial = true;
  uint32_t    timeout_us    = 0;
  bool        busy_poll     = false;
  uint32_t    burst_dequeue = 0;
  uint32_t    ring_watermark= 0;

  // Convenience helpers
  bool        has_single_ring() const { return rings.size() == 1; }
  const std::string& single_ring() const { return rings.at(0); }
};

struct DefaultConfig {
  uint32_t nb_mbuf   = 8192;
  uint32_t mp_cache  = 256;
  uint32_t ring_size = 512;
  std::string data_format; // "cs16" etc.

  StreamConfig tx_stream;
  StreamConfig rx_stream;
};

struct PoolConf {
  std::string name;
  uint32_t    size     = 0;   // number of mbufs/elements
  uint32_t    elt_size = 0;   // data room size for pktmbuf pools
};

struct RingConf {
  std::string name;
  uint32_t    size = 0;
};

struct PrimaryConfig {
  std::vector<PoolConf> pools;
  std::vector<RingConf> rings;
};

struct TestTransmit {
  bool     enabled     = false;
  uint32_t pps         = 0;   // 0 => let spp/burst drive
  uint32_t burst_size  = 0;
  uint32_t duration_s  = 0;
};

struct AppConfig {
  // RX path (into the app)
  std::string            inbound_ring;
  std::string            rx_pool;
  std::vector<int>       rx_cores;
  StreamConfig           rx_stream;

  // TX path (out of the app)
  std::string            tx_pool;
  std::vector<int>       tx_cores;
  StreamConfig           tx_stream;

  TestTransmit           test_tx;
};

struct FullConfig {
  EALConfig    eal;
  DefaultConfig     defaults;
  PrimaryConfig primary;
  AppConfig    ue_app;
  AppConfig    gnb_app;
};

// -----------------------------
// Loaders (parse YAML)
// -----------------------------

// Load entire file (eal, defaults, primary, ue-app, gnb-app).
FullConfig load_full_config(const std::string& yaml_file);

// Convenience: load just sections
PrimaryConfig load_primary_conf_params(const std::string& yaml_file);
AppConfig    load_ue_config_params(const std::string& yaml_file);
AppConfig    load_gnb_conf_params(const std::string& yaml_file);
EALConfig    load_eal_params(const std::string& yaml_file);
DefaultConfig     load_defaults(const std::string& yaml_file);

// -----------------------------
// DPDK helpers (optional)
// -----------------------------
//
// create_primary_resources:
//   - Creates pktmbuf pools for each PrimaryConfig::pools (data room = elt_size)
//   - Creates rings for each PrimaryConfig::rings
//
// validate_secondary_resources:
//   - Verifies presence of rings/pools needed by a secondary app (ue/gnb)
//
// NOTE: Call these *after* rte_eal_init().
// Returns true on success.
//
bool create_primary_resources(const PrimaryConfig& primary,
                              unsigned socket_id,
                              unsigned mp_cache_default,
                              bool allow_existing = true,
                              unsigned ring_flags = 0 /* RING_F_SP_ENQ|RING_F_SC_DEQ is a good default for SPSC */);

bool validate_secondary_resources(const AppConfig& app,
                                  bool check_rx = true,
                                  bool check_tx = true);

// -----------------------------
// Pretty-print (for logs)
// -----------------------------
std::string summarize(const EALConfig& c);
std::string summarize(const DefaultConfig& d);
std::string summarize(const StreamConfig& s);
std::string summarize(const PrimaryConfig& p);
std::string summarize(const AppConfig& a);
std::string summarize(const FullConfig& f);

} // namespace flexsdr
