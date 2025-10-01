#pragma once

#include <string>
#include <vector>
#include <optional>

namespace flexsdr {
namespace conf {

// -------- Roles -------------------------------------------------------------
enum class Role {
  PrimaryUe,
  PrimaryGnb,
  Ue,
  Gnb
};

inline const char* role_to_string(Role r) {
  switch (r) {
    case Role::PrimaryUe:  return "primary-ue";
    case Role::PrimaryGnb: return "primary-gnb";
    case Role::Ue:         return "ue";
    case Role::Gnb:        return "gnb";
    default:               return "unknown";
  }
}

// -------- EAL config --------------------------------------------------------
struct EalConfig {
  std::string              file_prefix;          // --file-prefix
  std::string              huge_dir;             // --huge-dir
  std::string              socket_mem;           // --socket-mem (e.g. "512,512")
  bool                     no_pci{true};         // --no-pci
  std::string              iova{"va"};           // --iova va|pa

  // Optional knobs referenced by eal_bootstrap.cpp
  std::optional<std::string> lcores;             // --lcores
  std::optional<int>         main_lcore;         // --main-lcore
  std::optional<std::string> socket_limit;       // --socket-limit
};

// -------- Rings / Pools -----------------------------------------------------
struct RingSpec {
  std::string name;
  unsigned    size{512};
};

struct PoolSpec {
  std::string name;        // base name (e.g., "ue_inbound_pool")
  unsigned    size{8192};  // total mbufs
  unsigned    elt_size{2048};
  unsigned    cache_size{256};  // per-lcore cache (new: fixes prior build errors)
};

// -------- Streams -----------------------------------------------------------
struct Stream {
  std::string              mode{"planar"};  // or "interleaved"
  unsigned                 num_channels{1};
  bool                     allow_partial{true};
  unsigned                 timeout_us{10};
  bool                     busy_poll{true};
  std::vector<RingSpec>    rings;
};

// -------- Interconnect (only for primaries) ---------------------------------
struct InterconnectConfig {
  std::vector<RingSpec> rings;  // e.g., pg_to_pu, pu_to_pg
  // (Optionally: pool info if you want a dedicated pool for interconnect)
  std::optional<std::string> pool_name;
  std::optional<unsigned>    pool_size;
  std::optional<unsigned>    pool_elt_size;
  std::optional<unsigned>    pool_cache_size;
};

// -------- Defaults ----------------------------------------------------------
struct DefaultConfig {
  Role        role{Role::Ue};    // overridable via YAML
  unsigned    nb_mbuf{8192};
  unsigned    mp_cache{256};
  unsigned    ring_size{512};
  std::string data_format{"cs16"};

  Stream      tx_stream{};
  Stream      rx_stream{};
  InterconnectConfig interconnect{}; // present in defaults; used by primaries
};

// -------- Per-role config blocks -------------------------------------------
struct RoleConfig {
  std::optional<Stream>            tx_stream;
  std::optional<Stream>            rx_stream;
  std::vector<PoolSpec>            pools;        // creator roles only
  std::optional<InterconnectConfig> interconnect; // primary-gnb may create here
};

// -------- Top-level ---------------------------------------------------------
struct PrimaryConfig {
  EalConfig     eal;
  DefaultConfig defaults;

  std::optional<RoleConfig> primary_ue;
  std::optional<RoleConfig> ue;

  std::optional<RoleConfig> primary_gnb;
  std::optional<RoleConfig> gnb;
};

// YAML loader (implemented in src/conf/config_params.cpp)
int load_from_yaml(const char* path, PrimaryConfig& out);

} // namespace conf
} // namespace flexsdr
