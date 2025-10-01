#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>

#include "transport/flexsdr_primary.hpp"
#include "transport/flexsdr_secondary.hpp"

using std::string;

static int run_primary(const string& cfg) {
  std::fprintf(stderr, "[primary pid=%d] starting with %s\n", getpid(), cfg.c_str());
  setenv("RTE_LOG_LEVEL", "8", 1); // max logging
  flexsdr::FlexSDRPrimary app(cfg);
  std::fprintf(stderr, "[primary pid=%d] constructed FlexSDRPrimary\n", getpid());
  int rc = app.init_resources();
  if (rc) { std::fprintf(stderr, "[primary pid=%d] init_resources failed rc=%d\n", getpid(), rc); return -1; }
  std::fprintf(stderr, "[primary pid=%d] resources ready; idle...\n", getpid());
  return 0;
}

static int run_secondary(const string& cfg) {
  std::fprintf(stderr, "[secondary pid=%d] starting with %s\n", getpid(), cfg.c_str());
  setenv("RTE_LOG_LEVEL", "8", 1); // max logging
  flexsdr::FlexSDRSecondary app(cfg);
  std::fprintf(stderr, "[secondary pid=%d] constructed FlexSDRSecondary\n", getpid());
  int rc = app.init_resources();
  if (rc) { std::fprintf(stderr, "[secondary pid=%d] init_resources failed rc=%d\n", getpid(), rc); return -1; }
  std::fprintf(stderr, "[secondary pid=%d] resources ready; idle...\n", getpid());
  return 0;
}

static int exec_role(const char* self, const char* role, const char* cfg) {
  execlp(self, self, "--role", role, "--config", cfg, (char*)nullptr);
  std::perror("execlp");
  return -1;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <config.yaml>\n", argv[0]);
    return 2;
  }

  // Role-specific direct invocation: test_dpdk_infra --role primary|secondary --config cfg.yaml
  if (argc == 5 && std::strcmp(argv[1], "--role") == 0 && std::strcmp(argv[3], "--config") == 0) {
    string role = argv[2], cfg = argv[4];
    if (role == string("primary"))   return run_primary(cfg);
    if (role == string("secondary")) return run_secondary(cfg);
    std::fprintf(stderr, "unknown role: %s\n", role.c_str());
    return 2;
  }

  // Orchestrator mode: test_dpdk_infra cfg.yaml
  string cfg_path = argv[1];

  // Spawn primary
  pid_t p_primary = fork();
  if (p_primary < 0) { std::perror("fork primary"); return 1; }
  if (p_primary == 0) {
    // Child: exec into role=primary (prevents double construction in the same PID)
    return exec_role(argv[0], "primary", cfg_path.c_str());
  }

  // Spawn secondary
  pid_t p_secondary = fork();
  if (p_secondary < 0) { std::perror("fork secondary"); return 1; }
  if (p_secondary == 0) {
    // Child: exec into role=secondary
    // tiny delay so primary creates objects first
    usleep(150 * 1000);
    return exec_role(argv[0], "secondary", cfg_path.c_str());
  }

  // Parent: wait both
  int st_p = 0, st_s = 0;
  waitpid(p_primary,   &st_p, 0);
  waitpid(p_secondary, &st_s, 0);

  auto exit_or_sig = [](int st) {
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return -WTERMSIG(st);
    return -99;
  };

  int ep = exit_or_sig(st_p);
  int es = exit_or_sig(st_s);
  std::fprintf(stderr, "[orchestrator] primary=%d secondary=%d\n", ep, es);
  return (ep || es) ? 1 : 0;
}
