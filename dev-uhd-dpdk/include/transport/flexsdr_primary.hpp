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

class FlexSDRPrimary {
public:
  // Construct with a path to either configuration-ue.yaml or configuration-gnb.yaml
  explicit FlexSDRPrimary(std::string yaml_path);
  ~FlexSDRPrimary() = default;

  // Parse YAML and stage internal plans (no DPDK objects created yet)
  void load_config();

  // Create or lookup DPDK objects according to role:
  // - For primary-* roles: create pools+rings (including interconnect on primary-gnb)
  // - For ue/gnb roles: lookup rings only (and interconnect)
  // Returns 0 on success; negative errno otherwise.
  int init_resources();

  // Accessors
  const flexsdr::conf::PrimaryConfig& cfg() const { return cfg_; }
  flexsdr::conf::Role role() const { return cfg_.defaults.role; }

  // Materialized (prefixed) ring/pool names that we created/checked
  const std::vector<std::string>& created_or_found_rings() const { return materialized_ring_names_; }
  const std::vector<std::string>& created_or_found_pools() const { return materialized_pool_names_; }

public:
  // Helpers
  bool is_creator_role() const;  // primary-ue or primary-gnb
  int  create_pools_();
  int  create_or_lookup_rings_();
  int  create_or_lookup_interconnect_();

  // Creation/lookup utility
  int  create_or_lookup_ring_(const std::string& name, unsigned size, rte_ring** out);
  int  create_pool_(const std::string& name, unsigned n, unsigned elt_size, rte_mempool** out);

private:
  std::string yaml_path_;
  flexsdr::conf::PrimaryConfig cfg_;

  // Keep pointers for later teardown/use (if needed)
  std::vector<rte_ring*>     rings_;
  std::vector<rte_mempool*>  pools_;

  // For logging/debug
  std::vector<std::string> materialized_ring_names_;
  std::vector<std::string> materialized_pool_names_;
};

} // namespace flexsdr
