#include "conf/config_params.hpp"
#include <yaml-cpp/yaml.h>
#include <type_traits>
#include <sstream>
#include <iostream>
#include <algorithm>

using namespace flexsdr::conf;

// ---------- helpers ----------
namespace {

template <typename T>
T get_or(const YAML::Node& n, const char* key, T def) {
  if (!n || !n[key]) return def;
  if constexpr (std::is_same_v<T, std::string>) return n[key].as<std::string>();
  else                                          return n[key].as<T>();
}

template <typename T>
std::optional<T> get_opt(const YAML::Node& n, const char* key) {
  if (!n || !n[key]) return std::nullopt;
  return n[key].as<T>();
}

DataFormat parse_format(const YAML::Node& n, const char* key, DataFormat def) {
  const auto s = get_or<std::string>(n, key, "");
  if (s == "cs16" || s == "i16q16" || s == "iq16") return DataFormat::cs16;
  if (s == "cf32" || s == "f32")                   return DataFormat::cf32;
  return def;
}

LayoutMode parse_layout(const YAML::Node& n, const char* key, LayoutMode def) {
  const auto s = get_or<std::string>(n, key, "");
  if (s == "planar")                                return LayoutMode::planar;
  if (s == "interleaved" || s == "interleave")      return LayoutMode::interleaved;
  return def;
}

Role parse_role(const YAML::Node& n, const char* key, Role def) {
  const auto s = get_or<std::string>(n, key, "");
  if (s == "primary-ue")   return Role::primary_ue;
  if (s == "primary-gnb")  return Role::primary_gnb;
  if (s == "ue")           return Role::ue;
  if (s == "gnb")          return Role::gnb;
  return def;
}

RingSpec parse_ring_spec(const YAML::Node& rn) {
  RingSpec rs;
  if (rn.IsScalar()) {
    rs.name = rn.as<std::string>();
    rs.size = 0;
  } else {
    rs.name = get_or<std::string>(rn, "name", "");
    rs.size = get_or<unsigned>(rn, "size", 0u);
  }
  if (rs.name.empty()) throw std::runtime_error("RingSpec missing 'name'");
  return rs;
}

PoolSpec parse_pool_spec(const YAML::Node& pn) {
  PoolSpec ps;
  ps.name     = get_or<std::string>(pn, "name", "");
  ps.size     = get_or<unsigned>(pn, "size", 8192u);
  ps.elt_size = get_or<unsigned>(pn, "elt_size", 2048u);
  if (ps.name.empty()) throw std::runtime_error("PoolSpec missing 'name'");
  return ps;
}

std::vector<RingSpec> parse_rings_seq(const YAML::Node& n) {
  std::vector<RingSpec> out;
  if (!n) return out;
  if (!n.IsSequence()) throw std::runtime_error("'rings' must be a sequence");
  out.reserve(n.size());
  for (auto it : n) out.emplace_back(parse_ring_spec(it));
  return out;
}

Stream parse_stream(const YAML::Node& n, const Stream& seed) {
  Stream s = seed;
  if (!n) return s;
  s.mode          = parse_layout(n, "mode", s.mode);
  s.num_channels  = get_or<unsigned>(n, "num_channels", s.num_channels);
  s.allow_partial = get_or<bool>(n, "allow_partial", s.allow_partial);
  s.timeout_us    = get_or<uint32_t>(n, "timeout_us", s.timeout_us);
  s.busy_poll     = get_or<bool>(n, "busy_poll", s.busy_poll);
  s.high_watermark_pct = get_opt<unsigned>(n, "high_watermark_pct");
  s.hard_drop_pct      = get_opt<unsigned>(n, "hard_drop_pct");

  s.rings.clear();
  if (n["rings"]) {
    s.rings = parse_rings_seq(n["rings"]);
  } else if (n["ring"]) {
    if (!n["ring"].IsSequence()) throw std::runtime_error("'ring' must be a sequence");
    for (auto it : n["ring"]) s.rings.emplace_back(parse_ring_spec(it));
  }
  return s;
}

RoleConfig parse_role_block(const YAML::Node& n, const DefaultConfig& dflt) {
  RoleConfig rc;
  if (!n) return rc;
  if (n["pools"]) {
    if (!n["pools"].IsSequence()) throw std::runtime_error("'pools' must be a sequence");
    for (auto it : n["pools"]) rc.pools.emplace_back(parse_pool_spec(it));
  }
  if (n["tx_stream"]) rc.tx_stream = parse_stream(n["tx_stream"], dflt.tx_stream);
  if (n["rx_stream"]) rc.rx_stream = parse_stream(n["rx_stream"], dflt.rx_stream);

  if (n["interconnect"]) {
    InterconnectConfig ic;
    if (n["interconnect"]["rings"])
      ic.rings = parse_rings_seq(n["interconnect"]["rings"]);
    rc.interconnect = ic;
  }
  return rc;
}

} // namespace

// ---------- PrimaryConfig::load ----------
PrimaryConfig PrimaryConfig::load(const std::string& yaml_path) {
  YAML::Node root = YAML::LoadFile(yaml_path);
  if (!root) throw std::runtime_error("Failed to load YAML: " + yaml_path);

  PrimaryConfig cfg;

  // eal
  if (auto e = root["eal"]) {
    cfg.eal.file_prefix = get_or<std::string>(e, "file_prefix", cfg.eal.file_prefix);
    cfg.eal.huge_dir    = get_or<std::string>(e, "huge_dir",    cfg.eal.huge_dir);
    cfg.eal.socket_mem  = get_or<std::string>(e, "socket_mem",  cfg.eal.socket_mem);
    cfg.eal.no_pci      = get_or<bool>(e,        "no_pci",      cfg.eal.no_pci);
    cfg.eal.iova        = get_or<std::string>(e, "iova",        cfg.eal.iova);
    cfg.eal.main_lcore  = get_opt<int>(e,                "main_lcore");
    cfg.eal.lcores      = get_opt<std::string>(e,        "lcores");
    cfg.eal.isolcpus    = get_opt<std::string>(e,        "isolcpus");
    cfg.eal.numa        = get_opt<bool>(e,               "numa");
    cfg.eal.socket_limit= get_opt<std::string>(e,        "socket_limit");
  }

  // defaults
  if (auto d = root["defaults"]) {
    cfg.defaults.nb_mbuf      = get_or<unsigned>(d, "nb_mbuf", cfg.defaults.nb_mbuf);
    cfg.defaults.mp_cache     = get_or<unsigned>(d, "mp_cache", cfg.defaults.mp_cache);
    cfg.defaults.ring_size    = get_or<unsigned>(d, "ring_size", cfg.defaults.ring_size);
    cfg.defaults.data_format  = parse_format(d, "data_format", cfg.defaults.data_format);
    cfg.defaults.role         = parse_role(d, "role", cfg.defaults.role);
    if (d["tx_stream"]) cfg.defaults.tx_stream = parse_stream(d["tx_stream"], cfg.defaults.tx_stream);
    if (d["rx_stream"]) cfg.defaults.rx_stream = parse_stream(d["rx_stream"], cfg.defaults.rx_stream);
    if (d["interconnect"] && d["interconnect"]["rings"])
      cfg.defaults.interconnect.rings = parse_rings_seq(d["interconnect"]["rings"]);
  }

  // role blocks
  cfg.primary_ue  = root["primary-ue"]  ? std::optional<RoleConfig>(parse_role_block(root["primary-ue"],  cfg.defaults)) : std::nullopt;
  cfg.primary_gnb = root["primary-gnb"] ? std::optional<RoleConfig>(parse_role_block(root["primary-gnb"], cfg.defaults)) : std::nullopt;
  cfg.ue          = root["ue"]          ? std::optional<RoleConfig>(parse_role_block(root["ue"],          cfg.defaults)) : std::nullopt;
  cfg.gnb         = root["gnb"]         ? std::optional<RoleConfig>(parse_role_block(root["gnb"],         cfg.defaults)) : std::nullopt;

  return cfg;
}

// ---------- effective getters ----------
static const RoleConfig* pick_role(const PrimaryConfig& c, Role r) {
  switch (r) {
    case Role::primary_ue:  return c.primary_ue  ? &*c.primary_ue  : nullptr;
    case Role::primary_gnb: return c.primary_gnb ? &*c.primary_gnb : nullptr;
    case Role::ue:          return c.ue          ? &*c.ue          : nullptr;
    case Role::gnb:         return c.gnb         ? &*c.gnb         : nullptr;
  }
  return nullptr;
}

const Stream& PrimaryConfig::effective_tx_stream() const {
  if (auto rc = pick_role(*this, defaults.role)) if (rc->tx_stream) return *rc->tx_stream;
  return defaults.tx_stream;
}

const Stream& PrimaryConfig::effective_rx_stream() const {
  if (auto rc = pick_role(*this, defaults.role)) if (rc->rx_stream) return *rc->rx_stream;
  return defaults.rx_stream;
}

InterconnectConfig PrimaryConfig::effective_interconnect() const {
  InterconnectConfig out = defaults.interconnect; // seed with defaults
  if (auto rc = pick_role(*this, defaults.role)) {
    if (rc->interconnect && !rc->interconnect->rings.empty())
      out.rings = rc->interconnect->rings;
  }
  return out;
}

const std::vector<PoolSpec>& PrimaryConfig::effective_pools() const {
  static const std::vector<PoolSpec> kEmpty;
  if (auto rc = pick_role(*this, defaults.role)) return rc->pools;
  return kEmpty;
}

// ---------- materialization: "<role>_<base>" ----------
std::string PrimaryConfig::materialize_name(const std::string& base) const {
  // No automatic prefix/suffix anymore. YAML provides the full object names.
  return base;
}

std::vector<RingSpec> PrimaryConfig::materialized_tx_rings() const {
  std::vector<RingSpec> out;
  const auto& st = effective_tx_stream();
  out.reserve(st.rings.size());
  for (const auto& r : st.rings) {
    RingSpec rs = r;
    rs.name = materialize_name(r.name);
    if (rs.size == 0) rs.size = defaults.ring_size;
    out.emplace_back(std::move(rs));
  }
  return out;
}

std::vector<RingSpec> PrimaryConfig::materialized_rx_rings() const {
  std::vector<RingSpec> out;
  const auto& st = effective_rx_stream();
  out.reserve(st.rings.size());
  for (const auto& r : st.rings) {
    RingSpec rs = r;
    rs.name = materialize_name(r.name);
    if (rs.size == 0) rs.size = defaults.ring_size;
    out.emplace_back(std::move(rs));
  }
  return out;
}

std::vector<RingSpec> PrimaryConfig::materialized_interconnect_rings() const {
  std::vector<RingSpec> out;
  const auto ic = effective_interconnect();
  out.reserve(ic.rings.size());
  for (const auto& r : ic.rings) {
    RingSpec rs = r;
    rs.name = materialize_name(r.name);
    if (rs.size == 0) rs.size = defaults.ring_size;
    out.emplace_back(std::move(rs));
  }
  return out;
}

// ---------- to_string ----------
std::string PrimaryConfig::to_string(Role r) {
  switch (r) {
    case Role::primary_ue:  return "primary-ue";
    case Role::primary_gnb: return "primary-gnb";
    case Role::ue:          return "ue";
    case Role::gnb:         return "gnb";
  }
  return "unknown";
}
std::string PrimaryConfig::to_string(DataFormat f) {
  switch (f) { case DataFormat::cs16: return "cs16"; case DataFormat::cf32: return "cf32"; }
  return "unknown";
}
std::string PrimaryConfig::to_string(LayoutMode m) {
  switch (m) { case LayoutMode::planar: return "planar"; case LayoutMode::interleaved: return "interleaved"; }
  return "unknown";
}

// ---------- printers ----------
std::ostream& flexsdr::conf::operator<<(std::ostream& os, const RingSpec& r) {
  return os << "{name=" << r.name << ", size=" << r.size << "}";
}
std::ostream& flexsdr::conf::operator<<(std::ostream& os, const PoolSpec& p) {
  return os << "{name=" << p.name << ", size=" << p.size << ", elt_size=" << p.elt_size << "}";
}
std::ostream& flexsdr::conf::operator<<(std::ostream& os, const EalConfig& e) {
  os << "EAL{file_prefix=" << e.file_prefix
     << ", huge_dir=" << e.huge_dir
     << ", socket_mem=" << e.socket_mem
     << ", no_pci=" << (e.no_pci ? "true" : "false")
     << ", iova=" << e.iova;
  if (e.main_lcore)   os << ", main_lcore=" << *e.main_lcore;
  if (e.lcores)       os << ", lcores=" << *e.lcores;
  if (e.isolcpus)     os << ", isolcpus=" << *e.isolcpus;
  if (e.numa)         os << ", numa=" << (*e.numa ? "true" : "false");
  if (e.socket_limit) os << ", socket_limit=" << *e.socket_limit;
  return os << "}";
}
std::ostream& flexsdr::conf::operator<<(std::ostream& os, const Stream& s) {
  os << "Stream{mode=" << PrimaryConfig::to_string(s.mode)
     << ", num_channels=" << s.num_channels
     << ", allow_partial=" << (s.allow_partial ? "true" : "false")
     << ", timeout_us=" << s.timeout_us
     << ", busy_poll=" << (s.busy_poll ? "true" : "false");
  if (s.high_watermark_pct) os << ", high_wm%=" << *s.high_watermark_pct;
  if (s.hard_drop_pct)      os << ", hard_drop%=" << *s.hard_drop_pct;
  os << ", rings=[";
  for (std::size_t i = 0; i < s.rings.size(); ++i) { if (i) os << ","; os << s.rings[i]; }
  return os << "]}";
}
std::ostream& flexsdr::conf::operator<<(std::ostream& os, const InterconnectConfig& ic) {
  os << "Interconnect{rings=[";
  for (std::size_t i = 0; i < ic.rings.size(); ++i) { if (i) os << ","; os << ic.rings[i]; }
  return os << "]}";
}
std::ostream& flexsdr::conf::operator<<(std::ostream& os, const DefaultConfig& d) {
  os << "Defaults{nb_mbuf=" << d.nb_mbuf
     << ", mp_cache=" << d.mp_cache
     << ", ring_size=" << d.ring_size
     << ", data_format=" << PrimaryConfig::to_string(d.data_format)
     << ", role=" << PrimaryConfig::to_string(d.role)
     << ", tx=" << d.tx_stream
     << ", rx=" << d.rx_stream
     << ", interconnect=" << d.interconnect
     << "}";
  return os;
}
std::ostream& flexsdr::conf::operator<<(std::ostream& os, const RoleConfig& rc) {
  os << "RoleConfig{";
  if (rc.tx_stream) os << "tx=" << *rc.tx_stream << ", ";
  if (rc.rx_stream) os << "rx=" << *rc.rx_stream << ", ";
  os << "pools=[";
  for (std::size_t i = 0; i < rc.pools.size(); ++i) { if (i) os << ","; os << rc.pools[i]; }
  os << "]";
  if (rc.interconnect) os << ", interconnect=" << *rc.interconnect;
  return os << "}";
}
std::ostream& flexsdr::conf::operator<<(std::ostream& os, const PrimaryConfig& c) {
  os << c.eal << "\n" << c.defaults << "\n";
  os << "primary-ue=";  if (c.primary_ue)  os << *c.primary_ue;  else os << "{}"; os << "\n";
  os << "primary-gnb="; if (c.primary_gnb) os << *c.primary_gnb; else os << "{}"; os << "\n";
  os << "ue=";          if (c.ue)          os << *c.ue;          else os << "{}"; os << "\n";
  os << "gnb=";         if (c.gnb)         os << *c.gnb;         else os << "{}"; os << "\n";
  return os;
}
