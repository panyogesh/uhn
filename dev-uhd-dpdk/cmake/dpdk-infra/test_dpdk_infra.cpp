#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <execinfo.h>

#include <rte_config.h>
#include <rte_errno.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>
#include <rte_cycles.h>

#include "transport/flexsdr_primary.hpp"
#include "transport/flexsdr_secondary.hpp"

// =====================================================================================
// Crash handler (prints backtrace even from forked children)
// =====================================================================================
static void crash_handler(int sig) {
  void* bt[64];
  int n = backtrace(bt, 64);
  fprintf(stderr, "\n=== crash_handler: got signal %d (%s) ===\n", sig, strsignal(sig));
  backtrace_symbols_fd(bt, n, STDERR_FILENO);
  _exit(128 + sig);
}

// =====================================================================================
// Small helpers
// =====================================================================================
static const char* env_or(const char* key, const char* defval) {
  const char* v = std::getenv(key);
  return (v && *v) ? v : defval;
}

static void explain_status(const char* who, int st) {
  if (WIFEXITED(st)) {
    std::fprintf(stderr, "[orchestrator] %s exit=%d\n", who, WEXITSTATUS(st));
  } else if (WIFSIGNALED(st)) {
    std::fprintf(stderr, "[orchestrator] %s signal=%d (%s)\n",
                 who, WTERMSIG(st), strsignal(WTERMSIG(st)));
  } else {
    std::fprintf(stderr, "[orchestrator] %s unknown status=0x%x\n", who, st);
  }
}

static rte_ring* must_ring_lookup(const char* name) {
  rte_ring* r = rte_ring_lookup(name);
  if (!r) {
    std::fprintf(stderr, "[ring] lookup failed: %s rte_errno=%d (%s)\n",
                 name, rte_errno, rte_strerror(rte_errno));
  } else {
    std::fprintf(stderr, "[ring] ok: %s (size=%u cap=%u)\n",
                 name, rte_ring_get_size(r), rte_ring_get_capacity(r));
  }
  return r;
}

static rte_mempool* any_pool_lookup(const std::vector<std::string>& candidates) {
  for (auto& n : candidates) {
    rte_mempool* mp = rte_mempool_lookup(n.c_str());
    if (mp) { std::fprintf(stderr, "[pool] ok: %s\n", n.c_str()); return mp; }
  }
  std::fprintf(stderr, "[pool] lookup failed; tried:\n");
  for (auto& n : candidates) std::fprintf(stderr, "  - %s\n", n.c_str());
  return nullptr;
}

// =====================================================================================
static constexpr uint32_t kMagic      = 0xF1F2F3F4u;
static constexpr uint32_t kNumFrames  = 64;   // frames per run
static constexpr uint32_t kIQPerFrame = 32;   // IQ pairs per frame

struct __attribute__((packed)) IQHdr {
  uint32_t magic;
  uint32_t idx;
  uint32_t count; // IQ pairs
};
struct __attribute__((packed)) IQSample { int16_t i; int16_t q; };

// =====================================================================================
// Secondary: send frames UE->Primary, receive echoes Primary->UE
// =====================================================================================
static int secondary_run_io(const char* tx_ring_name,
                            const char* rx_ring_name,
                            const std::vector<std::string>& pool_names) {
  rte_ring* tx = must_ring_lookup(tx_ring_name);
  rte_ring* rx = must_ring_lookup(rx_ring_name);
  if (!tx || !rx) return -10;

  rte_mempool* mp = any_pool_lookup(pool_names);
  if (!mp) return -11;

  uint32_t sent = 0;
  for (uint32_t idx = 0; idx < kNumFrames; ++idx) {
    rte_mbuf* m = rte_pktmbuf_alloc(mp);
    if (!m) { std::fprintf(stderr, "[UE] mbuf alloc failed idx=%u\n", idx); break; }

    const uint32_t need = sizeof(IQHdr) + kIQPerFrame * sizeof(IQSample);
    if (rte_pktmbuf_tailroom(m) < need) {
      std::fprintf(stderr, "[UE] tailroom too small need=%u have=%u\n",
                   need, rte_pktmbuf_tailroom(m));
      rte_pktmbuf_free(m);
      break;
    }

    IQHdr* hdr = reinterpret_cast<IQHdr*>(rte_pktmbuf_mtod(m, void*));
    hdr->magic = kMagic;
    hdr->idx   = idx;
    hdr->count = kIQPerFrame;
    IQSample* s = reinterpret_cast<IQSample*>(hdr + 1);
    for (uint32_t i = 0; i < kIQPerFrame; ++i) {
      s[i].i = static_cast<int16_t>(idx + i);
      s[i].q = static_cast<int16_t>(-(int16_t)(idx + i));
    }
    m->data_len = need;
    m->pkt_len  = need;

    const int rc = rte_ring_enqueue(tx, m);
    if (rc != 0) {
      std::fprintf(stderr, "[UE] enqueue %s failed idx=%u rc=%d (%s)\n",
                   tx_ring_name, idx, rc, rte_strerror(-rc));
      rte_pktmbuf_free(m);
      break;
    }
    ++sent;
  }
  std::fprintf(stderr, "[UE] sent %u/%u frames to %s\n", sent, kNumFrames, tx_ring_name);

  uint32_t got = 0;
  const uint64_t end = rte_get_timer_cycles() + 2 * rte_get_timer_hz(); // ~2s
  while (got < sent && rte_get_timer_cycles() < end) {
    rte_mbuf* m = nullptr;
    if (rte_ring_dequeue(rx, reinterpret_cast<void**>(&m)) == 0) {
      if (!m) continue;
      if (m->pkt_len >= sizeof(IQHdr)) {
        auto* hdr = reinterpret_cast<IQHdr*>(rte_pktmbuf_mtod(m, void*));
        if (hdr->magic != kMagic) {
          std::fprintf(stderr, "[UE] bad magic on echo idx=%u\n", hdr->idx);
        }
      }
      ++got;
      rte_pktmbuf_free(m);
    } else {
      rte_pause();
    }
  }
  std::fprintf(stderr, "[UE] received %u/%u echo frames on %s\n", got, sent, rx_ring_name);
  return (got == sent) ? 0 : -12;
}

// =====================================================================================
// Primary: echo frames back
// =====================================================================================
static int primary_run_io(const char* rx_from_ue,   // we dequeue here (ue_tx_ch1)
                          const char* tx_to_ue) {   // we enqueue here (ue_inbound_ring)
  rte_ring* in  = must_ring_lookup(rx_from_ue);
  rte_ring* out = must_ring_lookup(tx_to_ue);
  if (!in || !out) return -20;

  uint32_t got = 0, echoed = 0;
  const uint64_t end = rte_get_timer_cycles() + 2 * rte_get_timer_hz(); // ~2s
  while (rte_get_timer_cycles() < end) {
    rte_mbuf* m = nullptr;
    if (rte_ring_dequeue(in, reinterpret_cast<void**>(&m)) == 0) {
      ++got;
      bool ok = (m->pkt_len >= sizeof(IQHdr));
      if (ok) {
        auto* hdr = reinterpret_cast<IQHdr*>(rte_pktmbuf_mtod(m, void*));
        ok = (hdr->magic == kMagic);
      }
      if (ok && rte_ring_enqueue(out, m) == 0) {
        ++echoed;
      } else {
        rte_pktmbuf_free(m);
      }
    } else {
      rte_pause();
    }
  }
  std::fprintf(stderr, "[Primary] dequeued=%u echoed=%u\n", got, echoed);
  return (echoed > 0) ? 0 : -21;
}

// =====================================================================================
// Barrier via pipe: parent sends 'G' when primary ready
// =====================================================================================
static void barrier_parent_signal(int fd) {
  const char c = 'G';
  (void)!write(fd, &c, 1);
  close(fd);
}
static void barrier_child_wait(int fd) {
  char c = 0;
  (void)!read(fd, &c, 1);
  close(fd);
}

// =====================================================================================
// Entrypoints per process (classes own EAL init)
// =====================================================================================
static void set_dpdk_debug_env() {
  // Enable loud logs even if user forgets to export
  setenv("RTE_LOG_LEVEL", "8", 0);
  setenv("RTE_LOG_LEVEL_EAL", "8", 0);
  setenv("RTE_LOG_LEVEL_RING", "8", 0);
  setenv("RTE_LOG_LEVEL_MEMPOOL", "8", 0);
}

static int run_primary_proc(const std::string& yaml, int barrier_fd) {
  using namespace flexsdr;

  set_dpdk_debug_env();
  std::fprintf(stderr, "[primary] starting with %s\n", yaml.c_str());
  FlexSDRPrimary app(yaml);
  std::fprintf(stderr, "[primary] constructed FlexSDRPrimary\n");

  if (int rc = app.init_resources(); rc) {
    std::fprintf(stderr, "[primary] init_resources failed rc=%d\n", rc);
    return 11;
  }
  std::fprintf(stderr, "[primary] resources ready; signalling secondary...\n");
  barrier_parent_signal(barrier_fd);

  const char* rx_from_ue = env_or("FLEXSDR_TX_RING", "ue_tx_ch1");
  const char* tx_to_ue   = env_or("FLEXSDR_RX_RING", "ue_inbound_ring");
  const int rc = primary_run_io(rx_from_ue, tx_to_ue);

  ::usleep(100 * 1000);
  return rc ? 12 : 0;
}

static int run_secondary_proc(const std::string& yaml, int barrier_fd) {
  using namespace flexsdr;

  set_dpdk_debug_env();
  std::fprintf(stderr, "[secondary] waiting for primary barrier...\n");
  barrier_child_wait(barrier_fd);
  std::fprintf(stderr, "[secondary] starting with %s\n", yaml.c_str());
  FlexSDRSecondary app(yaml);
  std::fprintf(stderr, "[secondary] constructed FlexSDRSecondary\n");

  if (int rc = app.init_resources(); rc) {
    std::fprintf(stderr, "[secondary] init_resources failed rc=%d\n", rc);
    return 21;
  }

  const char* tx_to_pri   = env_or("FLEXSDR_TX_RING", "ue_tx_ch1");
  const char* rx_from_pri = env_or("FLEXSDR_RX_RING", "ue_inbound_ring");
  const char* pool_pref   = env_or("FLEXSDR_POOL", "");

  std::vector<std::string> pools;
  if (pool_pref && *pool_pref) pools.emplace_back(pool_pref);
  pools.emplace_back("ue_outbound_pool");
  pools.emplace_back("ue_inbound_pool");
  pools.emplace_back("outbound_pool");
  pools.emplace_back("inbound_pool");

  const int rc = secondary_run_io(tx_to_pri, rx_from_pri, pools);
  return rc ? 22 : 0;
}

// =====================================================================================
// Main: supports optional --only=primary / --only=secondary for isolation
// =====================================================================================
int main(int argc, char** argv) {
  signal(SIGSEGV, crash_handler);
  signal(SIGABRT, crash_handler);
  signal(SIGBUS,  crash_handler);
  signal(SIGILL,  crash_handler);

  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <config.yaml> [--only=primary|--only=secondary]\n", argv[0]);
    return 2;
  }
  const std::string cfg_path = argv[1];
  bool only_primary = false, only_secondary = false;
  for (int i = 2; i < argc; ++i) {
    if (strcmp(argv[i], "--only=primary") == 0)   only_primary   = true;
    if (strcmp(argv[i], "--only=secondary") == 0) only_secondary = true;
  }

  if (only_primary && only_secondary) {
    std::fprintf(stderr, "Choose one: --only=primary OR --only=secondary\n");
    return 2;
  }

  if (only_primary) {
    // single-process primary (useful to see it doesn’t crash alone)
    return run_primary_proc(cfg_path, /*barrier_fd (unused in this mode)*/ -1);
  }
  if (only_secondary) {
    // single-process secondary (will fail lookups, but shouldn’t segfault)
    return run_secondary_proc(cfg_path, /*barrier_fd (unused)*/ -1);
  }

  // Dual-process mode with pipe barrier
  int pipefd[2];
  if (pipe(pipefd) != 0) { std::perror("pipe"); return 3; }

  pid_t p1 = ::fork();
  if (p1 < 0) { std::perror("fork primary"); return 100; }
  if (p1 == 0) {
    close(pipefd[0]); // child primary uses write end
    int rc = run_primary_proc(cfg_path, pipefd[1]);
    _exit(rc);
  }

  pid_t p2 = ::fork();
  if (p2 < 0) { std::perror("fork secondary"); return 101; }
  if (p2 == 0) {
    close(pipefd[1]); // child secondary uses read end
    int rc = run_secondary_proc(cfg_path, pipefd[0]);
    _exit(rc);
  }

  close(pipefd[0]);
  close(pipefd[1]);

  int st1 = 0, st2 = 0;
  ::waitpid(p1, &st1, 0);
  ::waitpid(p2, &st2, 0);

  explain_status("primary", st1);
  explain_status("secondary", st2);

  const int rc1 = WIFEXITED(st1) ? WEXITSTATUS(st1) : 1;
  const int rc2 = WIFEXITED(st2) ? WEXITSTATUS(st2) : 1;
  return rc1 + rc2;
}
