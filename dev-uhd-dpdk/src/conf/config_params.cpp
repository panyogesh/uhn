#include "conf/config_params.hpp"
#include <yaml-cpp/yaml.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace flexsdr { namespace conf {

// ---------------- helpers ----------------
static inline unsigned as_u32(const YAML::Node& n, unsigned def) {
  return (n && n.IsScalar()) ? n.as<unsigned>() : def;
}
static inline bool as_bool(const YAML::Node& n, bool def) {
  return (n && n.IsScalar()) ? n.as<bool>() : def;
}
static inline std::string as_str(const YAML::Node& n, const std::string& def = {}) {
  return (n && n.IsScalar()) ? n.as<std::string>() : def;
}

static inline std::string role_from_string(const std::string& s, const std::string& def) {
  // Keep as string; if you want strict validation, add it here.
  return s.empty() ? def : s;
}

static inline std::vector<RingSpec> parse_ring_list(const YAML::Node& n, unsigned def_size) {
  std::vector<RingSpec> out;
  if (!n || !n.IsSequence()) return out;
  for (const auto& it : n) {
    RingSpec r{};
    r.name = as_str(it["name"]);
    r.size = as_u32(it["size"], def_size);
    if (!r.name.empty()) out.push_back(r);
  }
  return out;
}

static inline Stream parse_stream(const YAML::Node& n, const Stream& def, unsigned def_ring_size) {
  Stream s = def;
  if (!n || !n.IsMap()) return s;

  s.mode          = as_str(n["mode"],          def.mode);
  s.num_channels  = as_u32(n["num_channels"],  def.num_channels);
  s.allow_partial = as_bool(n["allow_partial"], def.allow_partial);
  s.timeout_us    = as_u32(n["timeout_us"],    def.timeout_us);
  s.busy_poll     = as_bool(n["busy_poll"],     def.busy_poll);

  if (const auto nr = n["rings"]; nr) {
    s.rings = parse_ring_list(nr, def_ring_size);
  }
  return s;
}

static inline std::vector<PoolSpec> parse_pool_list(const YAML::Node& n) {
  std::vector<PoolSpec> out;
  if (!n || !n.IsSequence()) return out;
  for (const auto& it : n) {
    PoolSpec p{};
    p.name     = as_str(it["name"]);
    p.size     = as_u32(it["size"], 8192);
    p.elt_size = as_u32(it["elt_size"], 2048);
    p.cache    = as_u32(it["cache"], 0); // 0 => use defaults.mp_cache at runtime
    if (!p.name.empty()) out.push_back(p);
  }
  return out;
}

static inline std::optional<InterconnectConfig> parse_interconnect(const YAML::Node& n, unsigned def_ring_size) {
  if (!n || !n.IsMap()) return std::nullopt;
  InterconnectConfig ic{};
  ic.rings = parse_ring_list(n["rings"], def_ring_size);
  return ic;
}

static inline std::optional<RoleConfig> parse_role(const YAML::Node& rn, const DefaultConfig& def) {
  if (!rn || !rn.IsMap()) return std::nullopt;
  RoleConfig rc{};

  // pools (creator roles only)
  rc.pools = parse_pool_list(rn["pools"]);

  // tx_stream / rx_stream (override defaults if present)
  if (const auto ntx = rn["tx_stream"]; ntx && ntx.IsMap())
    rc.tx_stream = parse_stream(ntx, def.tx_stream, def.ring_size);
  if (const auto nrx = rn["rx_stream"]; nrx && nrx.IsMap())
    rc.rx_stream = parse_stream(nrx, def.rx_stream, def.ring_size);

  // interconnect
  if (const auto ni = rn["interconnect"]; ni && ni.IsMap())
    rc.interconnect = parse_interconnect(ni, def.ring_size);

  return rc;
}

// --------------- loader -------------------
int load_from_yaml(const char* path, PrimaryConfig& cfg) {
  try {
    YAML::Node root = YAML::LoadFile(path);
    if (!root || !root.IsMap()) {
      std::fprintf(stderr, "[cfg] YAML root not a map: %s\n", path ? path : "<null>");
      return -1;
    }

    // ---- eal ----
    if (const auto ne = root["eal"]; ne && ne.IsMap()) {
      cfg.eal.file_prefix = as_str(ne["file_prefix"], cfg.eal.file_prefix);
      cfg.eal.huge_dir    = as_str(ne["huge_dir"], cfg.eal.huge_dir);
      cfg.eal.socket_mem  = as_str(ne["socket_mem"], cfg.eal.socket_mem);
      cfg.eal.no_pci      = as_bool(ne["no_pci"], cfg.eal.no_pci);
      cfg.eal.iova        = as_str(ne["iova"], cfg.eal.iova);

      // optional knobs used by EalBootstrap
      if (ne["lcores"])       cfg.eal.lcores       = as_str(ne["lcores"]);
      if (ne["main_lcore"])   cfg.eal.main_lcore   = static_cast<int>(as_u32(ne["main_lcore"], 0));
      if (ne["socket_limit"]) cfg.eal.socket_limit = as_str(ne["socket_limit"]);
    }

    // ---- defaults ----
    if (const auto nd = root["defaults"]; nd && nd.IsMap()) {
      if (const auto nrole = nd["role"]; nrole && nrole.IsScalar()) {
        const auto r = nrole.as<std::string>();
        cfg.defaults.role = role_from_string(r, cfg.defaults.role);
      }
      cfg.defaults.nb_mbuf     = as_u32(nd["nb_mbuf"],     cfg.defaults.nb_mbuf);
      cfg.defaults.mp_cache    = as_u32(nd["mp_cache"],    cfg.defaults.mp_cache);
      cfg.defaults.ring_size   = as_u32(nd["ring_size"],   cfg.defaults.ring_size);
      cfg.defaults.data_format = as_str(nd["data_format"], cfg.defaults.data_format);

      // stream defaults
      cfg.defaults.tx_stream = parse_stream(nd["tx_stream"], cfg.defaults.tx_stream, cfg.defaults.ring_size);
      cfg.defaults.rx_stream = parse_stream(nd["rx_stream"], cfg.defaults.rx_stream, cfg.defaults.ring_size);

      // interconnect defaults
      if (const auto ni = nd["interconnect"]; ni && ni.IsMap()) {
        cfg.defaults.interconnect.rings = parse_ring_list(ni["rings"], cfg.defaults.ring_size);
      }
    }

    // ---- per-role blocks (optional) ----
    if (const auto n_pue = root["primary-ue"]; n_pue)  cfg.primary_ue  = parse_role(n_pue, cfg.defaults);
    if (const auto n_ue  = root["ue"];         n_ue)   cfg.ue          = parse_role(n_ue,  cfg.defaults);

    if (const auto n_pgnb = root["primary-gnb"]; n_pgnb) cfg.primary_gnb = parse_role(n_pgnb, cfg.defaults);
    if (const auto n_gnb  = root["gnb"];         n_gnb)  cfg.gnb         = parse_role(n_gnb,  cfg.defaults);

    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[cfg] exception while parsing %s: %s\n", path ? path : "<null>", e.what());
    return -2;
  }
}

}} // namespace flexsdr::conf
