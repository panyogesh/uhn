#include "flexsdr_primary.hpp"
#include <iostream>

namespace flexsdr {

static rte_mempool* create_pktmbuf_pool(const std::string& name,
                                        uint32_t n, uint32_t cache,
                                        uint32_t data_room_size,
                                        int socket_id)
{
  if (data_room_size < RTE_MBUF_DEFAULT_BUF_SIZE)
    data_room_size = RTE_MBUF_DEFAULT_BUF_SIZE;

  rte_mempool* mp = rte_pktmbuf_pool_create(
      name.c_str(), n, cache, 0, data_room_size, socket_id);

  if (!mp) {
    std::cerr << "[DPDK][ERR] create pool '" << name << "' failed: "
              << rte_strerror(rte_errno) << "\n";
  }
  return mp;
}

static rte_ring* create_ring(const std::string& name,
                             uint32_t size, int socket_id, unsigned flags)
{
  unsigned f = flags | RING_F_EXACT_SZ;
  rte_ring* r = rte_ring_create(name.c_str(), size, socket_id, f);
  if (!r) {
    std::cerr << "[DPDK][ERR] create ring '" << name << "' failed: "
              << rte_strerror(rte_errno) << "\n";
  }
  return r;
}

int create_primary_objects(const PrimaryConfig& primary,
                           const DefaultConfig& defaults,
                           Handles& out,
                           int socket_id,
                           unsigned ring_flags)
{
  // Pools
  for (const auto& pd : primary.pools) {
    auto* mp = create_pktmbuf_pool(pd.name, pd.size, defaults.mp_cache, pd.elt_size, socket_id);
    if (!mp) return -1;
    out.pools[pd.name] = mp;
  }
  // Rings
  for (const auto& rd : primary.rings) {
    auto* r = create_ring(rd.name, rd.size, socket_id, ring_flags);
    if (!r) return -2;
    out.rings[rd.name] = r;
  }

  std::cout << "[DPDK] Primary created " << out.pools.size()
            << " pools and " << out.rings.size() << " rings.\n";
  return 0;
}

} // namespace flexsdr
