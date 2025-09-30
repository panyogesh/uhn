#pragma once
#include <string>
#include <vector>
#include "conf/config_params.hpp"

namespace flexsdr {

/**
 * Initialize DPDK EAL from YAML EALConfig.
 * Builds argv like:
 *  app --proc-type=primary|secondary --file-prefix=... --huge-dir=... --socket-mem=... --iova=va|pa [--no-pci] <extra...>
 *
 * Returns rte_eal_init() rc (>=0 on success), throws on fatal argument build errors.
 */
int init_eal_from_config(const EALConfig& eal, bool secondary,
                         const std::vector<std::string>& extra_args = {});

} // namespace flexsdr
