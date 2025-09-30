#include "eal_bootstrap.hpp"
#include <cstring>
#include <memory>
#include <iostream>

extern "C" {
#include <rte_eal.h>
#include <rte_errno.h>
}

namespace flexsdr {

static char* dup_cstr(const std::string& s) {
  char* p = static_cast<char*>(std::malloc(s.size() + 1));
  if (!p) return nullptr;
  std::memcpy(p, s.c_str(), s.size() + 1);
  return p;
}

int init_eal_from_config(const EALConfig& eal, bool secondary,
                         const std::vector<std::string>& extra_args)
{
  std::vector<std::string> sargs;
  sargs.emplace_back("flexsdr"); // argv[0]
  sargs.emplace_back(std::string("--proc-type=") + (secondary ? "secondary" : "primary"));
  sargs.emplace_back(std::string("--file-prefix=") + eal.file_prefix);
  sargs.emplace_back(std::string("--huge-dir=") + eal.huge_dir);
  sargs.emplace_back(std::string("--socket-mem=") + eal.socket_mem);
  sargs.emplace_back(std::string("--iova=") + eal.iova);
  if (eal.no_pci) sargs.emplace_back("--no-pci");
  for (const auto& x : extra_args) sargs.emplace_back(x);

  // Build mutable argv
  std::vector<char*> argv;
  argv.reserve(sargs.size());
  std::vector<char*> to_free;
  to_free.reserve(sargs.size());

  for (const auto& s : sargs) {
    char* p = dup_cstr(s);
    if (!p) { std::cerr << "OOM building EAL args\n"; return -1; }
    argv.push_back(p);
    to_free.push_back(p);
  }

  int argc = static_cast<int>(argv.size());
  int rc = rte_eal_init(argc, argv.data());

  for (char* p : to_free) std::free(p);

  if (rc < 0) {
    std::cerr << "[DPDK][ERR] rte_eal_init failed: " << rte_strerror(rte_errno) << "\n";
  }
  return rc;
}

} // namespace flexsdr
