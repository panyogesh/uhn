#pragma once
#include <string>               // <-- ensure this is present
#include <vector>
#include <optional>
#include <rte_mempool.h>
#include <rte_ring.h>
#include "conf/config_params.hpp"

namespace flexsdr {

class FlexSDRSecondary {
public:
  explicit FlexSDRSecondary(std::string yaml_path);

  int init_resources();
  const std::vector<rte_mempool*>& pools()    const { return pools_;    }
  const std::vector<rte_ring*>&     tx_rings() const { return tx_rings_; }
  const std::vector<rte_ring*>&     rx_rings() const { return rx_rings_; }

private:
  int  load_config_();
  int  lookup_pools_();
  int  lookup_rings_tx_();
  int  lookup_rings_rx_();
  int  lookup_interconnect_();

  // >>> ADD THESE TWO LINES <<<
  static int lookup_ring_(const std::string& name, rte_ring** out);
  static int lookup_pool_(const std::string& name, rte_mempool** out);

private:
  std::string yaml_path_;
  conf::PrimaryConfig cfg_;

  std::vector<rte_mempool*> pools_;
  std::vector<rte_ring*>    tx_rings_;
  std::vector<rte_ring*>    rx_rings_;
};

} // namespace flexsdr
