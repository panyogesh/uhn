#pragma once

#include <string>
#include <vector>
#include <optional>

namespace flexsdr { namespace conf {

// ----------------------
// EAL (DPDK) parameters
// ----------------------
struct EalConfig {
  std::string file_prefix;      // e.g. "flexsdr-app"
  std::string huge_dir;         // e.g. "/dev/hugepages"
  std::string socket_mem;       // e.g. "512,512"
  bool        no_pci{true};
  std::string iova;             // "va" or "pa"

  // Optional knobs that EalBootstrap may emit
  std::optional<std::string> lcores;       // "--lcores"
  std::optional<int>         main_lcore;   // "--main-lcore"
  std::optional<std::string> socket_limit; // "--socket-limit"
};

// ----------------------
// Ring / Pool metadata
// ----------------------
struct RingSpec {
  std::string name;
  unsigned    size{0};
};

struct PoolSpec {
  std::string name;      // e.g., "ue_inbound_pool"
  unsigned    size{8192};
  unsigned    elt_size{2048};
  unsigned    cache{0};  // 0 => use defaults.mp_cache
};

// ----------------------
// Per-stream defaults
// ----------------------
struct Stream {
  std::string mode;          // "planar" | "interleaved"
  unsigned    num_channels{0};
  bool        allow_partial{true};
  unsigned    timeout_us{0};
  bool        busy_poll{false};
  std::vector<RingSpec> rings;  // TX/RX ring list
};

// ----------------------
// Interconnect (PG ⇄ PU)
// ----------------------
struct InterconnectConfig {
  std::vector<RingSpec> rings;  // e.g., {pg_to_pu, pu_to_pg}
};

// ----------------------
// Global defaults block
// ----------------------
struct DefaultConfig {
  std::string role;        // keep as string for clean prints
  unsigned    nb_mbuf{8192};
  unsigned    mp_cache{256};
  unsigned    ring_size{512};
  std::string data_format; // e.g., "cs16"

  Stream              tx_stream;
  Stream              rx_stream;
  InterconnectConfig  interconnect;
};

// ----------------------
// Per-role configuration
// ----------------------
struct RoleConfig {
  std::vector<PoolSpec>             pools;        // creator roles
  std::optional<Stream>             tx_stream;
  std::optional<Stream>             rx_stream;
  std::optional<InterconnectConfig> interconnect;
};

// ----------------------
// Top-level configuration
// ----------------------
struct PrimaryConfig {
  EalConfig      eal;
  DefaultConfig  defaults;

  // UE side
  std::optional<RoleConfig> primary_ue;
  std::optional<RoleConfig> ue;

  // gNB side (harmless if absent in UE YAML)
  std::optional<RoleConfig> primary_gnb;
  std::optional<RoleConfig> gnb;
};

// YAML loader
int load_from_yaml(const char* path, PrimaryConfig& out);

}} // namespace flexsdr::conf
