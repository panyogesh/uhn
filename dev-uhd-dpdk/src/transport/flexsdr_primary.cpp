#include "transport/flexsdr_primary.hpp"
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

// size helper stays (this is stable)
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

FlexSDRPrimary::FlexSDRPrimary(std::string yaml_path)
  : yaml_path_(std::move(yaml_path)) {}

int FlexSDRPrimary::init_resources() {
  int rc = load_config_();
  if (rc) return rc;

  std::printf("[cfg] role=%s, ring_size=%u\n",
              role_str(cfg_.defaults.role), cfg_.defaults.ring_size);

  if ((rc = create_pools_()))   return rc;
  if ((rc = create_rings_tx_())) return rc;
  if ((rc = create_rings_rx_())) return rc;
  return 0;
}

// don’t peek into ring internals; resolve via lookup and check membership
rte_ring* FlexSDRPrimary::ring_by_name(const std::string& name) const {
  rte_ring* candidate = rte_ring_lookup(name.c_str());
  if (!candidate) return nullptr;
  for (auto* r : tx_rings_) if (r == candidate) return r;
  for (auto* r : rx_rings_) if (r == candidate) return r;
  return nullptr;
}

int FlexSDRPrimary::load_config_() {
  return conf::load_from_yaml(yaml_path_.c_str(), cfg_);
}

int FlexSDRPrimary::create_pools_() {
  // primary_ue is optional in the schema; if absent there’s nothing to do.
  if (!cfg_.primary_ue.has_value()) return 0;

  for (const auto& p : cfg_.primary_ue->pools) {
    rte_mempool* mp = rte_mempool_lookup(p.name.c_str());
    if (mp) { pools_.push_back(mp); continue; }

    mp = rte_mempool_create(p.name.c_str(),
                            p.size,
                            p.elt_size,
                            cfg_.defaults.mp_cache,
                            0 /* priv_size */,
                            nullptr, nullptr, nullptr, nullptr,
                            rte_socket_id(),
                            0 /* flags */);
    if (!mp) {
      int err = rte_errno;
      std::printf("[pool] create failed: %s (n=%u elt=%u) rc=%d rte_errno=%d\n",
                  p.name.c_str(), p.size, p.elt_size, -err, err);
      return -err ? -err : -1;
    }
    std::printf("[pool] created: %s\n", p.name.c_str());
    pools_.push_back(mp);
  }
  return 0;
}

static inline const std::vector<RingSpec>&
collect_tx_rings_(const PrimaryConfig& cfg) {
  // Source of truth: defaults.tx_stream.rings
  return cfg.defaults.tx_stream.rings;
}

static inline const std::vector<RingSpec>&
collect_rx_rings_(const PrimaryConfig& cfg) {
  // Source of truth: defaults.rx_stream.rings
  return cfg.defaults.rx_stream.rings;
}

int FlexSDRPrimary::create_rings_tx_() {
  const auto& rings = collect_tx_rings_(cfg_);
  for (const auto& r : rings) {
    rte_ring* ptr = nullptr;
    const unsigned sz = r.size ? r.size : cfg_.defaults.ring_size;
    int rc = create_or_lookup_ring_(r.name, sz, &ptr);
    if (rc) return rc;
    std::printf("[ring] tx ready: %s (size=%u)\n", r.name.c_str(), ring_size(ptr));
    tx_rings_.push_back(ptr);
  }
  return 0;
}

int FlexSDRPrimary::create_rings_rx_() {
  const auto& rings = collect_rx_rings_(cfg_);
  for (const auto& r : rings) {
    rte_ring* ptr = nullptr;
    const unsigned sz = r.size ? r.size : cfg_.defaults.ring_size;
    int rc = create_or_lookup_ring_(r.name, sz, &ptr);
    if (rc) return rc;
    std::printf("[ring] rx ready: %s (size=%u)\n", r.name.c_str(), ring_size(ptr));
    rx_rings_.push_back(ptr);
  }
  return 0;
}

int FlexSDRPrimary::create_or_lookup_ring_(const std::string& name,
                                           unsigned size,
                                           rte_ring** out) {
  if (auto* r = rte_ring_lookup(name.c_str())) { *out = r; return 0; }

  auto* r = rte_ring_create(name.c_str(), size, rte_socket_id(),
                            RING_F_SP_ENQ | RING_F_SC_DEQ);
  if (!r) {
    int err = rte_errno;
    if (err == EEXIST) { // RTE_EEXIST isn’t always defined; EEXIST is enough
      r = rte_ring_lookup(name.c_str());
      if (r) { *out = r; return 0; }
    }
    std::printf("[ring] %s failed (size=%u) rc=%d rte_errno=%d\n",
                name.c_str(), size, -err, err);
    return -err ? -err : -1;
  }
  *out = r;
  return 0;
}

} // namespace flexsdr
