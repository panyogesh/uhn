#pragma once

#include <string>
#include <vector>

#include <rte_mempool.h>
#include <rte_ring.h>

#include "conf/config_params.hpp"

namespace flexsdr {

class FlexSDRSecondary {
public:
  explicit FlexSDRSecondary(std::string yaml_path);
  int init_resources(); // lookup-only

  // Legacy vector access
  const std::vector<rte_mempool*>& pools()    const { return pools_;    }
  const std::vector<rte_ring*>&     tx_rings() const { return tx_rings_; }
  const std::vector<rte_ring*>&     rx_rings() const { return rx_rings_; }

  // NEW: Queue-specific ring access (for high-throughput applications)
  rte_ring* rx_ring_for_queue(uint16_t qid) const {
    return (qid < rx_rings_.size()) ? rx_rings_[qid] : nullptr;
  }
  
  rte_ring* tx_ring_for_queue(uint16_t qid) const {
    return (qid < tx_rings_.size()) ? tx_rings_[qid] : nullptr;
  }
  
  rte_mempool* pool_for_queue(uint16_t qid) const {
    return (qid < pools_.size()) ? pools_[qid] : nullptr;
  }
  
  // NEW: Get queue counts
  size_t num_rx_queues() const { return rx_rings_.size(); }
  size_t num_tx_queues() const { return tx_rings_.size(); }
  size_t num_pools() const { return pools_.size(); }
  
  // NEW: Statistics support
  struct queue_stats {
    uint64_t rx_packets;
    uint64_t rx_bytes;
    uint64_t tx_packets;
    uint64_t tx_bytes;
    uint64_t ring_full_drops;
    uint64_t mbuf_alloc_fails;
  };
  
  queue_stats get_stats(uint16_t qid) const {
    // For now, return zeros - implementation can be added later
    // Real implementation would query DPDK ring stats
    return queue_stats{};
  }
  
  void reset_stats(uint16_t qid) {
    // Placeholder for stats reset
    (void)qid;
  }

private:
  // config
  int load_config_();

  // local lookup helpers (secondaries never create, never use interconnect)
  int lookup_pools_();
  int lookup_rings_tx_();
  int lookup_rings_rx_();
  int lookup_ring_(const std::string& name, rte_ring** out);

private:
  std::string yaml_path_;
  conf::PrimaryConfig cfg_;

  std::vector<rte_mempool*> pools_;
  std::vector<rte_ring*>    tx_rings_;
  std::vector<rte_ring*>    rx_rings_;
};

} // namespace flexsdr
