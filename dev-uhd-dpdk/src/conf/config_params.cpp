#include "conf/config_params.hpp"

#include <yaml-cpp/yaml.h>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace flexsdr {
namespace conf {

// --------------------------- small helpers ----------------------------------

static inline unsigned as_u32(const YAML::Node& n, unsigned def) {
  if (!n) return def;
  try {
    return n.as<unsigned>();
  } catch (...) {
    return def;
  }
}

static inline bool as_bool(const YAML::Node& n, bool def) {
  if (!n) return def;
  try {
    return n.as<bool>();
  } catch (...) {
    return def;
  }
}

static inline std::string as_str(const YAML::Node& n, const std::string& def = {}) {
  if (!n) return def;
  try {
    return n.as<std::string>();
  } catch (...) {
    return def;
  }
}

// role <-> string
static inline Role role_from_string(const std::string& s, Role def) {
  if (s == "primary-ue")  return Role::PrimaryUe;
  if (s == "primary-gnb") return Role::PrimaryGnb;
  if (s == "ue")          return Role::Ue;
  if (s == "gnb")         return Role::Gnb;
  return def;
}

// rings
static inline std::vector<RingSpec> parse_ring_list(const YAML::Node& n, unsigned def_size) {
  std::vector<RingSpec> out;
  if (!n || !n.IsSequence()) return out;
  for (const auto& it : n) {
    RingSpec r{};
    r.name = as_str(it["name"]);
    r.size = as_u32(it["size"], def_size);
    if (!r.name.empty())
      out.push_back(r);
  }
  return out;
}

// pools
static inline std::vector<PoolSpec> parse_pool_list(const YAML::Node& n, unsigned def_cache) {
  std::vector<PoolSpec> out;
  if (!n || !n.IsSequence()) return out;
  for (const auto& it : n) {
    PoolSpec p{};
    p.name       = as_str(it["name"]);
    p.size       = as_u32(it["size"],    8192);
    p.elt_size   = as_u32(it["elt_size"], 2048);
    // Allow either explicit cache_size or fall back to defaults.mp_cache
    p.cache_size = as_u32(it["cache_size"], def_cache);
    if (!p.name.empty())
      out.push_back(p);
  }
  return out;
}

// streams (tx/rx)
static inline void parse_stream(const YAML::Node& n, unsigned def_ring_size, Stream& s) {
  if (!n || !n.IsMap()) return;
  if (n["mode"])          s.mode = as_str(n["mode"], s.mode);
  if (n["num_channels"])  s.num_channels = as_u32(n["num_channels"], s.num_channels);
  if (n["allow_partial"]) s.allow_partial = as_bool(n["allow_partial"], s.allow_partial);
  if (n["timeout_us"])    s.timeout_us = as_u32(n["timeout_us"], s.timeout_us);
  if (n["busy_poll"])     s.busy_poll  = as_bool(n["busy_poll"], s.busy_poll);
  if (n["rings"])         s.rings = parse_ring_list(n["rings"], def_ring_size);
}

// interconnect (rings and optional dedicated pool info)
static inline void parse_interconnect(const YAML::Node& n, unsigned def_ring_size, InterconnectConfig& ic) {
  if (!n || !n.IsMap()) return;
  if (n["rings"]) ic.rings = parse_ring_list(n["rings"], def_ring_size);

  if (n["pool_name"])       ic.pool_name       = as_str(n["pool_name"]);
  if (n["pool_size"])       ic.pool_size       = as_u32(n["pool_size"], 0);
  if (n["pool_elt_size"])   ic.pool_elt_size   = as_u32(n["pool_elt_size"], 0);
  if (n["pool_cache_size"]) ic.pool_cache_size = as_u32(n["pool_cache_size"], 0);
}

// role block (primary-ue, ue, primary-gnb, gnb)
static inline void parse_role_block(const YAML::Node& n,
                                    const DefaultConfig& defs,
                                    RoleConfig& rc) {
  if (!n || !n.IsMap()) return;

  if (const auto n_tx = n["tx_stream"]) {
    Stream s = defs.tx_stream;
    parse_stream(n_tx, defs.ring_size, s);
    rc.tx_stream = std::move(s);
  }
  if (const auto n_rx = n["rx_stream"]) {
    Stream s = defs.rx_stream;
    parse_stream(n_rx, defs.ring_size, s);
    rc.rx_stream = std::move(s);
  }
  if (const auto n_pools = n["pools"]) {
    rc.pools = parse_pool_list(n_pools, defs.mp_cache);
  }
  if (const auto n_ic = n["interconnect"]) {
    InterconnectConfig ic{};
    parse_interconnect(n_ic, defs.ring_size, ic);
    // Only set if there’s at least something (rings or pool fields)
    if (!ic.rings.empty() ||
        ic.pool_name.has_value() || ic.pool_size.has_value() ||
        ic.pool_elt_size.has_value() || ic.pool_cache_size.has_value()) {
      rc.interconnect = std::move(ic);
    }
  }
}

// --------------------------- public API -------------------------------------

int load_from_yaml(const char* path, PrimaryConfig& out) {
  try {
    YAML::Node root = YAML::LoadFile(path);

    // ---- eal ---------------------------------------------------------------
    if (const auto neal = root["eal"]; neal && neal.IsMap()) {
      out.eal.file_prefix  = as_str(neal["file_prefix"],  out.eal.file_prefix);
      out.eal.huge_dir     = as_str(neal["huge_dir"],     out.eal.huge_dir);
      out.eal.socket_mem   = as_str(neal["socket_mem"],   out.eal.socket_mem);
      out.eal.no_pci       = as_bool(neal["no_pci"],      out.eal.no_pci);
      out.eal.iova         = as_str(neal["iova"],         out.eal.iova);

      if (neal["lcores"])       out.eal.lcores       = as_str(neal["lcores"]);
      if (neal["main_lcore"])   out.eal.main_lcore   = static_cast<int>(as_u32(neal["main_lcore"], 0));
      if (neal["socket_limit"]) out.eal.socket_limit = as_str(neal["socket_limit"]);
    }

    // ---- defaults ----------------------------------------------------------
    if (const auto ndef = root["defaults"]; ndef && ndef.IsMap()) {
      if (ndef["role"]) {
        const auto r = as_str(ndef["role"]);
        if (!r.empty()) out.defaults.role = role_from_string(r, out.defaults.role);
      }
      if (ndef["nb_mbuf"])    out.defaults.nb_mbuf    = as_u32(ndef["nb_mbuf"],    out.defaults.nb_mbuf);
      if (ndef["mp_cache"])   out.defaults.mp_cache   = as_u32(ndef["mp_cache"],   out.defaults.mp_cache);
      if (ndef["ring_size"])  out.defaults.ring_size  = as_u32(ndef["ring_size"],  out.defaults.ring_size);
      if (ndef["data_format"])out.defaults.data_format= as_str(ndef["data_format"],out.defaults.data_format);

      // defaults.tx_stream / rx_stream
      parse_stream(ndef["tx_stream"], out.defaults.ring_size, out.defaults.tx_stream);
      parse_stream(ndef["rx_stream"], out.defaults.ring_size, out.defaults.rx_stream);

      // defaults.interconnect
      parse_interconnect(ndef["interconnect"], out.defaults.ring_size, out.defaults.interconnect);
    }

    // ---- per-role blocks ---------------------------------------------------
    if (const auto n_pue = root["primary-ue"]; n_pue && n_pue.IsMap()) {
      RoleConfig rc{};
      parse_role_block(n_pue, out.defaults, rc);
      out.primary_ue = std::move(rc);
    }
    if (const auto n_ue = root["ue"]; n_ue && n_ue.IsMap()) {
      RoleConfig rc{};
      parse_role_block(n_ue, out.defaults, rc);
      out.ue = std::move(rc);
    }
    if (const auto n_pgnb = root["primary-gnb"]; n_pgnb && n_pgnb.IsMap()) {
      RoleConfig rc{};
      parse_role_block(n_pgnb, out.defaults, rc);
      out.primary_gnb = std::move(rc);
    }
    if (const auto n_gnb = root["gnb"]; n_gnb && n_gnb.IsMap()) {
      RoleConfig rc{};
      parse_role_block(n_gnb, out.defaults, rc);
      out.gnb = std::move(rc);
    }

    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[config] YAML error: %s\n", e.what());
    return -1;
  } catch (...) {
    std::fprintf(stderr, "[config] Unknown YAML error\n");
    return -2;
  }
}

} // namespace conf
} // namespace flexsdr
