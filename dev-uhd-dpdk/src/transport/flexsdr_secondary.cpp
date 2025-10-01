#include "transport/flexsdr_secondary.hpp"
#include "conf/config_params.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// DPDK
#include <rte_config.h>
#include <rte_eal.h>
#include <rte_errno.h>
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

// For secondary, we keep it simple: use defaults.* rings.
// (If your config parser also fills cfg_.ue / cfg_.gnb role blocks and you
// want to prefer those, you can add optional checks here similar to primary.)
static inline const std::vector<conf::RingSpec>&
collect_tx_rings_(const conf::PrimaryConfig& cfg) {
  return cfg.defaults.tx_stream.rings;
}

static inline const std::vector<conf::RingSpec>&
collect_rx_rings_(const conf::PrimaryConfig& cfg) {
  return cfg.defaults.rx_stream.rings;
}

// --------------------------- FlexSDRSecondary --------------------------------

FlexSDRSecondary::FlexSDRSecondary(std::string yaml_path)
  : yaml_path_(std::move(yaml_path)) {
  (void)load_config_();
  std::fprintf(stderr, "[secondary] constructed FlexSDRSecondary\n");
}

int FlexSDRSecondary::load_config_() {
  int rc = conf::load_from_yaml(yaml_path_.c_str(), cfg_);
  if (rc) {
    std::fprintf(stderr, "[secondary] load_from_yaml failed rc=%d\n", rc);
    return rc;
  }
  return 0;
}

int FlexSDRSecondary::init_resources() {
  std::fprintf(stderr, "[secondary] init_resources: role=%s ring_size=%u\n",
               role_str(cfg_.defaults.role), cfg_.defaults.ring_size);

  if (int rc = lookup_rings_tx_(); rc) return rc;
  if (int rc = lookup_rings_rx_(); rc) return rc;

  return 0;
}

// --------------------------- ring lookups ------------------------------------

int FlexSDRSecondary::lookup_ring_(const std::string& name, rte_ring** out) {
  *out = nullptr;
  rte_ring* r = rte_ring_lookup(name.c_str());
  if (!r) {
    std::fprintf(stderr, "[ring] lookup failed: %s rc=%d rte_errno=%d\n",
                 name.c_str(), -2, rte_errno);
    return -2;
  }
  *out = r;
  return 0;
}

int FlexSDRSecondary::lookup_rings_tx_() {
  const auto& rings = collect_tx_rings_(cfg_);
  for (const auto& r : rings) {
    rte_ring* ptr = nullptr;
    int rc = lookup_ring_(r.name, &ptr);
    if (rc) return rc;
    std::fprintf(stderr, "[ring] found TX: %s (size=%u)\n",
                 r.name.c_str(), rte_ring_get_size(ptr));
    tx_rings_.push_back(ptr);
  }
  return 0;
}

int FlexSDRSecondary::lookup_rings_rx_() {
  const auto& rings = collect_rx_rings_(cfg_);
  for (const auto& r : rings) {
    rte_ring* ptr = nullptr;
    int rc = lookup_ring_(r.name, &ptr);
    if (rc) return rc;
    std::fprintf(stderr, "[ring] found RX: %s (size=%u)\n",
                 r.name.c_str(), rte_ring_get_size(ptr));
    rx_rings_.push_back(ptr);
  }
  return 0;
}

} // namespace flexsdr
