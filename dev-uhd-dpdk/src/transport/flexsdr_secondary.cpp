#include "transport/flexsdr_secondary.hpp"
#include "conf/config_params.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <rte_version.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_errno.h>

namespace flexsdr {

using conf::PrimaryConfig;
using conf::RingSpec;

static inline unsigned ring_size(const rte_ring* r) {
#if defined(RTE_VERSION) && RTE_VERSION >= RTE_VERSION_NUM(24, 11, 0, 0)
  return rte_ring_get_size(r);
#else
  return r->size;
#endif
}

// forward decl (defined in config_params.cpp)
namespace conf { int load_from_yaml(const char* path, PrimaryConfig& out); }

static const char* role_str(conf::Role r) {
  switch (r) {
    case conf::Role::ue:          return "ue";
    case conf::Role::gnb:         return "gnb";
    case conf::Role::primary_ue:  return "primary-ue";
    case conf::Role::primary_gnb: return "primary-gnb";
  }
  return "unknown";
}

FlexSDRSecondary::FlexSDRSecondary(std::string yaml_path)
  : yaml_path_(std::move(yaml_path)) {}

int FlexSDRSecondary::init_resources() {
  int rc = load_config_();
  if (rc) return rc;

  std::printf("[cfg] (secondary) role=%s, ring_size=%u\n",
              role_str(cfg_.defaults.role), cfg_.defaults.ring_size);

  if ((rc = lookup_pools_()))    return rc;
  if ((rc = lookup_rings_tx_())) return rc;
  if ((rc = lookup_rings_rx_())) return rc;
  return 0;
}

// don’t peek into ring internals; resolve via lookup and check membership
rte_ring* FlexSDRSecondary::ring_by_name(const std::string& name) const {
  rte_ring* candidate = rte_ring_lookup(name.c_str());
  if (!candidate) return nullptr;
  for (auto* r : tx_rings_) if (r == candidate) return r;
  for (auto* r : rx_rings_) if (r == candidate) return r;
  return nullptr;
}

int FlexSDRSecondary::load_config_() {
  return conf::load_from_yaml(yaml_path_.c_str(), cfg_);
}

int FlexSDRSecondary::lookup_pools_() {
  if (!cfg_.primary_ue.has_value()) return 0; // no pools to lookup
  for (const auto& p : cfg_.primary_ue->pools) {
    rte_mempool* mp = rte_mempool_lookup(p.name.c_str());
    if (!mp) {
      int err = rte_errno;
      std::printf("[pool] lookup failed: %s rc=%d rte_errno=%d\n",
                  p.name.c_str(), -err, err);
      return -err ? -err : -1;
    }
    std::printf("[pool] found: %s\n", p.name.c_str());
    pools_.push_back(mp);
  }
  return 0;
}

static inline const std::vector<RingSpec>&
collect_tx_rings_(const PrimaryConfig& cfg) {
  if (cfg.primary_ue && !cfg.primary_ue->rings.tx.empty())
    return cfg.primary_ue->rings.tx;
  return cfg.defaults.tx_stream.rings;
}

static inline const std::vector<RingSpec>&
collect_rx_rings_(const PrimaryConfig& cfg) {
  if (cfg.primary_ue && !cfg.primary_ue->rings.rx.empty())
    return cfg.primary_ue->rings.rx;
  return cfg.defaults.rx_stream.rings;
}

int FlexSDRSecondary::lookup_rings_tx_() {
  const auto& rings = collect_tx_rings_(cfg_);
  for (const auto& r : rings) {
    rte_ring* ptr = rte_ring_lookup(r.name.c_str());
    if (!ptr) {
      int err = rte_errno;
      std::printf("[ring] lookup failed: %s rc=%d rte_errno=%d\n",
                  r.name.c_str(), -err, err);
      return -err ? -err : -1;
    }
    std::printf("[ring] tx found: %s (size=%u)\n", r.name.c_str(), ring_size(ptr));
    tx_rings_.push_back(ptr);
  }
  return 0;
}

int FlexSDRSecondary::lookup_rings_rx_() {
  const auto& rings = collect_rx_rings_(cfg_);
  for (const auto& r : rings) {
    rte_ring* ptr = rte_ring_lookup(r.name.c_str());
    if (!ptr) {
      int err = rte_errno;
      std::printf("[ring] lookup failed: %s rc=%d rte_errno=%d\n",
                  r.name.c_str(), -err, err);
      return -err ? -err : -1;
    }
    std::printf("[ring] rx found: %s (size=%u)\n", r.name.c_str(), ring_size(ptr));
    rx_rings_.push_back(ptr);
  }
  return 0;
}

} // namespace flexsdr
