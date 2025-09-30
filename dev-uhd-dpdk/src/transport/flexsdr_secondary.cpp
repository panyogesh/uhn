#include "flexsdr_secondary.hpp"
#include "dpdk_common.hpp"

#include <sstream>
#include <iostream>
#include <type_traits>
#include <utility>
#include <vector>
#include <string>

extern "C" {
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_errno.h>
}

namespace flexsdr {

// ---------- C++17-safe SFINAE: detect .ring vs .rings ----------

template <typename T, typename = void>
struct has_member_ring : std::false_type {};

template <typename T>
struct has_member_ring<T, std::void_t<decltype(std::declval<const T>().ring)>>
  : std::true_type {};

// Return the TX ring list regardless of whether the member is named `ring` or `rings`
template <typename S>
static inline const std::vector<std::string>& get_tx_rings(const S& s) {
  if constexpr (has_member_ring<S>::value) {
    return s.ring;
  } else {
    return s.rings; // assume plural exists in your struct
  }
}

// ---------- lookups ----------

static rte_ring* lookup_ring(const std::string& name) {
  rte_ring* r = rte_ring_lookup(name.c_str());
  if (!r) {
    std::cerr << "[DPDK][ERR] ring_lookup('" << name << "') failed: "
              << rte_strerror(rte_errno) << "\n";
  }
  return r;
}

static rte_mempool* lookup_pool(const std::string& name) {
  rte_mempool* mp = rte_mempool_lookup(name.c_str());
  if (!mp) {
    std::cerr << "[DPDK][ERR] mempool_lookup('" << name << "') failed: "
              << rte_strerror(rte_errno) << "\n";
  }
  return mp;
}

// ---------- main attach ----------

int attach_secondary_app(const AppConfig& app,
                         const DefaultConfig& /*defaults*/,
                         const char* app_name,
                         Handles& out,
                         std::string& error_out)
{
  // Inbound ring (single)
  if (!app.inbound_ring.empty()) {
    auto* rin = lookup_ring(app.inbound_ring);
    if (!rin) { error_out = "Missing inbound ring: " + app.inbound_ring; return -1; }
    out.rings[app.inbound_ring] = rin;

    // Watermark is advisory only on modern DPDK
    if (app.rx_stream.ring_watermark) {
      std::cerr << "[DPDK][NOTE] (" << app_name
                << ") rx ring_watermark requested (" << app.rx_stream.ring_watermark
                << ") but watermark API is removed in recent DPDK; ignoring.\n";
    }
  }

  // TX rings
  const auto& tx_rings = get_tx_rings(app.tx_stream);
  if (!tx_rings.empty()) {
    for (const auto& rn : tx_rings) {
      auto* r = lookup_ring(rn);
      if (!r) { error_out = std::string("Missing TX ring: ") + rn; return -2; }
      out.rings[rn] = r;
    }
    if (app.tx_stream.ring_watermark) {
      std::cerr << "[DPDK][NOTE] (" << app_name
                << ") tx ring_watermark requested (" << app.tx_stream.ring_watermark
                << ") but watermark API is removed; ignoring.\n";
    }
  }

  // Pools
  auto* rxp = lookup_pool(app.rx_pool);
  if (!rxp) { error_out = "Missing rx_pool: " + app.rx_pool; return -3; }
  out.pools[app.rx_pool] = rxp;

  auto* txp = lookup_pool(app.tx_pool);
  if (!txp) { error_out = "Missing tx_pool: " + app.tx_pool; return -4; }
  out.pools[app.tx_pool] = txp;

  // Conservative payload sanity (advisory only)
  const uint32_t bytes_per_cplx = 4; // "cs16" I16,Q16
  const uint32_t spp = app.tx_stream.spp ? app.tx_stream.spp
                                         : (app.rx_stream.spp ? app.rx_stream.spp : 64);
  const uint32_t channels = tx_rings.empty()
                              ? 1u
                              : static_cast<uint32_t>(tx_rings.size());
  const uint32_t payload = spp * channels * bytes_per_cplx;

  const uint32_t rx_room = pool_data_room(rxp);
  const uint32_t tx_room = pool_data_room(txp);
  if (rx_room && rx_room < payload)
    std::cerr << "[DPDK][WARN] (" << app_name << ") rx_pool '" << app.rx_pool
              << "' data_room=" << rx_room << " < payload=" << payload << "\n";
  if (tx_room && tx_room < payload)
    std::cerr << "[DPDK][WARN] (" << app_name << ") tx_pool '" << app.tx_pool
              << "' data_room=" << tx_room << " < payload=" << payload << "\n";

  std::cout << "[DPDK] (" << app_name << ") attached OK (rings=" << out.rings.size()
            << ", pools=" << out.pools.size() << ")\n";
  return 0;
}

} // namespace flexsdr
