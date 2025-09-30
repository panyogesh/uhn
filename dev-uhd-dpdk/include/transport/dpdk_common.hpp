// include/transport/dpdk_common.hpp
#pragma once
#include <string>
#include <unordered_map>

extern "C" {
#include <rte_config.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_mbuf.h>    // <-- needed for rte_pktmbuf_data_room_size
#include <rte_mempool.h>
#include <rte_ring.h>
}

namespace flexsdr {

struct Handles {
  std::unordered_map<std::string, rte_mempool*> pools;
  std::unordered_map<std::string, rte_ring*>    rings;
};

static inline uint32_t ring_capacity(const rte_ring* r) {
  return r ? rte_ring_get_capacity(const_cast<rte_ring*>(r)) : 0u;
}

// Non-const overload
static inline uint32_t pool_data_room(rte_mempool* mp) {
  return mp ? static_cast<uint32_t>(rte_pktmbuf_data_room_size(mp)) : 0u;
}
// Const overload dispatches to non-const
static inline uint32_t pool_data_room(const rte_mempool* mp) {
  return pool_data_room(const_cast<rte_mempool*>(mp));
}

} // namespace flexsdr
