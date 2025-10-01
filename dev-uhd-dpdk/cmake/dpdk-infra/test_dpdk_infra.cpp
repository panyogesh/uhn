// cmake/dpdk-infra/test_dpdk_infra.cpp  (DROP-IN)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>

#include "conf/config_params.hpp"
#include "transport/eal_bootstrap.hpp"
#include "transport/flexsdr_primary.hpp"
#include "transport/flexsdr_secondary.hpp"

using std::string;
using std::vector;

static int run_primary_proc(const string& cfg_path) {
  using namespace flexsdr;

  // Load config
  conf::Config cfg;
  if (int rc = cfg.load_from_yaml(cfg_path.c_str()); rc) {
    std::fprintf(stderr, "config load failed: %d\n", rc);
    return 10;
  }

  // Build EAL args for primary
  vector<string> eal_args = {
    "test_dpdk_infra_primary",
    "--file-prefix", cfg.eal.file_prefix.c_str(),
    "--huge-dir",    cfg.eal.huge_dir.c_str(),
    "--socket-mem",  cfg.eal.socket_mem.c_str(),
    "--iova",        cfg.eal.iova.c_str(),
    "--proc-type=primary",
  };
  if (cfg.eal.no_pci) eal_args.emplace_back("--no-pci");

  // Bring up EAL
  EalBootstrap eal;
  eal.build_args(eal_args);
  std::fprintf(stderr, "[eal] init with args: %s --file-prefix %s --huge-dir %s --socket-mem %s --iova %s --proc-type=primary\n",
               eal_args[0].c_str(),
               cfg.eal.file_prefix.c_str(), cfg.eal.huge_dir.c_str(),
               cfg.eal.socket_mem.c_str(), cfg.eal.iova.c_str());
  if (int rc = eal.init(); rc < 0) return 11;

  // Create primary infra
  FlexSDRPrimary app(cfg);
  if (int rc = app.init_resources(); rc) {
    std::fprintf(stderr, "[primary] init_resources failed rc=%d\n", rc);
    return 11;
  }

  // Keep primary alive while secondary runs. In a real test you’d do IO here.
  // Sleep ~5s; adjust as needed or use IPC for coordination.
  ::sleep(5);

  return 0;
}

static int run_secondary_proc(const string& cfg_path) {
  using namespace flexsdr;

  conf::Config cfg;
  if (int rc = cfg.load_from_yaml(cfg_path.c_str()); rc) {
    std::fprintf(stderr, "config load failed: %d\n", rc);
    return 20;
  }

  vector<string> eal_args = {
    "test_dpdk_infra_secondary",
    "--file-prefix", cfg.eal.file_prefix.c_str(),
    "--huge-dir",    cfg.eal.huge_dir.c_str(),
    "--socket-mem",  cfg.eal.socket_mem.c_str(),
    "--iova",        cfg.eal.iova.c_str(),
    "--proc-type=secondary",
  };
  if (cfg.eal.no_pci) eal_args.emplace_back("--no-pci");

  EalBootstrap eal;
  eal.build_args(eal_args);
  std::fprintf(stderr, "[eal] init with args: %s --file-prefix %s --huge-dir %s --socket-mem %s --iova %s --proc-type=secondary\n",
               eal_args[0].c_str(),
               cfg.eal.file_prefix.c_str(), cfg.eal.huge_dir.c_str(),
               cfg.eal.socket_mem.c_str(), cfg.eal.iova.c_str());
  if (int rc = eal.init(); rc < 0) return 21;

  FlexSDRSecondary app(cfg);
  if (int rc = app.init_resources(); rc) {
    std::fprintf(stderr, "[secondary] init_resources failed rc=%d\n", rc);
    return 21;
  }

  // Optional: perform a small ring exercise here if you have enqueue/dequeue helpers.
  // For now, just succeed if rings were found.
  return 0;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <config.yaml>\n", argv[0]);
    return 2;
  }
  const string cfg_path = argv[1];

  // Fork child #1: primary
  pid_t p1 = ::fork();
  if (p1 < 0) { std::perror("fork primary"); return 100; }
  if (p1 == 0) {
    int rc = run_primary_proc(cfg_path);
    _exit(rc);
  }

  // Small delay to let primary finish ring creation
  ::usleep(300 * 1000);

  // Fork child #2: secondary
  pid_t p2 = ::fork();
  if (p2 < 0) { std::perror("fork secondary"); return 101; }
  if (p2 == 0) {
    int rc = run_secondary_proc(cfg_path);
    _exit(rc);
  }

  // Wait for both
  int st1 = 0, st2 = 0;
  ::waitpid(p1, &st1, 0);
  ::waitpid(p2, &st2, 0);

  std::fprintf(stderr, "[orchestrator] primary exit=%d, secondary exit=%d\n",
               WIFEXITED(st1) ? WEXITSTATUS(st1) : -1,
               WIFEXITED(st2) ? WEXITSTATUS(st2) : -1);

  return (WIFEXITED(st1) ? WEXITSTATUS(st1) : 1)
       + (WIFEXITED(st2) ? WEXITSTATUS(st2) : 1);
}
