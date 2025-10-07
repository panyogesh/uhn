#include "transport/flexsdr_secondary.hpp"
#include "conf/config_params.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// DPDK (must be in extern "C" block)
extern "C" {
#include <rte_config.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ring.h>
#include <rte_lcore.h>
#include <rte_memory.h>
#include <rte_mbuf.h>
}

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

static inline const std::vector<conf::PoolSpec>&
collect_pools_(const conf::PrimaryConfig& cfg) {
  // Secondary looks up pools created by primary
  // Check role-specific config first, then fall back to defaults
  if (cfg.primary_ue.has_value() && !cfg.primary_ue->pools.empty())
    return cfg.primary_ue->pools;
  if (cfg.primary_gnb.has_value() && !cfg.primary_gnb->pools.empty())
    return cfg.primary_gnb->pools;
  static const std::vector<conf::PoolSpec> kEmpty;
  return kEmpty;
}

// --------------------------- FlexSDRSecondary --------------------------------

FlexSDRSecondary::FlexSDRSecondary(std::string yaml_path)
  : yaml_path_(std::move(yaml_path)) {
  (void)load_config_();
  std::fprintf(stderr, "[secondary] constructed FlexSDRSecondary\n");
}

FlexSDRSecondary::~FlexSDRSecondary() {
  // No mbuf cache to clean up
  std::fprintf(stderr, "[secondary] destroyed FlexSDRSecondary\n");
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

  if (int rc = lookup_pools_(); rc) return rc;
  if (int rc = lookup_rings_tx_(); rc) return rc;
  if (int rc = lookup_rings_rx_(); rc) return rc;
  // No mbuf cache needed - using direct alloc/free

  return 0;
}

// --------------------------- pool & ring lookups ------------------------------------

int FlexSDRSecondary::lookup_pools_() {
  const auto& pools = collect_pools_(cfg_);
  for (const auto& p : pools) {
    rte_mempool* mp = rte_mempool_lookup(p.name.c_str());
    if (!mp) {
      std::fprintf(stderr, "[pool] lookup failed: %s rc=%d rte_errno=%d\n",
                   p.name.c_str(), -2, rte_errno);
      return -2;
    }
    std::fprintf(stderr, "[pool] found: %s (capacity=%u)\n",
                 p.name.c_str(), rte_mempool_avail_count(mp) + rte_mempool_in_use_count(mp));
    pools_.push_back(mp);
  }
  return 0;
}

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

bool FlexSDRSecondary::send_burst(std::size_t chan,
                                   const void* data,
                                   std::size_t bytes,
                                   uint64_t tsf,
                                   uint32_t spp,
                                   uint16_t fmt,
                                   bool sob,
                                   bool eob) {
  // Suppress unused parameter warnings
  (void)tsf;
  (void)spp;
  (void)fmt;
  (void)sob;
  (void)eob;
  
  if (chan >= tx_rings_.size() || !tx_rings_[chan] || chan >= pools_.size() || !pools_[chan]) {
    static uint64_t err_count = 0;
    if (++err_count % 1000 == 1) {
      std::fprintf(stderr, "[send_burst] ERROR: Invalid channel %zu (tx_rings=%zu, pools=%zu)\n",
                   chan, tx_rings_.size(), pools_.size());
    }
    return false;
  }

  rte_ring* r = tx_rings_[chan];
  rte_mempool* pool = pools_[chan];

  // Allocate mbuf directly from pool (simple approach)
  rte_mbuf* m = rte_pktmbuf_alloc(pool);
  if (!m) {
    static uint64_t alloc_fail_count = 0;
    if (++alloc_fail_count % 1000 == 1) {
      std::fprintf(stderr, "[send_burst] ERROR: mbuf alloc failed (pool=%s, avail=%u, in_use=%u)\n",
                   pool->name, rte_mempool_avail_count(pool), 
                   rte_mempool_in_use_count(pool));
    }
    return false;
  }

  // Validate mbuf structure
  if (!m->buf_addr || m->buf_len == 0) {
    static uint64_t invalid_mbuf_count = 0;
    if (++invalid_mbuf_count % 1000 == 1) {
      std::fprintf(stderr, "[send_burst] ERROR: Invalid mbuf (buf_addr=%p, buf_len=%u)\n",
                   m->buf_addr, m->buf_len);
    }
    rte_pktmbuf_free(m);
    return false;
  }

  // Check if we have enough tailroom
  uint16_t tailroom = rte_pktmbuf_tailroom(m);
  if (tailroom < bytes) {
    static uint64_t space_err_count = 0;
    if (++space_err_count % 1000 == 1) {
      std::fprintf(stderr, "[send_burst] ERROR: Insufficient space (need=%zu, have=%u)\n",
                   bytes, tailroom);
    }
    rte_pktmbuf_free(m);
    return false;
  }

  // Get data pointer with proper bounds checking
  char* buf_addr = static_cast<char*>(m->buf_addr);
  if (!buf_addr) {
    static uint64_t bad_addr_count = 0;
    if (++bad_addr_count % 1000 == 1) {
      std::fprintf(stderr, "[send_burst] ERROR: mbuf buf_addr is NULL\n");
    }
    rte_pktmbuf_free(m);
    return false;
  }

  // Calculate actual data pointer: buf_addr + headroom
  char* data_ptr = buf_addr + m->data_off;
  
  // Verify the pointer is within buffer bounds
  if (data_ptr < buf_addr || data_ptr + bytes > buf_addr + m->buf_len) {
    static uint64_t bounds_err_count = 0;
    if (++bounds_err_count % 1000 == 1) {
      std::fprintf(stderr, "[send_burst] ERROR: Data pointer out of bounds (buf_addr=%p, data_ptr=%p, bytes=%zu, buf_len=%u)\n",
                   buf_addr, data_ptr, bytes, m->buf_len);
    }
    rte_pktmbuf_free(m);
    return false;
  }

  // Validate source pointer
  if (!data) {
    static uint64_t null_src_count = 0;
    if (++null_src_count % 1000 == 1) {
      std::fprintf(stderr, "[send_burst] ERROR: Source data pointer is NULL\n");
    }
    rte_pktmbuf_free(m);
    return false;
  }

  // DEBUG: Print all values before memcpy
  static uint64_t call_count = 0;
  if (++call_count <= 3) {
    std::fprintf(stderr, "[send_burst DEBUG #%lu] chan=%zu, bytes=%zu\n", call_count, chan, bytes);
    std::fprintf(stderr, "  buf_addr=%p, data_off=%u, buf_len=%u\n", buf_addr, m->data_off, m->buf_len);
    std::fprintf(stderr, "  data_ptr=%p, data=%p\n", data_ptr, data);
    std::fprintf(stderr, "  bounds: data_ptr >= buf_addr? %d\n", data_ptr >= buf_addr);
    std::fprintf(stderr, "  bounds: data_ptr + bytes <= buf_addr + buf_len? %d\n", 
                 data_ptr + bytes <= buf_addr + m->buf_len);
  }

  // Copy data
  std::memcpy(data_ptr, data, bytes);
  
  // Update mbuf lengths
  m->data_len = static_cast<uint16_t>(bytes);
  m->pkt_len = static_cast<uint32_t>(bytes);

  // Enqueue to DPDK ring (single-producer per channel)
  const unsigned enq = rte_ring_enqueue_burst(r, reinterpret_cast<void**>(&m), 1, nullptr);
  if (!enq) {
    static uint64_t ring_full_count = 0;
    if (++ring_full_count % 1000 == 1) {
      std::fprintf(stderr, "[send_burst] ERROR: Ring full (ring=%s, capacity=%u, free=%u)\n",
                   r->name, rte_ring_get_capacity(r), rte_ring_free_count(r));
    }
    // Free mbuf since we couldn't enqueue it
    rte_pktmbuf_free(m);
    return false;
  }
  // mbuf successfully enqueued - it will be freed by the consumer
  return true;
}

// --------------------------- mbuf cache helpers ---------------------------------

int FlexSDRSecondary::init_mbuf_cache_() {
  // Initialize cache for all channels
  mbuf_cache_.resize(tx_rings_.size());
  
  for (size_t chan = 0; chan < tx_rings_.size(); ++chan) {
    if (chan >= pools_.size() || !pools_[chan]) {
      std::fprintf(stderr, "[mbuf_cache] ERROR: No pool for channel %zu\n", chan);
      return -1;
    }
    
    // Pre-allocate mbufs for this channel's cache
    for (size_t i = 0; i < MBUF_CACHE_SIZE; ++i) {
      rte_mbuf* m = rte_pktmbuf_alloc(pools_[chan]);
      if (!m) {
        std::fprintf(stderr, "[mbuf_cache] WARNING: Could only pre-allocate %zu/%zu mbufs for channel %zu\n",
                     i, MBUF_CACHE_SIZE, chan);
        break;
      }
      mbuf_cache_[chan].push_back(m);
    }
    
    std::fprintf(stderr, "[mbuf_cache] Channel %zu: pre-allocated %zu mbufs\n",
                 chan, mbuf_cache_[chan].size());
  }
  
  return 0;
}

void FlexSDRSecondary::cleanup_mbuf_cache_() {
  for (size_t chan = 0; chan < mbuf_cache_.size(); ++chan) {
    for (rte_mbuf* m : mbuf_cache_[chan]) {
      if (m) {
        rte_pktmbuf_free(m);
      }
    }
    mbuf_cache_[chan].clear();
  }
  mbuf_cache_.clear();
}

rte_mbuf* FlexSDRSecondary::get_mbuf_from_cache(size_t chan) {
  if (chan >= mbuf_cache_.size()) {
    return nullptr;
  }
  
  // Try to get from cache first
  if (!mbuf_cache_[chan].empty()) {
    rte_mbuf* m = mbuf_cache_[chan].front();
    mbuf_cache_[chan].pop_front();
    return m;
  }
  
  // Cache empty - allocate new mbuf (fallback)
  if (chan >= pools_.size() || !pools_[chan]) {
    return nullptr;
  }
  
  return rte_pktmbuf_alloc(pools_[chan]);
}

void FlexSDRSecondary::return_mbuf_to_cache(size_t chan, rte_mbuf* m) {
  if (!m || chan >= mbuf_cache_.size()) {
    if (m) rte_pktmbuf_free(m);
    return;
  }
  
  // If cache is full, free the mbuf instead of caching
  if (mbuf_cache_[chan].size() >= MBUF_CACHE_SIZE) {
    rte_pktmbuf_free(m);
    return;
  }
  
  // Return to cache for reuse
  mbuf_cache_[chan].push_back(m);
}

} // namespace flexsdr
