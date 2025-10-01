#include "transport/flexsdr_primary.hpp"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <algorithm>

// DPDK
#include <rte_log.h>
#include <rte_eal.h>
#include <rte_errno.h>

using namespace flexsdr;
using flexsdr::conf::RingSpec;
using flexsdr::conf::PoolSpec;
using flexsdr::conf::Role;

// ----------------- ctor -----------------
FlexSDRPrimary::FlexSDRPrimary(std::string yaml_path)
  : yaml_path_(std::move(yaml_path)) {}

// ----------------- config load -----------------
void FlexSDRPrimary::load_config() {
  cfg_ = flexsdr::conf::PrimaryConfig::load(yaml_path_);
  // (Optional) Log a short summary
  std::printf("[cfg] role=%s, ring_size=%u, prefix_with_role=%s, sep='%s'\n",
              flexsdr::conf::PrimaryConfig::to_string(cfg_.defaults.role).c_str(),
              cfg_.defaults.ring_size,
              cfg_.naming.prefix_with_role ? "true" : "false",
              cfg_.naming.separator.c_str());
}

bool FlexSDRPrimary::is_creator_role() const {
  return cfg_.defaults.role == Role::primary_ue || cfg_.defaults.role == Role::primary_gnb;
}

// ----------------- resource init -----------------
int FlexSDRPrimary::init_resources() {
  // Pools (creators only)
  if (is_creator_role()) {
    int rc = create_pools_();
    if (rc < 0) return rc;
  }

  // Normal TX/RX rings
  {
    int rc = create_or_lookup_rings_();
    if (rc < 0) return rc;
  }

  // Interconnect pair (created on Primary-GNB; looked up elsewhere)
  {
    int rc = create_or_lookup_interconnect_();
    if (rc < 0) return rc;
  }

  return 0;
}

// ----------------- pools -----------------
int FlexSDRPrimary::create_pools_() {
  const auto& pools_cfg = cfg_.effective_pools();
  for (const auto& p : pools_cfg) {
    const std::string mat = cfg_.naming.materialize(cfg_.defaults.role, p.name);
    rte_mempool* mp = nullptr;
    int rc = create_pool_(mat, p.size, p.elt_size, &mp);
    if (rc < 0) {
      std::fprintf(stderr, "[pool] create failed: %s (n=%u elt=%u) rc=%d rte_errno=%d\n",
                   mat.c_str(), p.size, p.elt_size, rc, rte_errno);
      return rc;
    }
    pools_.push_back(mp);
    materialized_pool_names_.push_back(mat);
    std::printf("[pool] created: %s n=%u elt=%u\n", mat.c_str(), p.size, p.elt_size);
  }
  return 0;
}

int FlexSDRPrimary::create_pool_(const std::string& name, unsigned n, unsigned elt_size, rte_mempool** out) {
  // NOTE: tune cache and priv as needed; using cfg_.defaults.mp_cache
  const unsigned cache = std::max(1u, cfg_.defaults.mp_cache);

  // Generic mempool — fine if you manage raw buffers. If you need mbufs, switch to rte_pktmbuf_pool_create.
  rte_mempool* mp = rte_mempool_create(
      name.c_str(), n, elt_size, cache, 0 /*priv_data_size*/,
      nullptr, nullptr, nullptr, nullptr, SOCKET_ID_ANY, 0);

  if (!mp) return -rte_errno ? -rte_errno : -1;
  *out = mp;
  return 0;
}

// ----------------- rings (TX/RX) -----------------
int FlexSDRPrimary::create_or_lookup_rings_() {
  auto tx = cfg_.materialized_tx_rings();
  auto rx = cfg_.materialized_rx_rings();
  tx.insert(tx.end(), rx.begin(), rx.end());

  for (const auto& r : tx) {
    rte_ring* rr = nullptr;
    const unsigned size = r.size ? r.size : cfg_.defaults.ring_size;
    int rc = create_or_lookup_ring_(r.name, size, &rr);
    if (rc < 0) {
      std::fprintf(stderr, "[ring] %s failed (size=%u) rc=%d rte_errno=%d\n",
                   r.name.c_str(), size, rc, rte_errno);
      return rc;
    }
    rings_.push_back(rr);
    materialized_ring_names_.push_back(r.name);
  }
  return 0;
}

// ----------------- interconnect rings -----------------
int FlexSDRPrimary::create_or_lookup_interconnect_() {
  auto ic = cfg_.materialized_interconnect_rings(); // 0, 1, or 2 entries (pg_to_pu / pu_to_pg)
  if (ic.empty()) return 0;

  const bool create_here = (cfg_.defaults.role == Role::primary_gnb);

  for (const auto& r : ic) {
    rte_ring* rr = nullptr;
    const unsigned size = r.size ? r.size : cfg_.defaults.ring_size;
    int rc = create_or_lookup_ring_(r.name, size, &rr);
    if (rc < 0) {
      std::fprintf(stderr, "[ic] %s failed (size=%u) rc=%d rte_errno=%d (creator=%s)\n",
                   r.name.c_str(), size, rc, rte_errno, create_here ? "yes" : "no");
      return rc;
    }
    rings_.push_back(rr);
    materialized_ring_names_.push_back(r.name);
    std::printf("[ic] %s: %s (size=%u)\n",
                create_here ? "created" : "found", r.name.c_str(), size);
  }
  return 0;
}

// ----------------- ring util -----------------
int FlexSDRPrimary::create_or_lookup_ring_(const std::string& name, unsigned size, rte_ring** out) {
  *out = nullptr;

  if (is_creator_role()) {
    // Fast-path flags for SPSC; adjust to MP/MC as needed per ring.
    const unsigned flags = RING_F_SP_ENQ | RING_F_SC_DEQ;
    rte_ring* r = rte_ring_create(name.c_str(), size, SOCKET_ID_ANY, flags);
    if (!r) {
      if (rte_errno == EEXIST) {
        r = rte_ring_lookup(name.c_str());
      }
    }
    if (!r) {
      return -rte_errno ? -rte_errno : -1;
    }
    *out = r;
    std::printf("[ring] created: %s (size=%u)\n", name.c_str(), size);
    return 0;
  }

  // Lookup-only path for secondaries/consumers in primary process as needed
  rte_ring* r = rte_ring_lookup(name.c_str());
  if (!r) {
    return -rte_errno ? -rte_errno : -1;
  }
  *out = r;
  std::printf("[ring] found: %s\n", name.c_str());
  return 0;
}