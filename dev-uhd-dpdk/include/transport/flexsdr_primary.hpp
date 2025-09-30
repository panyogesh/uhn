#pragma once
#include <cstdint>
#include <string>
#include "config_params.hpp"
#include "dpdk_common.hpp"

namespace flexsdr {

/**
 * Create all pools/rings defined under `primary` in YAML.
 * Uses pktmbuf pools (elt_size => data room, floored at RTE_MBUF_DEFAULT_BUF_SIZE)
 * and rings with RING_F_EXACT_SZ plus provided flags (default SPSC).
 *
 * Returns 0 on success; negative on failure (stderr explains).
 */
int create_primary_objects(const PrimaryConfig& primary,
                           const DefaultConfig& defaults,
                           Handles& out,
                           int socket_id = SOCKET_ID_ANY,
                           unsigned ring_flags = RING_F_SP_ENQ | RING_F_SC_DEQ);

} // namespace flexsdr
