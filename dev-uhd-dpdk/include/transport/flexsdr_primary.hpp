#pragma once

#include <string>
#include <vector>

#include <rte_mempool.h>
#include <rte_ring.h>

#include "conf/config_params.hpp"

namespace flexsdr {

class FlexSDRPrimary {
public:
  explicit FlexSDRPrimary(std::string yaml_path);
  int init_resources();

  const std::vector<rte_mempool*>& pools()       const { return pools_;       }
  const std::vector<rte_ring*>&     tx_rings()    const { return tx_rings_;    }
  const std::vector<rte_ring*>&     rx_rings()    const { return rx_rings_;    }

  // Interconnect (only valid when role is primary-gnb or primary-ue)
  const std::vector<rte_ring*>&     ic_tx_rings() const { return ic_tx_rings_; }
  const std::vector<rte_ring*>&     ic_rx_rings() const { return ic_rx_rings_; }

private:
  // config
  int load_config_();

  // local resources
  int create_pools_();     // creator roles
  int create_rings_tx_();  // creator roles
  int create_rings_rx_();  // creator roles

  // helpers
  int create_ring_(const std::string& name, unsigned size, rte_ring** out);
  int lookup_pools_();     // (if you reuse primary for lookup-only roles)
  int lookup_rings_tx_();
  int lookup_rings_rx_();
  int lookup_ring_(const std::string& name, rte_ring** out);

  // interconnect (used ONLY on primaries)
  int create_interconnect_(); // primary-gnb creates IC rings (+pool if configured)
  int lookup_interconnect_(); // primary-ue looks up IC rings

private:
  std::string yaml_path_;
  conf::PrimaryConfig cfg_;

  // local IO
  std::vector<rte_mempool*> pools_;
  std::vector<rte_ring*>    tx_rings_;
  std::vector<rte_ring*>    rx_rings_;

  // interconnect (if applicable)
  std::vector<rte_ring*>    ic_tx_rings_;
  std::vector<rte_ring*>    ic_rx_rings_;
};

} // namespace flexsdr
