#pragma once
#include <string>
#include <vector>

#include <rte_mempool.h>
#include <rte_ring.h>

#include "conf/config_params.hpp"   // flexsdr::conf::PrimaryConfig

namespace flexsdr {

class FlexSDRSecondary {
public:
  explicit FlexSDRSecondary(std::string yaml_path);

  // Attach to resources created by primary (rings + pools).
  // Returns 0 on success; negative rte-style on failure.
  int init_resources();

  // ---- Read-only accessors for tests / higher layers ----
  const std::vector<rte_ring*>&    tx_rings() const { return tx_rings_; }
  const std::vector<rte_ring*>&    rx_rings() const { return rx_rings_; }
  const std::vector<rte_mempool*>& pools()    const { return pools_;    }

  rte_ring*    ring_by_name(const std::string& name) const;
  rte_mempool* pool_by_name(const std::string& name) const;

  // Diagnostics
  const conf::PrimaryConfig& cfg() const { return cfg_; }

private:
  // Internals
  int load_config_();
  int lookup_pools_();
  int lookup_rings_tx_();
  int lookup_rings_rx_();

  // Strict lookups (return -errno on failure)
  int lookup_ring_strict_(const std::string& name, rte_ring** out);
  int lookup_pool_strict_(const std::string& name, rte_mempool** out);

  // Data
  std::string                 yaml_path_;
  conf::PrimaryConfig         cfg_{};

  std::vector<rte_mempool*>   pools_;
  std::vector<rte_ring*>      tx_rings_;
  std::vector<rte_ring*>      rx_rings_;
};

} // namespace flexsdr
