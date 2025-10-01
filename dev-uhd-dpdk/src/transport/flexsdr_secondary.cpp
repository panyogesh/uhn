#include "transport/flexsdr_secondary.hpp"
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <rte_errno.h>
#include <rte_mempool.h>
#include <rte_ring.h>

using std::string;
namespace flexsdr {

static inline bool bad_name(const std::string& s) { return s.empty(); }

int FlexSDRSecondary::lookup_ring_(const string& name, rte_ring** out) {
  if (bad_name(name)) {
    std::fprintf(stderr, "[ring] ERROR: empty name for lookup\n");
    return -EINVAL;
  }
  rte_errno = 0;
  rte_ring* r = rte_ring_lookup(name.c_str());
  if (!r) {
    std::fprintf(stderr, "[ring] lookup(%s) failed: rte_errno=%d\n",
                 name.c_str(), rte_errno);
    return -ENOENT;
  }
  *out = r;
  return 0;
}

int FlexSDRSecondary::lookup_pool_(const string& name, rte_mempool** out) {
  if (bad_name(name)) {
    std::fprintf(stderr, "[pool] ERROR: empty name for lookup\n");
    return -EINVAL;
  }
  rte_errno = 0;
  rte_mempool* mp = rte_mempool_lookup(name.c_str());
  if (!mp) {
    std::fprintf(stderr, "[pool] lookup(%s) failed: rte_errno=%d\n",
                 name.c_str(), rte_errno);
    return -ENOENT;
  }
  *out = mp;
  return 0;
}

// -------------- lifecycle --------------

FlexSDRSecondary::FlexSDRSecondary(std::string yaml_path)
: yaml_path_(std::move(yaml_path)) {}

int FlexSDRSecondary::load_config_() {
  int rc = conf::load_from_yaml(yaml_path_.c_str(), cfg_);
  if (rc) {
    std::fprintf(stderr, "[cfg] load_from_yaml(%s) failed rc=%d\n",
                 yaml_path_.c_str(), rc);
  }
  return rc;
}

int FlexSDRSecondary::init_resources() {
  if (int rc = load_config_(); rc) return rc;

  std::fprintf(stderr, "[secondary] init_resources: ring_size=%u\n",
               cfg_.defaults.ring_size);

  // --- Always LOOKUP from 'ue' section ---
  if (!cfg_.ue) {
    std::fprintf(stderr, "[secondary] ERROR: missing 'ue' section\n");
    return -EINVAL;
  }

  if (int rc = lookup_pools_(); rc) return rc;
  if (int rc = lookup_rings_tx_(); rc) return rc;
  if (int rc = lookup_rings_rx_(); rc) return rc;

  // Interconnect is lookup-only (created by gNB primary).
  (void)lookup_interconnect_();

  std::fprintf(stderr, "[secondary] resources ready\n");
  return 0;
}

// -------------- lookups --------------

static inline const std::vector<conf::RingSpec>& pick_tx(const conf::PrimaryConfig& cfg) {
  if (cfg.ue && cfg.ue->tx_stream && !cfg.ue->tx_stream->rings.empty())
    return cfg.ue->tx_stream->rings;
  return cfg.defaults.tx_stream.rings;
}
static inline const std::vector<conf::RingSpec>& pick_rx(const conf::PrimaryConfig& cfg) {
  if (cfg.ue && cfg.ue->rx_stream && !cfg.ue->rx_stream->rings.empty())
    return cfg.ue->rx_stream->rings;
  return cfg.defaults.rx_stream.rings;
}

int FlexSDRSecondary::lookup_pools_() {
  // UE secondary normally doesn’t need pools, but if user config asks for it, honor it
  for (const auto& p : cfg_.ue->pools) {
    rte_mempool* mp = nullptr;
    if (int rc = lookup_pool_(p.name, &mp); rc) return rc;
    pools_.push_back(mp);
    std::fprintf(stderr, "[pool] found: %s\n", p.name.c_str());
  }
  return 0;
}

int FlexSDRSecondary::lookup_rings_tx_() {
  const auto& rings = pick_tx(cfg_);
  for (const auto& r : rings) {
    rte_ring* ptr = nullptr;
    if (int rc = lookup_ring_(r.name, &ptr); rc) return rc;
    tx_rings_.push_back(ptr);
    std::fprintf(stderr, "[ring] found tx: %s\n", r.name.c_str());
  }
  return 0;
}

int FlexSDRSecondary::lookup_rings_rx_() {
  const auto& rings = pick_rx(cfg_);
  for (const auto& r : rings) {
    rte_ring* ptr = nullptr;
    if (int rc = lookup_ring_(r.name, &ptr); rc) return rc;
    rx_rings_.push_back(ptr);
    std::fprintf(stderr, "[ring] found rx: %s\n", r.name.c_str());
  }
  return 0;
}

int FlexSDRSecondary::lookup_interconnect_() {
  if (!cfg_.defaults.interconnect || cfg_.defaults.interconnect->rings.empty())
    return 0;

  for (const auto& r : cfg_.defaults.interconnect->rings) {
    rte_ring* ptr = nullptr;
    int rc = lookup_ring_(r.name, &ptr);
    if (rc) {
      std::fprintf(stderr, "[ic] WARN: interconnect ring missing: %s\n",
                   r.name.c_str());
      continue;
    }
    std::fprintf(stderr, "[ic] found interconnect: %s\n", r.name.c_str());
  }
  return 0;
}

} // namespace flexsdr
