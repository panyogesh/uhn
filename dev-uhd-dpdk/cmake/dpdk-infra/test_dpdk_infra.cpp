// test_dpdk_infra.cpp
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <csignal>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_version.h>

#include "transport/flexsdr_primary.hpp"
#include "transport/flexsdr_secondary.hpp"

// ---- DPDK version helpers (builds on 21.x .. 24.x) -----------------------
#ifndef RTE_VERSION_NUM
#define RTE_VERSION_NUM(a,b,c,d) ((((a)*100 + (b))*100 + (c))*100 + (d))
#endif
#ifndef RTE_VER_YEAR
#define RTE_VER_YEAR 24
#define RTE_VER_MONTH 11
#endif
#define RTE_AT_LEAST(YY,MM) \
  ( RTE_VERSION_NUM(RTE_VER_YEAR, RTE_VER_MONTH, 0, 0) >= RTE_VERSION_NUM((YY),(MM),0,0) )

// ---- tiny helpers used below ---------------------------------------------
static inline const char* mempool_name(const rte_mempool* mp) {
#if RTE_AT_LEAST(24,11)
  return rte_mempool_get_name(mp);
#else
  return mp ? mp->name : "<null>";
#endif
}

static inline const char* ring_name(const rte_ring* r)   { return r ? rte_ring_get_name(r) : "<null>"; }
static inline unsigned     ring_size(const rte_ring* r)  { return r ? rte_ring_get_size(r)  : 0U; }

// ---------- crash handler ----------
static void crash_handler(int sig) {
  std::fprintf(stderr, "=== crash_handler: got signal %d (%s) ===\n", sig, strsignal(sig));
  std::_Exit(128 + sig);
}

static void install_handlers() {
  std::signal(SIGSEGV, crash_handler);
  std::signal(SIGABRT, crash_handler);
  std::signal(SIGBUS,  crash_handler);
}

// ---------- env helpers ----------
static void ensure_env_defaults() {
  // Max DPDK chatter for debugging (unless caller already set it).
  if (!std::getenv("RTE_LOG_LEVEL")) {
    setenv("RTE_LOG_LEVEL", "8", 0); // 8 = DEBUG
  }
}

// ---------- pretty printers ----------
static void dump_primary(const flexsdr::FlexSDRPrimary& app) {
  std::fprintf(stderr, "[primary] pools=%zu, tx_rings=%zu, rx_rings=%zu\n",
               app.pools().size(), app.tx_rings().size(), app.rx_rings().size());

  for (auto* mp : app.pools()) {
    std::fprintf(stderr, "  [pool] %s\n", mempool_name(mp));
  }

  for (auto* r : app.tx_rings()) {
    if (!r) continue;
    std::fprintf(stderr, "  [tx]   %s (size=%u)\n", ring_name(r), ring_size(r));
  }
  for (auto* r : app.rx_rings()) {
    if (!r) continue;
     std::fprintf(stderr, "  [rx]   %s (size=%u)\n", ring_name(r), ring_size(r));
  }
}

static void dump_secondary(const flexsdr::FlexSDRSecondary& app) {
  std::fprintf(stderr, "[secondary] pools=%zu, tx_rings=%zu, rx_rings=%zu\n",
               app.pools().size(), app.tx_rings().size(), app.rx_rings().size());

  for (auto* mp : app.pools()) {
     std::fprintf(stderr, "  [pool] %s\n", mempool_name(mp));
  }
  for (auto* r : app.tx_rings()) {
    std::fprintf(stderr, "  [tx]   %s (size=%u)\n", ring_name(r), ring_size(r));
  }
  for (auto* r : app.rx_rings()) {
    std::fprintf(stderr, "  [rx]   %s (size=%u)\n", ring_name(r), ring_size(r));
  }
}

// ---------- barrier via named pipe ----------
struct Barrier {
  std::string path;
};

static Barrier make_barrier() {
  Barrier b;
  b.path = "/tmp/flexsdr_test_barrier_" + std::to_string(getpid());
  ::unlink(b.path.c_str());
  if (mkfifo(b.path.c_str(), 0600) != 0) {
    std::perror("mkfifo");
    std::fprintf(stderr, "Failed to create barrier FIFO at %s\n", b.path.c_str());
    std::_Exit(2);
  }
  return b;
}

static void barrier_signal(const Barrier& b) {
  int fd = ::open(b.path.c_str(), O_WRONLY);
  if (fd < 0) {
    std::perror("open(barrier wr)");
    std::_Exit(3);
  }
  const char ch = 'X';
  (void)::write(fd, &ch, 1);
  ::close(fd);
}

static void barrier_wait(const Barrier& b) {
  int fd = ::open(b.path.c_str(), O_RDONLY);
  if (fd < 0) {
    std::perror("open(barrier rd)");
    std::_Exit(4);
  }
  char ch = 0;
  (void)::read(fd, &ch, 1);
  ::close(fd);
}

// ---------- roles ----------
static int run_primary_proc(const std::string& cfg_path) {
  install_handlers();
  ensure_env_defaults();

  std::fprintf(stderr, "[primary] starting with %s\n", cfg_path.c_str());
  flexsdr::FlexSDRPrimary app(cfg_path);
  std::fprintf(stderr, "[primary] constructed FlexSDRPrimary\n");

  int rc = app.init_resources();
  if (rc) {
    std::fprintf(stderr, "[primary] init_resources failed rc=%d\n", rc);
    return 11;
  }
  dump_primary(app);
  return 0;
}

static int run_secondary_proc(const std::string& cfg_path) {
  install_handlers();
  ensure_env_defaults();

  std::fprintf(stderr, "[secondary] starting with %s\n", cfg_path.c_str());
  flexsdr::FlexSDRSecondary app(cfg_path);
  std::fprintf(stderr, "[secondary] constructed FlexSDRSecondary\n");

  int rc = app.init_resources();
  if (rc) {
    std::fprintf(stderr, "[secondary] init_resources failed rc=%d\n", rc);
    return 21;
  }
  dump_secondary(app);
  return 0;
}

// ---------- main orchestrator ----------
int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <config.yaml>\n", argv[0]);
    return 1;
  }
  const std::string cfg_path = argv[1];

  install_handlers();
  ensure_env_defaults();

  // Create a named-pipe barrier visible to both children.
  Barrier bar = make_barrier();

  // Fork primary
  pid_t primary_pid = fork();
  if (primary_pid == 0) {
    // Child: primary — init, then signal the barrier
    int rc = run_primary_proc(cfg_path);
    if (rc == 0) {
      std::fprintf(stderr, "[primary] resources ready; signalling secondary...\n");
      barrier_signal(bar);
    }
    std::_Exit(rc == 0 ? 0 : rc);
  } else if (primary_pid < 0) {
    std::perror("fork(primary)");
    ::unlink(bar.path.c_str());
    return 100;
  }

  // Fork secondary
  pid_t secondary_pid = fork();
  if (secondary_pid == 0) {
    std::fprintf(stderr, "[secondary] waiting for primary barrier...\n");
    barrier_wait(bar);
    int rc = run_secondary_proc(cfg_path);
    std::_Exit(rc == 0 ? 0 : rc);
  } else if (secondary_pid < 0) {
    std::perror("fork(secondary)");
    kill(primary_pid, SIGKILL);
    ::unlink(bar.path.c_str());
    return 101;
  }

  // Parent: wait and summarize
  int st1 = 0, st2 = 0;
  pid_t w1 = waitpid(primary_pid, &st1, 0);
  pid_t w2 = waitpid(secondary_pid, &st2, 0);
  (void)w1; (void)w2;

  ::unlink(bar.path.c_str());

  auto explain = [](int st) -> std::string {
    if (WIFEXITED(st)) {
      int ec = WEXITSTATUS(st);
      char buf[64]; std::snprintf(buf, sizeof(buf), "exit=%d", ec);
      return std::string(buf);
    }
    if (WIFSIGNALED(st)) {
      int sg = WTERMSIG(st);
      char buf[64]; std::snprintf(buf, sizeof(buf), "signal=%d (%s)", sg, strsignal(sg));
      return std::string(buf);
    }
    return "unknown";
  };

  std::fprintf(stderr, "[orchestrator] primary %s, secondary %s\n",
               explain(st1).c_str(), explain(st2).c_str());

  // Return non-zero if any child failed.
  int rc = 0;
  if (!WIFEXITED(st1) || WEXITSTATUS(st1) != 0) rc = -1;
  if (!WIFEXITED(st2) || WEXITSTATUS(st2) != 0) rc = -1;
  return rc == 0 ? 0 : 1;
}
