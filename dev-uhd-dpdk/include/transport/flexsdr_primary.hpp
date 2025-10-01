#pragma once
#include <string>
#include <vector>
#include <memory>

#include <rte_mempool.h>
#include <rte_ring.h>

#include "conf/config_params.hpp"   // flexsdr::conf::PrimaryConfig

namespace flexsdr {

class FlexSDRPrimary {
public:
  explicit FlexSDRPrimary(std::string yaml_path);

  // Load YAML and create resources (pools + rings).
  // Returns 0 on success; negative rte-style on failure.
  int init_resources();

  // ---- Zero-copy access for test / app plumbing (avoid rte_*_lookup) ----
  const std::vector<rte_ring*>&    tx_rings() const { return tx_rings_; }
  const std::vector<rte_ring*>&    rx_rings() const { return rx_rings_; }
  const std::vector<rte_mempool*>& pools()    const { return pools_;    }

  rte_ring*    ring_by_name(const std::string& name) const;
  rte_mempool* pool_by_name(const std::string& name) const;

  // For diagnostics
  const conf::PrimaryConfig& cfg() const { return cfg_; }

private:
  // Helpers
  int  load_config_();
  int  create_pools_();
  int  create_rings_tx_();
  int  create_rings_rx_();

  // Create (or get-if-exists) a ring; returns 0 on success, -errno on fail
  int  create_or_lookup_ring_(const std::string& name, unsigned size, rte_ring** out);

  // Data
  std::string                 yaml_path_;
  conf::PrimaryConfig         cfg_{};          // parsed configuration

  std::vector<rte_mempool*>   pools_;
  std::vector<rte_ring*>      tx_rings_;
  std::vector<rte_ring*>      rx_rings_;
};

} // namespace flexsdr
