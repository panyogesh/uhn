#pragma once

#include <string>
#include <vector>
#include <optional>
#include <memory>

#include "conf/config_params.hpp"

// DPDK
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_ring.h>

namespace flexsdr {

class FlexSDRSecondary {
public:
  // Construct with a path to either configuration-ue.yaml or configuration-gnb.yaml
  explicit FlexSDRSecondary(std::string yaml_path);
  ~FlexSDRSecondary() = default;

  // Parse YAML and stage internal plans (no DPDK objects touched yet)
  void load_config();

  // Look up DPDK objects according to role (ue/gnb):
  // - Lookup rings (tx/rx) and interconnect rings
  // - Optionally lookup mempools if present in role block
  // Returns 0 on success; negative errno otherwise.
  int init_resources();

  // Accessors
  const flexsdr::conf::PrimaryConfig& cfg() const { return cfg_; }
  flexsdr::conf::Role role() const { return cfg_.defaults.role; }

  // Materialized (prefixed) ring/pool names we resolved
  const std::vector<std::string>& found_rings() const { return materialized_ring_names_; }
  const std::vector<std::string>& found_pools() const { return materialized_pool_names_; }

  // Raw pointers if callers need them
  const std::vector<rte_ring*>&     rings() const { return rings_; }
  const std::vector<rte_mempool*>&  pools() const { return pools_; }

private:
  // Helpers
  int  lookup_role_pools_();            // if any are defined under ue/gnb blocks
  int  lookup_rings_();                 // tx/rx
  int  lookup_interconnect_rings_();    // pg_to_pu / pu_to_pg

  // Utilities
  int  lookup_ring_(const std::string& name, rte_ring** out);
  int  lookup_pool_(const std::string& name, rte_mempool** out); // optional; will try standard lookup

private:
  std::string yaml_path_;
  flexsdr::conf::PrimaryConfig cfg_;

  // Resolved objects
  std::vector<rte_ring*>     rings_;
  std::vector<rte_mempool*>  pools_;

  // For logging/debug
  std::vector<std::string> materialized_ring_names_;
  std::vector<std::string> materialized_pool_names_;
};

} // namespace flexsdr
