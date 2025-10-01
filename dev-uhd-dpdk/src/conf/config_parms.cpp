#include "conf/config_params.hpp"
#include <yaml-cpp/yaml.h>
#include <type_traits>
#include <sstream>
#include <iostream>

using namespace flexsdr::conf;

// ------------------------- helpers -------------------------

namespace {

// Safe fetch with default
template <typename T>
T get_or(const YAML::Node& n, const char* key, T def) {
  if (!n || !n[key]) return def;
  if constexpr (std::is_same_v<T, std::string>) {
    return n[key].as<std::string>();
  } else {
    return n[key].as<T>();
  }
}

// Optional fetch
template <typename T>
std::optional<T> get_opt(const YAML::Node& n, const char* key) {
  if (!n || !n[key]) return std::nullopt;
  return n[key].as<T>();
}

LayoutMode parse_layout(const YAML::Node& n, const char* key, LayoutMode def) {
  auto s = get_or<std::string>(n, key, "");
  if (s == "planar") return LayoutMode::planar;
  if (s == "interleaved" || s == "interleave") return LayoutMode::interleaved;
  return def;
}

DataFormat parse_format(const YAML::Node& n, const char* key, DataFormat def) {
  auto s = get_or<std::string>(n, key, "");
  if (s == "cs16" || s == "i16q16" || s == "iq16") return DataFormat::cs16;
  if (s == "cf32" || s == "f32") return DataFormat::cf32;
  return def;
}

Stream parse_stream(const YAML::Node& n, const Defaults& dflt) {
  Stream s = dflt.rx_stream; // seed with defaults; caller can choose rx/tx
  if (!n) return s;

  // ring can be a single string or a list
  if (n["ring"]) {
    s.ring.clear();
    if (n["ring"].IsSequence()) {
      for (auto it : n["ring"]) s.ring.emplace_back(it.as<std::string>());
    } else {
      s.ring.emplace_back(n["ring"].as<std::string>());
    }
  }

  s.mode          = parse_layout(n, "mode", s.mode);
  s.spp           = get_or<std::size_t>(n, "spp", s.spp);
  s.num_channels  = get_or<unsigned>(n, "num_channels", s.num_channels);
  s.allow_partial = get_or<bool>(n, "allow_partial", s.allow_partial);
  s.timeout_us    = get_or<uint32_t>(n, "timeout_us", s.timeout_us);
  s.busy_poll     = get_or<bool>(n, "busy_poll", s.busy_poll);

  s.high_watermark_pct = get_opt<unsigned>(n, "high_watermark_pct");
  s.hard_drop_pct      = get_opt<unsigned>(n, "hard_drop_pct");
  return s;
}

AppSection parse_app(const YAML::Node& n, const Defaults& dflt) {
  AppSection a;
  if (!n) return a;
  if (n["rx_stream"]) {
    Stream rx = parse_stream(n["rx_stream"], dflt);
    a.rx_stream = rx;
  }
  if (n["tx_stream"]) {
    // For tx, seed with defaults.tx_stream then overlay
    Defaults tmp = dflt;
    tmp.rx_stream = dflt.tx_stream;
    Stream tx = parse_stream(n["tx_stream"], tmp);
    a.tx_stream = tx;
  }
  return a;
}

} // namespace

// ------------------------- loaders -------------------------

Config Config::load(const std::string& yaml_path) {
  YAML::Node root = YAML::LoadFile(yaml_path);
  if (!root) throw std::runtime_error("Failed to load YAML: " + yaml_path);

  Config cfg;

  // EAL
  if (auto e = root["eal"]) {
    cfg.eal.file_prefix = get_or<std::string>(e, "file_prefix", cfg.eal.file_prefix);
    cfg.eal.huge_dir    = get_or<std::string>(e, "huge_dir",    cfg.eal.huge_dir);
    cfg.eal.socket_mem  = get_or<std::string>(e, "socket_mem",  cfg.eal.socket_mem);
    cfg.eal.no_pci      = get_or<bool>(e,        "no_pci",      cfg.eal.no_pci);
    cfg.eal.iova        = get_or<std::string>(e, "iova",        cfg.eal.iova);

    cfg.eal.main_lcore  = get_opt<int>(e, "main_lcore");
    cfg.eal.lcores      = get_opt<std::string>(e, "lcores");
    cfg.eal.isolcpus    = get_opt<std::string>(e, "isolcpus");
    cfg.eal.numa        = get_opt<bool>(e, "numa");
    cfg.eal.socket_limit= get_opt<std::string>(e, "socket_limit");
  }

  // Defaults
  if (auto d = root["defaults"]) {
    cfg.defaults.nb_mbuf     = get_or<unsigned>(d, "nb_mbuf", cfg.defaults.nb_mbuf);
    cfg.defaults.mp_cache    = get_or<unsigned>(d, "mp_cache", cfg.defaults.mp_cache);
    cfg.defaults.ring_size   = get_or<unsigned>(d, "ring_size", cfg.defaults.ring_size);
    cfg.defaults.data_format = parse_format(d, "data_format", cfg.defaults.data_format);
    cfg.defaults.num_channels= get_or<unsigned>(d, "num_channels", cfg.defaults.num_channels);

    // default streams
    if (d["rx_stream"]) cfg.defaults.rx_stream = parse_stream(d["rx_stream"], cfg.defaults);
    if (d["tx_stream"]) {
      // Seed parser with tx defaults
      Defaults tmp = cfg.defaults;
      tmp.rx_stream = cfg.defaults.tx_stream;
      cfg.defaults.tx_stream = parse_stream(d["tx_stream"], tmp);
    }
  }

  // App sections (optional)
  cfg.primary = root["primary"] ? std::optional<AppSection>(parse_app(root["primary"], cfg.defaults)) : std::nullopt;
  cfg.ue      = root["ue"]      ? std::optional<AppSection>(parse_app(root["ue"], cfg.defaults))      : std::nullopt;
  cfg.gnb     = root["gnb"]     ? std::optional<AppSection>(parse_app(root["gnb"], cfg.defaults))     : std::nullopt;

  return cfg;
}

// ------------------------- effective getters -------------------------

static const AppSection* pick_app(const Config& c, const std::string& app) {
  if (app == "primary") return c.primary ? &*c.primary : nullptr;
  if (app == "ue")      return c.ue      ? &*c.ue      : nullptr;
  if (app == "gnb")     return c.gnb     ? &*c.gnb     : nullptr;
  return nullptr;
}

const Stream& Config::rx_stream_for(const std::string& app) const {
  if (auto a = pick_app(*this, app)) {
    if (a->rx_stream) return *a->rx_stream;
  }
  return defaults.rx_stream;
}

const Stream& Config::tx_stream_for(const std::string& app) const {
  if (auto a = pick_app(*this, app)) {
    if (a->tx_stream) return *a->tx_stream;
  }
  return defaults.tx_stream;
}

// ------------------------- printers -------------------------

std::ostream& flexsdr::conf::operator<<(std::ostream& os, DataFormat f) {
  switch (f) {
    case DataFormat::cs16: return os << "cs16";
    case DataFormat::cf32: return os << "cf32";
  }
  return os;
}

std::ostream& flexsdr::conf::operator<<(std::ostream& os, LayoutMode m) {
  switch (m) {
    case LayoutMode::planar:      return os << "planar";
    case LayoutMode::interleaved: return os << "interleaved";
  }
  return os;
}

std::ostream& flexsdr::conf::operator<<(std::ostream& os, const Eal& e) {
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
  os << "Stream{ring=[";
  for (std::size_t i = 0; i < s.ring.size(); ++i) {
    if (i) os << ",";
    os << s.ring[i];
  }
  os << "], mode=" << s.mode
     << ", spp=" << s.spp
     << ", num_channels=" << s.num_channels
     << ", allow_partial=" << (s.allow_partial ? "true" : "false")
     << ", timeout_us=" << s.timeout_us
     << ", busy_poll=" << (s.busy_poll ? "true" : "false");
  if (s.high_watermark_pct) os << ", high_wm%=" << *s.high_watermark_pct;
  if (s.hard_drop_pct)      os << ", hard_drop%=" << *s.hard_drop_pct;
  return os << "}";
}

std::ostream& flexsdr::conf::operator<<(std::ostream& os, const Defaults& d) {
  os << "Defaults{nb_mbuf=" << d.nb_mbuf
     << ", mp_cache=" << d.mp_cache
     << ", ring_size=" << d.ring_size
     << ", data_format=" << d.data_format
     << ", num_channels=" << d.num_channels
     << ", rx=" << d.rx_stream
     << ", tx=" << d.tx_stream
     << "}";
  return os;
}

std::ostream& flexsdr::conf::operator<<(std::ostream& os, const Config& c) {
  os << c.eal << "\n" << c.defaults << "\n";
  auto dump_app = [&](const char* name, const std::optional<AppSection>& a){
    os << name << "{";
    if (a && a->rx_stream) os << "rx=" << *a->rx_stream;
    if (a && a->tx_stream) {
      if (a->rx_stream) os << ", ";
      os << "tx=" << *a->tx_stream;
    }
    return os << "}\n";
  };
  dump_app("primary", c.primary);
  dump_app("ue",      c.ue);
  dump_app("gnb",     c.gnb);
  return os;
}
