//
// config_params.cpp
//

#include "conf/config_params.hpp"

#include <yaml-cpp/yaml.h>

#include <stdexcept>
#include <sstream>
#include <iostream>
#include <algorithm>

// DPDK
extern "C" {
#include <rte_config.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
}

namespace flexsdr {

// -----------------------------
// Small YAML helpers
// -----------------------------

static inline bool has_key(const YAML::Node& n, const char* k) {
  return n && n[k];
}

template <typename T>
static T get_or(const YAML::Node& n, const char* k, const T& def) {
  if (n && n[k]) return n[k].as<T>();
  return def;
}

static std::string get_str_or(const YAML::Node& n, const char* k, const std::string& def = {}) {
  if (n && n[k] && n[k].IsScalar()) return n[k].as<std::string>();
  return def;
}

static std::vector<std::string> parse_rings_as_list(const YAML::Node& stream_node) {
  std::vector<std::string> out;
  if (!stream_node) return out;
  const auto rn = stream_node["ring"];
  if (!rn) return out;

  if (rn.IsScalar()) {
    auto s = rn.as<std::string>();
    if (!s.empty()) out.push_back(s);
  } else if (rn.IsSequence()) {
    for (auto&& item : rn) {
      out.push_back(item.as<std::string>());
    }
  }
  return out;
}

static StreamConfig merge_stream(const YAML::Node& node, const StreamConfig& base) {
  StreamConfig s = base;
  if (!node) return s;

  // ring(s)
  if (node["ring"]) s.rings = parse_rings_as_list(node);

  if (has_key(node, "mode"))            s.mode           = node["mode"].as<std::string>();
  if (has_key(node, "spp"))             s.spp            = node["spp"].as<uint32_t>();
  if (has_key(node, "num_channels"))    s.num_channels   = node["num_channels"].as<uint32_t>();
  if (has_key(node, "allow_partial"))   s.allow_partial  = node["allow_partial"].as<bool>();
  if (has_key(node, "timeout_us"))      s.timeout_us     = node["timeout_us"].as<uint32_t>();
  if (has_key(node, "busy_poll"))       s.busy_poll      = node["busy_poll"].as<bool>();
  if (has_key(node, "burst_dequeue"))   s.burst_dequeue  = node["burst_dequeue"].as<uint32_t>();
  if (has_key(node, "ring_watermark"))  s.ring_watermark = node["ring_watermark"].as<uint32_t>();
  return s;
}

static StreamConfig parse_stream_exact(const YAML::Node& node) {
  // No defaults; use only specified values.
  StreamConfig base{};
  return merge_stream(node, base);
}

static EALConfig parse_eal(const YAML::Node& root) {
  const auto n = root["eal"];
  if (!n) throw std::runtime_error("Missing 'eal' section in YAML.");

  EALConfig e;
  e.file_prefix = get_str_or(n, "file_prefix");
  e.huge_dir    = get_str_or(n, "huge_dir");
  e.socket_mem  = get_str_or(n, "socket_mem");
  e.no_pci      = get_or<bool>(n, "no_pci", true);
  e.iova        = get_str_or(n, "iova");

  return e;
}

static DefaultConfig parse_defaults(const YAML::Node& root) {
  const auto n = root["defaults"];
  if (!n) throw std::runtime_error("Missing 'defaults' section in YAML.");

  DefaultConfig d;
  d.nb_mbuf   = get_or<uint32_t>(n, "nb_mbuf", 8192);
  d.mp_cache  = get_or<uint32_t>(n, "mp_cache", 256);
  d.ring_size = get_or<uint32_t>(n, "ring_size", 512);
  d.data_format = get_str_or(n, "data_format");

  d.tx_stream = parse_stream_exact(n["tx_stream"]);
  d.rx_stream = parse_stream_exact(n["rx_stream"]);
  return d;
}

static PrimaryConfig parse_primary(const YAML::Node& root) {
  const auto n = root["primary"];
  if (!n) throw std::runtime_error("Missing 'primary' section in YAML.");

  PrimaryConfig p;

  // pools
  if (n["pools"] && n["pools"].IsSequence()) {
    for (auto&& item : n["pools"]) {
      PoolConf pc;
      pc.name     = get_str_or(item, "name");
      pc.size     = get_or<uint32_t>(item, "size", 0);
      pc.elt_size = get_or<uint32_t>(item, "elt_size", 0);
      if (pc.name.empty() || pc.size==0 || pc.elt_size==0) {
        throw std::runtime_error("primary.pools: each entry must have name,size,elt_size");
      }
      p.pools.push_back(pc);
    }
  }

  // rings
  if (n["rings"] && n["rings"].IsSequence()) {
    for (auto&& item : n["rings"]) {
      RingConf rc;
      rc.name = get_str_or(item, "name");
      rc.size = get_or<uint32_t>(item, "size", 0);
      if (rc.name.empty() || rc.size==0) {
        throw std::runtime_error("primary.rings: each entry must have name,size");
      }
      p.rings.push_back(rc);
    }
  }

  return p;
}

static TestTransmit parse_test_tx(const YAML::Node& n) {
  TestTransmit t;
  if (!n) return t;
  t.enabled    = get_or<bool>(n, "enabled", false);
  t.pps        = get_or<uint32_t>(n, "pps", 0);
  t.burst_size = get_or<uint32_t>(n, "burst_size", 0);
  t.duration_s = get_or<uint32_t>(n, "duration_s", 0);
  return t;
}

static AppConfig parse_app(const YAML::Node& root,
                           const char* key,
                           const DefaultConfig& defs)
{
  const auto n = root[key];
  if (!n) throw std::runtime_error(std::string("Missing '") + key + "' section in YAML.");

  AppConfig a;
  a.inbound_ring = get_str_or(n, "inbound_ring");
  a.rx_pool      = get_str_or(n, "rx_pool");
  a.rx_cores     = get_or<std::vector<int>>(n, "rx_cores", {});
  a.tx_pool      = get_str_or(n, "tx_pool");
  a.tx_cores     = get_or<std::vector<int>>(n, "tx_cores", {});

  // Merge app streams over defaults
  a.rx_stream = merge_stream(n["rx_stream"], defs.rx_stream);
  a.tx_stream = merge_stream(n["tx_stream"], defs.tx_stream);

  a.test_tx = parse_test_tx(n["test_transmit"]);
  return a;
}

// -----------------------------
// Public loaders
// -----------------------------

FullConfig load_full_config(const std::string& yaml_file) {
  YAML::Node root = YAML::LoadFile(yaml_file);

  FullConfig cfg;
  cfg.eal      = parse_eal(root);
  cfg.defaults = parse_defaults(root);
  cfg.primary  = parse_primary(root);
  cfg.ue_app   = parse_app(root, "ue-app",  cfg.defaults);
  cfg.gnb_app  = parse_app(root, "gnb-app", cfg.defaults);
  return cfg;
}

PrimaryConfig load_primary_conf_params(const std::string& yaml_file) {
  YAML::Node root = YAML::LoadFile(yaml_file);
  return parse_primary(root);
}

AppConfig load_ue_config_params(const std::string& yaml_file) {
  YAML::Node root = YAML::LoadFile(yaml_file);
  auto defs = parse_defaults(root);
  return parse_app(root, "ue-app", defs);
}

AppConfig load_gnb_conf_params(const std::string& yaml_file) {
  YAML::Node root = YAML::LoadFile(yaml_file);
  auto defs = parse_defaults(root);
  return parse_app(root, "gnb-app", defs);
}

EALConfig load_eal_params(const std::string& yaml_file) {
  YAML::Node root = YAML::LoadFile(yaml_file);
  return parse_eal(root);
}

DefaultConfig load_defaults(const std::string& yaml_file) {
  YAML::Node root = YAML::LoadFile(yaml_file);
  return parse_defaults(root);
}

// -----------------------------
// DPDK helpers
// -----------------------------

static bool create_one_pktmbuf_pool(const PoolConf& pc,
                                    unsigned socket_id,
                                    unsigned cache_size,
                                    bool allow_existing)
{
  // If already present and allowed, reuse
  if (allow_existing) {
    if (auto* mp = rte_mempool_lookup(pc.name.c_str())) {
      (void)mp;
      return true;
    }
  }

  // Create a pktmbuf pool with the requested data room size
  // private_size=0, data_room = elt_size
  rte_mempool* mp = rte_pktmbuf_pool_create(pc.name.c_str(),
                                            pc.size,
                                            cache_size,
                                            0 /* priv_size */,
                                            pc.elt_size /* data room */,
                                            socket_id);
  if (!mp) {
    std::cerr << "[DPDK] Failed to create pktmbuf pool '" << pc.name
              << "' size=" << pc.size << " elt=" << pc.elt_size
              << " socket=" << socket_id << " (rte_errno=" << rte_errno << ")\n";
    return false;
  }
  return true;
}

static bool create_one_ring(const RingConf& rc,
                            unsigned socket_id,
                            bool allow_existing,
                            unsigned flags)
{
  if (allow_existing) {
    if (auto* r = rte_ring_lookup(rc.name.c_str())) {
      (void)r;
      return true;
    }
  }

  rte_ring* r = rte_ring_create(rc.name.c_str(), rc.size, socket_id, flags);
  if (!r) {
    std::cerr << "[DPDK] Failed to create ring '" << rc.name
              << "' size=" << rc.size
              << " socket=" << socket_id << " (rte_errno=" << rte_errno << ")\n";
    return false;
  }
  return true;
}

bool create_primary_resources(const PrimaryConfig& primary,
                              unsigned socket_id,
                              unsigned mp_cache_default,
                              bool allow_existing,
                              unsigned ring_flags)
{
  bool ok = true;

  for (const auto& pc : primary.pools) {
    ok = ok && create_one_pktmbuf_pool(pc, socket_id, mp_cache_default, allow_existing);
  }

  // Prefer SPSC rings for low latency if caller didn't specify flags
  unsigned flags = ring_flags;
  if (flags == 0) {
    flags = RING_F_SP_ENQ | RING_F_SC_DEQ;
  }
  for (const auto& rc : primary.rings) {
    ok = ok && create_one_ring(rc, socket_id, allow_existing, flags);
  }

  return ok;
}

static bool check_pool(const std::string& name) {
  auto* mp = rte_mempool_lookup(name.c_str());
  if (!mp) {
    std::cerr << "[DPDK] Missing mempool '" << name << "'\n";
    return false;
  }
  return true;
}

static bool check_ring(const std::string& name) {
  auto* r = rte_ring_lookup(name.c_str());
  if (!r) {
    std::cerr << "[DPDK] Missing ring '" << name << "'\n";
    return false;
  }
  return true;
}

bool validate_secondary_resources(const AppConfig& app, bool check_rx, bool check_tx) {
  bool ok = true;

  if (check_rx) {
    if (!app.inbound_ring.empty()) ok = ok && check_ring(app.inbound_ring);
    if (!app.rx_pool.empty())      ok = ok && check_pool(app.rx_pool);

    // rx_stream may have a single ring reference too
    if (!app.rx_stream.rings.empty()) {
      for (const auto& r : app.rx_stream.rings) ok = ok && check_ring(r);
    }
  }

  if (check_tx) {
    if (!app.tx_pool.empty()) ok = ok && check_pool(app.tx_pool);
    for (const auto& r : app.tx_stream.rings) ok = ok && check_ring(r);
  }

  return ok;
}

// -----------------------------
// Pretty printers
// -----------------------------

static std::string join(const std::vector<std::string>& v, const char* sep = ",") {
  std::ostringstream oss;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) oss << sep;
    oss << v[i];
  }
  return oss.str();
}

static std::string joini(const std::vector<int>& v, const char* sep = ",") {
  std::ostringstream oss;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) oss << sep;
    oss << v[i];
  }
  return oss.str();
}

std::string summarize(const EALConfig& c) {
  std::ostringstream o;
  o << "EAL{file_prefix='" << c.file_prefix
    << "', huge_dir='" << c.huge_dir
    << "', socket_mem='" << c.socket_mem
    << "', no_pci=" << (c.no_pci ? "true" : "false")
    << ", iova='" << c.iova << "'}";
  return o.str();
}

std::string summarize(const StreamConfig& s) {
  std::ostringstream o;
  o << "Stream{rings=[" << join(s.rings)
    << "], mode=" << s.mode
    << ", spp=" << s.spp
    << ", ch=" << s.num_channels
    << ", allow_partial=" << (s.allow_partial?"true":"false")
    << ", timeout_us=" << s.timeout_us
    << ", busy_poll=" << (s.busy_poll?"true":"false")
    << ", burst_dequeue=" << s.burst_dequeue
    << ", watermark=" << s.ring_watermark << "}";
  return o.str();
}

std::string summarize(const DefaultConfig& d) {
  std::ostringstream o;
  o << "DefaultConfig{nb_mbuf=" << d.nb_mbuf
    << ", mp_cache=" << d.mp_cache
    << ", ring_size=" << d.ring_size
    << ", data_format='" << d.data_format << "'\n  tx=" << summarize(d.tx_stream)
    << "\n  rx=" << summarize(d.rx_stream) << "}";
  return o.str();
}

std::string summarize(const PrimaryConfig& p) {
  std::ostringstream o;
  o << "Primary{\n  pools=[";
  for (size_t i=0;i<p.pools.size();++i) {
    const auto& pc = p.pools[i];
    if (i) o << ", ";
    o << "{name='"<<pc.name<<"', size="<<pc.size<<", elt="<<pc.elt_size<<"}";
  }
  o << "]\n  rings=[";
  for (size_t i=0;i<p.rings.size();++i) {
    const auto& rc = p.rings[i];
    if (i) o << ", ";
    o << "{name='"<<rc.name<<"', size="<<rc.size<<"}";
  }
  o << "]}";
  return o.str();
}

std::string summarize(const AppConfig& a) {
  std::ostringstream o;
  o << "App{inbound='"<<a.inbound_ring<<"', rx_pool='"<<a.rx_pool<<"', rx_cores=["<<joini(a.rx_cores)
    <<"], tx_pool='"<<a.tx_pool<<"', tx_cores=["<<joini(a.tx_cores)<<"]\n  rx="<<summarize(a.rx_stream)
    <<"\n  tx="<<summarize(a.tx_stream)
    <<"\n  test_tx={enabled="<<(a.test_tx.enabled?"true":"false")<<", pps="<<a.test_tx.pps
    <<", burst="<<a.test_tx.burst_size<<", dur_s="<<a.test_tx.duration_s<<"}}";
  return o.str();
}

std::string summarize(const FullConfig& f) {
  std::ostringstream o;
  o << summarize(f.eal) << "\n"
    << summarize(f.defaults) << "\n"
    << summarize(f.primary) << "\nUE " << summarize(f.ue_app)
    << "\nGNB " << summarize(f.gnb_app);
  return o.str();
}

} // namespace flexsdr
