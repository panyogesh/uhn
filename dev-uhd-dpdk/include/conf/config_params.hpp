#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <stdexcept>
#include <ostream>

namespace flexsdr::conf {

// ---------- enums ----------
enum class DataFormat { cs16, cf32 };
enum class LayoutMode { planar, interleaved };
enum class Role { primary_ue, primary_gnb, ue, gnb };

// ---------- small specs ----------
struct RingSpec {
  std::string name;      // base name from YAML (e.g., "tx_ch1")
  unsigned    size{0};   // 0 => use defaults.ring_size
};

struct PoolSpec {
  std::string name;      // base name (e.g., "inbound_pool")
  unsigned    size{8192};
  unsigned    elt_size{2048};
};

// ---------- EAL / naming ----------
struct EalConfig {
  std::string file_prefix = "flexsdr-app";
  std::string huge_dir    = "/dev/hugepages";
  std::string socket_mem  = "512,512";
  bool        no_pci      = true;
  std::string iova        = "va";

  // Optional RT/NUMA extras
  std::optional<int>         main_lcore;
  std::optional<std::string> lcores;
  std::optional<std::string> isolcpus;
  std::optional<bool>        numa;
  std::optional<std::string> socket_limit;
};

struct NamingPolicy {
  bool        prefix_with_role{true};
  std::string separator{"_"};
  std::string materialize(Role role, const std::string& base) const; // "<role><sep><base>"
};

// ---------- streams / defaults ----------
struct Stream {
  LayoutMode  mode{LayoutMode::planar};
  unsigned    num_channels{2};
  bool        allow_partial{true};
  uint32_t    timeout_us{10};
  bool        busy_poll{true};
  std::optional<unsigned> high_watermark_pct; // %
  std::optional<unsigned> hard_drop_pct;      // %
  std::vector<RingSpec>   rings{};
};

struct InterconnectConfig {
  std::vector<RingSpec> rings{};  // e.g., [{ name: "pg_to_pu", size: 512 }, ...]
};

struct DefaultConfig {
  unsigned    nb_mbuf{8192};
  unsigned    mp_cache{256};
  unsigned    ring_size{512};
  DataFormat  data_format{DataFormat::cs16};
  Role        role{Role::primary_ue};

  Stream              tx_stream{};
  Stream              rx_stream{};
  InterconnectConfig  interconnect{};
};

// ---------- per-role override block ----------
struct RoleConfig {
  std::optional<Stream>          tx_stream;
  std::optional<Stream>          rx_stream;
  std::vector<PoolSpec>          pools;        // creator roles
  std::optional<InterconnectConfig> interconnect;
};

// ---------- root config ----------
struct PrimaryConfig {
  EalConfig      eal{};
  NamingPolicy   naming{};
  DefaultConfig  defaults{};

  // role blocks (each file will use the ones it needs)
  std::optional<RoleConfig> primary_ue;
  std::optional<RoleConfig> primary_gnb;
  std::optional<RoleConfig> ue;
  std::optional<RoleConfig> gnb;

  // Load from YAML file path
  static PrimaryConfig load(const std::string& yaml_path);

  // Effective configs for the current defaults.role
  const Stream&             effective_tx_stream() const;
  const Stream&             effective_rx_stream() const;
  InterconnectConfig        effective_interconnect() const; // returns a value (merged)
  const std::vector<PoolSpec>& effective_pools() const;

  // Materialized (prefixed) ring names with size fallback
  std::vector<RingSpec> materialized_tx_rings() const;
  std::vector<RingSpec> materialized_rx_rings() const;
  std::vector<RingSpec> materialized_interconnect_rings() const;

  // Helpers
  static std::string to_string(Role r);
  static std::string to_string(DataFormat f);
  static std::string to_string(LayoutMode m);
};

// ---------- printers ----------
std::ostream& operator<<(std::ostream&, const RingSpec&);
std::ostream& operator<<(std::ostream&, const PoolSpec&);
std::ostream& operator<<(std::ostream&, const EalConfig&);
std::ostream& operator<<(std::ostream&, const NamingPolicy&);
std::ostream& operator<<(std::ostream&, const Stream&);
std::ostream& operator<<(std::ostream&, const InterconnectConfig&);
std::ostream& operator<<(std::ostream&, const DefaultConfig&);
std::ostream& operator<<(std::ostream&, const RoleConfig&);
std::ostream& operator<<(std::ostream&, const PrimaryConfig&);

} // namespace flexsdr::conf
