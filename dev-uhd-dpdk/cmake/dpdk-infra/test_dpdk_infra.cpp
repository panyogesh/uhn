#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>
#include <algorithm>

#include <rte_mbuf.h>
#include <rte_ring.h>
#include <rte_mempool.h>

#include "conf/config_params.hpp"
#include "transport/eal_bootstrap.hpp"
#include "transport/flexsdr_primary.hpp"
#include "transport/flexsdr_secondary.hpp"

// ---------------- helpers ----------------
static void die(const char* msg){ std::fprintf(stderr, "%s\n", msg); std::exit(1); }
static void print_list(const char* tag, const std::vector<std::string>& xs){
  std::printf("%s (%zu):\n", tag, xs.size());
  for (const auto& s : xs) std::printf("  - %s\n", s.c_str());
}

static bool pick_first_names(const flexsdr::conf::PrimaryConfig& cfg,
                             std::string& tx_name, std::string& rx_name){
  auto tx = cfg.materialized_tx_rings();
  auto rx = cfg.materialized_rx_rings();
  if (tx.empty() || rx.empty()) return false;
  tx_name = tx.front().name;
  rx_name = rx.front().name;
  return true;
}

static void log_effective(const flexsdr::conf::PrimaryConfig& cfg){
  auto tx = cfg.materialized_tx_rings();
  auto rx = cfg.materialized_rx_rings();
  std::printf("[cfg] role=%s, ring_size=%u\n",
              flexsdr::conf::PrimaryConfig::to_string(cfg.defaults.role).c_str(),
              cfg.defaults.ring_size);
  std::printf("[cfg] tx rings:\n");
  for (auto& r : tx) std::printf("  - %s (size=%u)\n", r.name.c_str(), r.size);
  std::printf("[cfg] rx rings:\n");
  for (auto& r : rx) std::printf("  - %s (size=%u)\n", r.name.c_str(), r.size);
}

// ---------------- primary child ----------------
static int run_primary(const std::string& yaml, const std::vector<std::string>& pass_flags) {
  using namespace flexsdr;

  conf::PrimaryConfig cfg = conf::PrimaryConfig::load(yaml);
  // default to primary-ue if the YAML is set to a consumer role
  if (cfg.defaults.role != conf::Role::primary_ue && cfg.defaults.role != conf::Role::primary_gnb)
    cfg.defaults.role = conf::Role::primary_ue;

  EalBootstrap eal(cfg, "test_dpdk_infra_primary");
  std::vector<std::string> flags = pass_flags;
  // ensure we are a primary unless caller forced something
  bool has_proc = false; for (auto& f: flags) if (f.find("--proc-type")!=std::string::npos) {has_proc=true;break;}
  if (!has_proc) flags.emplace_back("--proc-type=primary");
  eal.build_args(flags);
  if (eal.init() < 0) return 10;

  FlexSDRPrimary app(yaml);
  app.load_config();
  log_effective(cfg);

  if (int rc = app.init_resources(); rc < 0) {
    std::fprintf(stderr, "[primary] init_resources failed rc=%d\n", rc);
    return 11;
  }
  print_list("[primary] pools", app.created_or_found_pools());
  print_list("[primary] rings", app.created_or_found_rings());

  // first ring names (TX -> to secondary, RX <- from secondary)
  std::string tx_name, rx_name;
  if (!pick_first_names(cfg, tx_name, rx_name)) { std::fprintf(stderr, "[primary] need >=1 tx and >=1 rx ring\n"); return 12; }
  rte_ring* rx_ring = rte_ring_lookup(rx_name.c_str());
  rte_ring* tx_ring = rte_ring_lookup(tx_name.c_str());
  if (!rx_ring || !tx_ring) { std::fprintf(stderr, "[primary] ring lookup failed (rx=%p tx=%p)\n",(void*)rx_ring,(void*)tx_ring); return 13; }

  // choose a pool (by name list, then lookup)
  rte_mempool* pool = nullptr;
  if (!app.created_or_found_pools().empty()) {
    const std::string& first_pool_name = app.created_or_found_pools().front();
    pool = rte_mempool_lookup(first_pool_name.c_str());
  }
  if (!pool) pool = rte_mempool_lookup("ue_inbound_pool"); // optional fallback

  if (!pool) { std::fprintf(stderr, "[primary] no mempool available\n"); return 14; }

  // Phase A: receive N from secondary
  const unsigned N = 64;
  unsigned rx_cnt = 0;
  while (rx_cnt < N) {
    void* objs[32];
    unsigned nb = rte_ring_dequeue_burst(rx_ring, objs, 32, nullptr);
    for (unsigned i=0;i<nb;++i) rte_pktmbuf_free((rte_mbuf*)objs[i]); // free after inspection
    rx_cnt += nb;
  }
  std::printf("[primary] received %u packets from secondary on %s\n", rx_cnt, rx_name.c_str());

  // Phase B: send N to secondary
  unsigned tx_cnt = 0;
  while (tx_cnt < N) {
    rte_mbuf* bufs[32];
    unsigned n = std::min<unsigned>(32, N - tx_cnt);
    for (unsigned i = 0; i < n; ++i) {
      bufs[i] = rte_pktmbuf_alloc(pool);
      if (!bufs[i]) { n = i; break; }
      uint64_t* p = (uint64_t*)rte_pktmbuf_append(bufs[i], sizeof(uint64_t));
      if (p) *p = tx_cnt + i;
    }
    if (n == 0) continue;
    unsigned enq = rte_ring_enqueue_burst(tx_ring, (void**)bufs, n, nullptr);
    for (unsigned i = enq; i < n; ++i) rte_pktmbuf_free(bufs[i]);
    tx_cnt += enq;
  }
  std::printf("[primary] sent %u packets to secondary on %s\n", tx_cnt, tx_name.c_str());
  return 0;
}

// ---------------- secondary child ----------------
static int run_secondary(const std::string& yaml, const std::vector<std::string>& pass_flags) {
  using namespace flexsdr;

  conf::PrimaryConfig cfg = conf::PrimaryConfig::load(yaml);
  if (cfg.defaults.role != conf::Role::ue && cfg.defaults.role != conf::Role::gnb)
    cfg.defaults.role = conf::Role::ue;

  EalBootstrap eal(cfg, "test_dpdk_infra_secondary");
  std::vector<std::string> flags = pass_flags;
  bool has_proc = false; for (auto& f: flags) if (f.find("--proc-type")!=std::string::npos) {has_proc=true;break;}
  if (!has_proc) flags.emplace_back("--proc-type=secondary");
  eal.build_args(flags);
  if (eal.init() < 0) return 20;

  FlexSDRSecondary app(yaml);
  app.load_config();
  log_effective(cfg);

  if (int rc = app.init_resources(); rc < 0) {
    std::fprintf(stderr, "[secondary] init_resources failed rc=%d\n", rc);
    return 21;
  }
  print_list("[secondary] pools", app.found_pools());
  print_list("[secondary] rings", app.found_rings());

  std::string tx_name, rx_name;
  if (!pick_first_names(cfg, tx_name, rx_name)) { std::fprintf(stderr, "[secondary] need >=1 tx and >=1 rx ring\n"); return 22; }
  rte_ring* rx_ring = rte_ring_lookup(rx_name.c_str());
  rte_ring* tx_ring = rte_ring_lookup(tx_name.c_str());
  if (!rx_ring || !tx_ring) { std::fprintf(stderr, "[secondary] ring lookup failed (rx=%p tx=%p)\n",(void*)rx_ring,(void*)tx_ring); return 23; }

  // pool (secondary: use found pool names -> lookup)
  rte_mempool* pool = nullptr;
  if (!app.found_pools().empty()) {
    const std::string& first_pool_name = app.found_pools().front();
    pool = rte_mempool_lookup(first_pool_name.c_str());
  }
  if (!pool) pool = rte_mempool_lookup("ue_inbound_pool");

  if (!pool) { std::fprintf(stderr, "[secondary] no mempool available\n"); return 24; }

  // Phase A: send N to primary
  const unsigned N = 64;
  unsigned tx_cnt = 0;
  while (tx_cnt < N) {
    rte_mbuf* bufs[32];
    unsigned n = std::min<unsigned>(32, N - tx_cnt);
    for (unsigned i = 0; i < n; ++i) {
      bufs[i] = rte_pktmbuf_alloc(pool);
      if (!bufs[i]) { n = i; break; }
      uint64_t* p = (uint64_t*)rte_pktmbuf_append(bufs[i], sizeof(uint64_t));
      if (p) *p = tx_cnt + i;
    }
    if (n == 0) continue;
    unsigned enq = rte_ring_enqueue_burst(tx_ring, (void**)bufs, n, nullptr);
    for (unsigned i = enq; i < n; ++i) rte_pktmbuf_free(bufs[i]);
    tx_cnt += enq;
  }
  std::printf("[secondary] sent %u packets to primary on %s\n", tx_cnt, tx_name.c_str());

  // Phase B: receive N back from primary
  unsigned rx_cnt = 0;
  while (rx_cnt < N) {
    void* objs[32];
    unsigned nb = rte_ring_dequeue_burst(rx_ring, objs, 32, nullptr);
    for (unsigned i=0;i<nb;++i) rte_pktmbuf_free((rte_mbuf*)objs[i]);
    rx_cnt += nb;
  }
  std::printf("[secondary] received %u packets from primary on %s\n", rx_cnt, rx_name.c_str());
  return 0;
}

// ---------------- main orchestrator ----------------
int main(int argc, char** argv){
  if (argc < 2) {
    std::fprintf(stderr,
      "usage:\n"
      "  %s <yaml>                     # orchestrate: spawn primary & secondary, do roundtrip\n"
      "  %s <yaml> --child primary     # internal\n"
      "  %s <yaml> --child secondary   # internal\n",
      argv[0], argv[0], argv[0]);
    return 2;
  }

  const std::string yaml = argv[1];

  // child mode?
  bool is_child = false;
  bool is_primary_child = false;
  std::vector<std::string> extra_flags;
  for (int i = 2; i < argc; ++i) {
    if (std::string(argv[i]) == "--child" && i + 1 < argc) {
      is_child = true;
      is_primary_child = (std::string(argv[i+1]) == "primary");
      ++i;
    } else {
      extra_flags.emplace_back(argv[i]); // pass-through EAL flags if any
    }
  }

  if (is_child) {
    return is_primary_child ? run_primary(yaml, extra_flags)
                            : run_secondary(yaml, extra_flags);
  }

  // Orchestrate
  pid_t p = fork(); if (p < 0) die("fork(primary) failed");
  if (p == 0) {
    std::vector<const char*> args = { argv[0], yaml.c_str(), "--child", "primary", nullptr };
    execv(argv[0], (char* const*)args.data());
    std::perror("execv primary"); std::_Exit(127);
  }

  usleep(400 * 1000); // let primary create SHM objs

  pid_t s = fork(); if (s < 0) die("fork(secondary) failed");
  if (s == 0) {
    std::vector<const char*> args = { argv[0], yaml.c_str(), "--child", "secondary", nullptr };
    execv(argv[0], (char* const*)args.data());
    std::perror("execv secondary"); std::_Exit(127);
  }

  int st1=0, st2=0;
  waitpid(p, &st1, 0);
  waitpid(s, &st2, 0);
  std::printf("[orchestrator] primary exit=%d, secondary exit=%d\n",
              WIFEXITED(st1) ? WEXITSTATUS(st1) : -1,
              WIFEXITED(st2) ? WEXITSTATUS(st2) : -1);

  return (WIFEXITED(st1) && WEXITSTATUS(st1) == 0 &&
          WIFEXITED(st2) && WEXITSTATUS(st2) == 0) ? 0 : 1;
}
