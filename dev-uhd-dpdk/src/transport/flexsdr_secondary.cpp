#include "flexsdr_secondary.hpp"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <algorithm>

// DPDK
#include <rte_log.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_mempool.h>
#include <rte_ring.h>

using namespace flexsdr;
using flexsdr::conf::RingSpec;
using flexsdr::conf::PoolSpec;
using flexsdr::conf::Role;

// ----------------- ctor -----------------
FlexSDRSecondary::FlexSDRSecondary(std::string yaml_path)
  : yaml_path_(std::move(yaml_path)) {}

// ----------------- config load -----------------
void FlexSDRSecondary::load_config() {
  cfg_ = flexsdr::conf::PrimaryConfig::load(yaml_path_);
  // (Optional) Log a short summary
  std::printf("[cfg] (secondary) role=%s, ring_size=%u, prefix_with_role=%s, sep='%s'\n",
              flexsdr::conf::PrimaryConfig::to_string(cfg_.defaults.role).c_str(),
              cfg_.defaults.ring_size,
              cfg_.naming.prefix_with_role ? "true" : "false",
              cfg_.naming.separator.c_str());
}

// ----------------- resource init -----------------
int FlexSDRSecondary::init_resources() {
  // 1) Optional pools defined under role block (usually none for secondaries)
  {
    int rc = lookup_role_pools_();
    if (rc < 0) return rc;
  }

  // 2) TX/RX rings lookup
  {
    int rc = lookup_rings_();
    if (rc < 0) return rc;
  }

  // 3) Interconnect rings lookup
  {
    int rc = lookup_interconnect_rings_();
    if (rc < 0) return rc;
  }

  return 0;
}

// ----------------- pools (optional) -----------------
int FlexSDRSecondary::lookup_role_pools_() {
  const auto& pools_cfg = cfg_.effective_pools();
  if (pools_cfg.empty()) return 0;

  for (const auto& p : pools_cfg) {
    // Materialize the name using the *secondary* role (e.g., "ue_inbound_pool")
    const std::string mat = cfg_.naming.materialize(cfg_.defaults.role, p.name);
    rte_mempool* mp = nullptr;
    int rc = lookup_pool_(mat, &mp);
    if (rc < 0) {
      std::fprintf(stderr, "[pool] lookup failed: %s rc=%d rte_errno=%d\n",
                   mat.c_str(), rc, rte_errno);
      return rc;
    }
    pools_.push_back(mp);
    materialized_pool_names_.push_back(mat);
    std::printf("[pool] found: %s\n", mat.c_str());
  }
  return 0;
}

int FlexSDRSecondary::lookup_pool_(const std::string& name, rte_mempool** out) {
  *out = nullptr;
#if RTE_VERSION >= RTE_VERSION_NUM(21, 11, 0, 0)  // adjust if you want version branching
  rte_mempool* mp = rte_mempool_lookup(name.c_str());
#else
  rte_mempool* mp = rte_mempool_lookup(name.c_str());
#endif
  if (!mp) return -rte_errno ? -rte_errno : -1;
  *out = mp;
  return 0;
}

// ----------------- rings (TX/RX) -----------------
int FlexSDRSecondary::lookup_rings_() {
  auto tx = cfg_.materialized_tx_rings();
  auto rx = cfg_.materialized_rx_rings();
  tx.insert(tx.end(), rx.begin(), rx.end());

  for (const auto& r : tx) {
    rte_ring* rr = nullptr;
    int rc = lookup_ring_(r.name, &rr);
    if (rc < 0) {
      std::fprintf(stderr, "[ring] lookup failed: %s rc=%d rte_errno=%d\n",
                   r.name.c_str(), rc, rte_errno);
      return rc;
    }
    rings_.push_back(rr);
    materialized_ring_names_.push_back(r.name);
    std::printf("[ring] found: %s\n", r.name.c_str());
  }
  return 0;
}

// ----------------- interconnect rings -----------------
int FlexSDRSecondary::lookup_interconnect_rings_() {
  auto ic = cfg_.materialized_interconnect_rings();
  if (ic.empty()) return 0;

  for (const auto& r : ic) {
    rte_ring* rr = nullptr;
    int rc = lookup_ring_(r.name, &rr);
    if (rc < 0) {
      std::fprintf(stderr, "[ic] lookup failed: %s rc=%d rte_errno=%d\n",
                   r.name.c_str(), rc, rte_errno);
      return rc;
    }
    rings_.push_back(rr);
    materialized_ring_names_.push_back(r.name);
    std::printf("[ic] found: %s\n", r.name.c_str());
  }
  return 0;
}

// ----------------- util -----------------
int FlexSDRSecondary::lookup_ring_(const std::string& name, rte_ring** out) {
  *out = nullptr;
  rte_ring* r = rte_ring_lookup(name.c_str());
  if (!r) return -rte_errno ? -rte_errno : -1;
  *out = r;
  return 0;
}
