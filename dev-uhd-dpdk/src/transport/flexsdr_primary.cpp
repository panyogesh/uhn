#include "transport/flexsdr_primary.hpp"
#include "conf/config_params.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <optional>

// DPDK
#include <rte_config.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_ring.h>
#include <rte_lcore.h>
#include <rte_memory.h>

namespace flexsdr {

// --------------------------- tiny helpers -----------------------------------

static const char* role_str(conf::Role r) {
  switch (r) {
    case conf::Role::PrimaryUe:  return "primary-ue";
    case conf::Role::PrimaryGnb: return "primary-gnb";
    case conf::Role::Ue:         return "ue";
    case conf::Role::Gnb:        return "gnb";
    default:                     return "unknown";
  }
}

static inline const std::vector<conf::RingSpec>&
collect_tx_rings_(const conf::PrimaryConfig& cfg) {
  // Prefer explicit role block; fall back to defaults
  if (cfg.primary_ue && !cfg.primary_ue->tx_stream->rings.empty())
    return cfg.primary_ue->tx_stream->rings;
  if (cfg.primary_gnb && !cfg.primary_gnb->tx_stream->rings.empty())
    return cfg.primary_gnb->tx_stream->rings;
  return cfg.defaults.tx_stream.rings;
}

static inline const std::vector<conf::RingSpec>&
collect_rx_rings_(const conf::PrimaryConfig& cfg) {
  if (cfg.primary_ue && !cfg.primary_ue->rx_stream->rings.empty())
    return cfg.primary_ue->rx_stream->rings;
  if (cfg.primary_gnb && !cfg.primary_gnb->rx_stream->rings.empty())
    return cfg.primary_gnb->rx_stream->rings;
  return cfg.defaults.rx_stream.rings;
}

static inline const std::vector<conf::PoolSpec>&
collect_pools_(const conf::PrimaryConfig& cfg) {
  if (cfg.primary_ue && !cfg.primary_ue->pools.empty())
    return cfg.primary_ue->pools;
  if (cfg.primary_gnb && !cfg.primary_gnb->pools.empty())
    return cfg.primary_gnb->pools;
  static const std::vector<conf::PoolSpec> kEmpty;
  return kEmpty;
}

// --------------------------- FlexSDRPrimary ---------------------------------

FlexSDRPrimary::FlexSDRPrimary(std::string yaml_path)
  : yaml_path_(std::move(yaml_path)) {
  (void)load_config_();
  std::fprintf(stderr, "[primary] constructed FlexSDRPrimary\n");
}

int FlexSDRPrimary::load_config_() {
  int rc = conf::load_from_yaml(yaml_path_.c_str(), cfg_);
  if (rc) {
    std::fprintf(stderr, "[primary] load_from_yaml failed rc=%d\n", rc);
    return rc;
  }
  return 0;
}

int FlexSDRPrimary::init_resources() {
  std::fprintf(stderr, "[primary] init_resources: role=%s ring_size=%u\n",
               role_str(cfg_.defaults.role), cfg_.defaults.ring_size);

  // 1) pools
  if (int rc = create_pools_(); rc) return rc;

  // 2) TX rings
  if (int rc = create_rings_tx_(); rc) return rc;

  // 3) RX rings
  if (int rc = create_rings_rx_(); rc) return rc;

  return 0;
}

int FlexSDRPrimary::create_pools_() {
  const auto& pools = collect_pools_(cfg_);
  for (const auto& p : pools) {
    const std::string name = p.name;
    const unsigned    n    = p.size;
    const unsigned    esz  = p.elt_size;
    const unsigned    cache = p.cache_size ? p.cache_size : cfg_.defaults.mp_cache;

    rte_mempool* mp = rte_mempool_create(
        name.c_str(),
        n,                 // number of elements
        esz,               // element size
        cache,             // per-lcore cache
        0,                 // private size
        nullptr, nullptr,  // pool ctor
        nullptr, nullptr,  // obj ctor
        SOCKET_ID_ANY,
        0);

    if (!mp) {
      int err = rte_errno;
      std::fprintf(stderr,
                   "[pool] create failed: %s (n=%u elt=%u cache=%u) rc=%d rte_errno=%d\n",
                   name.c_str(), n, esz, cache, -1, err);
      return -1;
    }

    std::fprintf(stderr, "[pool] created: %s (n=%u elt=%u cache=%u)\n",
                 name.c_str(), n, esz, cache);
    pools_.push_back(mp);
  }
  return 0;
}

int FlexSDRPrimary::create_ring_(const std::string& name, unsigned size, rte_ring** out) {
  *out = nullptr;

  // Create with default flags; SINGLE-PRODUCER/SINGLE-CONSUMER are optional.
  rte_ring* r = rte_ring_create(name.c_str(), size, rte_socket_id(), 0);
  if (r) {
    *out = r;
    return 0;
  }

  // If it already exists, just look it up
  if (rte_errno == EEXIST) {
    r = rte_ring_lookup(name.c_str());
    if (r) {
      *out = r;
      return 0;
    }
  }

  std::fprintf(stderr, "[ring] create failed: %s (size=%u) rc=%d rte_errno=%d\n",
               name.c_str(), size, -1, rte_errno);
  return -1;
}

int FlexSDRPrimary::create_rings_tx_() {
  const auto& rings = collect_tx_rings_(cfg_);
  for (const auto& r : rings) {
    rte_ring* ptr = nullptr;
    int rc = create_ring_(r.name, r.size ? r.size : cfg_.defaults.ring_size, &ptr);
    if (rc) return rc;
    std::fprintf(stderr, "[ring] created TX: %s (size=%u)\n",
                 r.name.c_str(), rte_ring_get_size(ptr));
    tx_rings_.push_back(ptr);
  }
  return 0;
}

int FlexSDRPrimary::create_rings_rx_() {
  const auto& rings = collect_rx_rings_(cfg_);
  for (const auto& r : rings) {
    rte_ring* ptr = nullptr;
    int rc = create_ring_(r.name, r.size ? r.size : cfg_.defaults.ring_size, &ptr);
    if (rc) return rc;
    std::fprintf(stderr, "[ring] created RX: %s (size=%u)\n",
                 r.name.c_str(), rte_ring_get_size(ptr));
    rx_rings_.push_back(ptr);
  }
  return 0;
}

} // namespace flexsdr
