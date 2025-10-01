// src/conf/config_params.cpp  — drop-in replacement

#include "conf/config_params.hpp"   // <-- brings in Role, RingSpec, PoolSpec, PrimaryConfig
#include <yaml-cpp/yaml.h>

#include <cstdio>
#include <string>
#include <vector>

namespace flexsdr {
namespace conf {

// ------------------ small helpers ------------------
static inline unsigned as_u32(const YAML::Node& n, unsigned def = 0u) {
  if (!n) return def;
  try { return n.as<unsigned>(); } catch (...) { return def; }
}

static inline std::string as_str(const YAML::Node& n, const std::string& def = {}) {
  if (!n) return def;
  try { return n.as<std::string>(); } catch (...) { return def; }
}

static inline Role role_from_string(const std::string& s, Role def) {
  // Map common strings to your enum. Adjust if your Role enum uses different names.
  if (s == "primary-ue" || s == "primary") return Role::primary_ue;
  if (s == "ue" || s == "secondary-ue")    return Role::ue;
  if (s == "primary-gnb")                  return Role::primary_gnb;
  if (s == "gnb" || s == "secondary-gnb")  return Role::gnb;
  return def;
}

static inline std::vector<RingSpec> parse_ring_list(const YAML::Node& n, unsigned def_size) {
  std::vector<RingSpec> out;
  if (!n || !n.IsSequence()) return out;

  for (const auto& it : n) {
    RingSpec r{};
    if (it.IsScalar()) {
      r.name = it.as<std::string>();
      r.size = def_size;
    } else if (it.IsMap()) {
      r.name = as_str(it["name"]);
      r.size = as_u32(it["size"], def_size);
    }
    if (!r.name.empty()) out.push_back(r);
  }
  return out;
}

static inline std::vector<PoolSpec> parse_pool_list(const YAML::Node& n) {
  std::vector<PoolSpec> out;
  if (!n || !n.IsSequence()) return out;

  for (const auto& it : n) {
    PoolSpec p{};
    if (it.IsScalar()) {
      p.name     = it.as<std::string>();
      p.n        = 8192;   // defaults
      p.eltsz    = 2048;
      p.cache_sz = 256;
    } else if (it.IsMap()) {
      p.name     = as_str(it["name"]);
      p.n        = as_u32(it["n"],        8192);
      p.eltsz    = as_u32(it["elt_size"], 2048);
      p.cache_sz = as_u32(it["cache"],     256);
    }
    if (!p.name.empty()) out.push_back(p);
  }
  return out;
}

// ------------------ public API ------------------
int load_from_yaml(const char* path, PrimaryConfig& cfg)
{
  try {
    const YAML::Node root = YAML::LoadFile(path ? path : "");
    // defaults
    const auto n_defaults = root["defaults"];
    cfg.defaults.ring_size = as_u32(n_defaults["ring_size"], /*def*/512u);

    // role may be at root or under defaults
    {
      std::string r = as_str(root["role"]);
      if (r.empty()) r = as_str(n_defaults["role"]);
      if (!r.empty()) cfg.defaults.role = role_from_string(r, cfg.defaults.role);
    }

    // primary-ue (optional)
    if (const auto n_pue = root["primary-ue"]; n_pue && n_pue.IsMap()) {
      RoleConfig rc{};
      rc.pools = parse_pool_list(n_pue["pools"]);
      if (const auto n_rings = n_pue["rings"]; n_rings && n_rings.IsMap()) {
        rc.rings.tx = parse_ring_list(n_rings["tx"], cfg.defaults.ring_size);
        rc.rings.rx = parse_ring_list(n_rings["rx"], cfg.defaults.ring_size);
      }
      cfg.primary_ue = std::move(rc);
    }

    // ue (optional)
    if (const auto n_ue = root["ue"]; n_ue && n_ue.IsMap()) {
      RoleConfig rc{};
      if (const auto n_rings = n_ue["rings"]; n_rings && n_rings.IsMap()) {
        rc.rings.tx = parse_ring_list(n_rings["tx"], cfg.defaults.ring_size);
        rc.rings.rx = parse_ring_list(n_rings["rx"], cfg.defaults.ring_size);
      }
      cfg.ue = std::move(rc);
    }

    // primary-gnb (optional)
    if (const auto n_pg = root["primary-gnb"]; n_pg && n_pg.IsMap()) {
      RoleConfig rc{};
      rc.pools = parse_pool_list(n_pg["pools"]);
      if (const auto n_rings = n_pg["rings"]; n_rings && n_rings.IsMap()) {
        rc.rings.tx = parse_ring_list(n_rings["tx"], cfg.defaults.ring_size);
        rc.rings.rx = parse_ring_list(n_rings["rx"], cfg.defaults.ring_size);
      }
      cfg.primary_gnb = std::move(rc);
    }

    // gnb (optional)
    if (const auto n_g = root["gnb"]; n_g && n_g.IsMap()) {
      RoleConfig rc{};
      if (const auto n_rings = n_g["rings"]; n_rings && n_rings.IsMap()) {
        rc.rings.tx = parse_ring_list(n_rings["tx"], cfg.defaults.ring_size);
        rc.rings.rx = parse_ring_list(n_rings["rx"], cfg.defaults.ring_size);
      }
      cfg.gnb = std::move(rc);
    }

    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[cfg] YAML parse failed for '%s': %s\n", path ? path : "<null>", e.what());
    return -1;
  } catch (...) {
    std::fprintf(stderr, "[cfg] YAML parse failed for '%s' (unknown error)\n", path ? path : "<null>");
    return -2;
  }
}

} // namespace conf
} // namespace flexsdr
