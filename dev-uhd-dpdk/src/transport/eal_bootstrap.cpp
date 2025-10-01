#include "eal_bootstrap.hpp"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <algorithm>

#include <rte_errno.h>

namespace flexsdr {

EalBootstrap::EalBootstrap(const conf::PrimaryConfig& cfg, std::string prog)
  : cfg_(cfg), prog_(std::move(prog)) {}

void EalBootstrap::push_flag(const std::string& flag) {
  args_str_storage_.push_back(flag);
}

void EalBootstrap::push_flag_kv(const std::string& k, const std::string& v) {
  args_str_storage_.push_back(k);
  args_str_storage_.push_back(v);
}

void EalBootstrap::rebuild_ptrs() {
  argv_ptrs_.clear();
  argv_ptrs_.reserve(args_str_storage_.size());
  for (auto& s : args_str_storage_) {
    argv_ptrs_.push_back(s.data()); // safe: std::string is contiguous since C++11
  }
}

void EalBootstrap::build_args(const std::vector<std::string>& extra_flags) {
  args_str_storage_.clear();

  // argv[0]
  args_str_storage_.push_back(prog_);

  // ---- Required/common flags from YAML ----
  // file-prefix
  if (!cfg_.eal.file_prefix.empty())
    push_flag_kv("--file-prefix", cfg_.eal.file_prefix);

  // huge-dir
  if (!cfg_.eal.huge_dir.empty())
    push_flag_kv("--huge-dir", cfg_.eal.huge_dir);

  // socket-mem ("1024,1024")
  if (!cfg_.eal.socket_mem.empty())
    push_flag_kv("--socket-mem", cfg_.eal.socket_mem);

  // IOVA mode: "va" | "pa"
  if (!cfg_.eal.iova.empty())
    push_flag_kv("--iova", cfg_.eal.iova);

  // no-pci
  if (cfg_.eal.no_pci) push_flag("--no-pci");

  // lcores (DPDK syntax like "0-3,5")
  if (cfg_.eal.lcores && !cfg_.eal.lcores->empty())
    push_flag_kv("--lcores", *cfg_.eal.lcores);

  // main lcore
  if (cfg_.eal.main_lcore && *cfg_.eal.main_lcore >= 0)
    push_flag_kv("--main-lcore", std::to_string(*cfg_.eal.main_lcore));

  // socket-limit (e.g., "1024,1024")
  if (cfg_.eal.socket_limit && !cfg_.eal.socket_limit->empty())
    push_flag_kv("--socket-limit", *cfg_.eal.socket_limit);

  // NOTE: cfg_.eal.numa and cfg_.eal.isolcpus are informational here.
  // - NUMA is generally enabled by default if hugepages are per-socket.
  // - isolcpus is a kernel cmdline setting; we just log that it exists.

  // ---- Append any extra flags the caller provided ----
  for (const auto& f : extra_flags) {
    // allow both "--key=value" and "--key", "value" forms in a single string
    push_flag(f);
  }

  rebuild_ptrs_();
}

int EalBootstrap::init() {
  rebuild_ptrs_();
  int argc = static_cast<int>(argv_ptrs_.size());
  char** argv = argv_ptrs_.data();

  std::printf("[eal] init with args: %s\n", args_as_cmdline().c_str());
  int consumed = rte_eal_init(argc, argv);
  if (consumed < 0) {
    std::fprintf(stderr, "[eal] init failed: rc=%d rte_errno=%d (%s)\n",
                 consumed, rte_errno, rte_strerror(rte_errno));
  }
  return consumed;
}

std::string EalBootstrap::args_as_cmdline() const {
  std::ostringstream oss;
  for (std::size_t i = 0; i < args_str_storage_.size(); ++i) {
    if (i) oss << ' ';
    // quote any arg with spaces
    const auto& a = args_str_storage_[i];
    if (a.find(' ') != std::string::npos) oss << '"' << a << '"';
    else oss << a;
  }
  return oss.str();
}

} // namespace flexsdr
