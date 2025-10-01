#pragma once

#include <string>
#include <vector>
#include <optional>

#include <rte_eal.h>
#include <rte_log.h>

#include "conf/config_params.hpp"

namespace flexsdr {

class EalBootstrap {
public:
  // Build from full config; you can also pass an explicit "program name" for argv[0].
  explicit EalBootstrap(const conf::PrimaryConfig& cfg, std::string prog = "flexsdr");

  // Compose argv according to cfg.eal and (optionally) extra flags prior to init.
  // Extra flags are appended after config-derived flags (e.g. "--vdev=net_pcap0,iface=...").
  void build_args(const std::vector<std::string>& extra_flags = {});

  // Returns number of consumed args (>=0) or negative on error (same contract as rte_eal_init).
  int init();

  // Return the exact argv vector that will be/was passed to EAL (for logging/testing).
  const std::vector<std::string>& args() const { return args_str_storage_; }

  // Convenience: pretty print the args on one line.
  std::string args_as_cmdline() const;

private:
  const conf::PrimaryConfig& cfg_;
  std::string prog_;
  std::vector<std::string> args_str_storage_;   // owns strings
  std::vector<char*>       argv_ptrs_;          // pointers into args_str_storage_

  // Helpers
  void push_flag(const std::string& flag);                   // e.g., "--no-pci"
  void push_flag_kv(const std::string& k, const std::string& v); // e.g., "--file-prefix", "flexsdr"
  void rebuild_ptrs();                                       // refresh argv_ptrs_ from args_str_storage_
};

} // namespace flexsdr
