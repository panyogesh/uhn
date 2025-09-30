#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>
#include <string>

extern "C" {
#include <rte_config.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>
}

#include "conf/config_params.hpp"
#include "transport/dpdk_common.hpp"
#include "transport/flexsdr_primary.hpp"
#include "transport/flexsdr_secondary.hpp"
#include "transport/eal_bootstrap.hpp"

using namespace flexsdr;

static void hex_dump_first(const void* p, size_t n, size_t max = 32) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  size_t m = (n < max) ? n : max;
  for (size_t i = 0; i < m; ++i) std::printf("%02X ", b[i]);
  if (n > m) std::printf("... (+%zu bytes)", n - m);
  std::printf("\n");
}

static int enqueue_one(rte_ring* r, void* obj, const char* tag) {
  int rc = rte_ring_enqueue(r, obj);
  if (rc != 0) {
    std::fprintf(stderr, "[%s] enqueue failed on ring '%s': %s\n",
                 tag, r->name, rte_strerror(-rc));
  }
  return rc;
}

static int dequeue_one(rte_ring* r, void** obj, const char* tag) {
  int rc = rte_ring_dequeue(r, obj);
  if (rc != 0) {
    std::fprintf(stderr, "[%s] dequeue failed on ring '%s': %s\n",
                 tag, r->name, rte_strerror(-rc));
  }
  return rc;
}

static rte_mbuf* alloc_packet(rte_mempool* pool, size_t payload_len, uint8_t seed) {
  rte_mbuf* m = rte_pktmbuf_alloc(pool);
  if (!m) { std::fprintf(stderr, "pktmbuf_alloc failed: %s\n", rte_strerror(rte_errno)); return nullptr; }
  if (payload_len > rte_pktmbuf_tailroom(m)) {
    std::fprintf(stderr, "not enough data room (need %zu, have %u)\n", payload_len, rte_pktmbuf_tailroom(m));
    rte_pktmbuf_free(m);
    return nullptr;
  }
  uint8_t* p = static_cast<uint8_t*>(rte_pktmbuf_mtod(m, void*));
  for (size_t i = 0; i < payload_len; ++i) p[i] = static_cast<uint8_t>(seed + i);
  m->data_len = static_cast<uint16_t>(payload_len);
  m->pkt_len  = static_cast<uint32_t>(payload_len);
  return m;
}

int main(int argc, char** argv) {
  const std::string cfg_path = (argc > 1) ? argv[1] : "conf/configurations.yaml";

  FullConfig cfg;
  try {
    cfg = load_full_config(cfg_path);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "YAML load failed: %s\n", e.what());
    return 1;
  }

  if (init_eal_from_config(cfg.eal, /*secondary=*/false, {}) < 0) return 2;

  Handles primary_handles;
  if (create_primary_objects(cfg.primary, cfg.defaults, primary_handles,
                             SOCKET_ID_ANY, RING_F_SP_ENQ | RING_F_SC_DEQ) != 0) return 3;

  Handles ue_h, gnb_h;
  std::string ue_err, gnb_err;
  if (attach_secondary_ue(cfg.ue_app, cfg.defaults, ue_h, ue_err) != 0) {
    std::fprintf(stderr, "UE attach failed: %s\n", ue_err.c_str()); return 4;
  }
  if (attach_secondary_gnb(cfg.gnb_app, cfg.defaults, gnb_h, gnb_err) != 0) {
    std::fprintf(stderr, "gNB attach failed: %s\n", gnb_err.c_str()); return 5;
  }

  std::printf("=== Infra OK ===\n");
  std::printf("UE rings: %zu, pools: %zu | gNB rings: %zu, pools: %zu\n",
              ue_h.rings.size(), ue_h.pools.size(),
              gnb_h.rings.size(), gnb_h.pools.size());

  // Use plural 'rings'
  if (cfg.ue_app.tx_stream.rings.empty() || cfg.gnb_app.tx_stream.rings.empty()) {
    std::fprintf(stderr, "YAML must define tx_stream.rings for both ue-app and gnb-app\n");
    return 6;
  }
  const std::string ue_tx0_name  = cfg.ue_app.tx_stream.rings.front();
  const std::string gnb_tx0_name = cfg.gnb_app.tx_stream.rings.front();
  const std::string ue_in_name   = cfg.ue_app.inbound_ring;
  const std::string gnb_in_name  = cfg.gnb_app.inbound_ring;

  rte_ring* ue_tx0  = ue_h.rings.at(ue_tx0_name);
  rte_ring* gnb_tx0 = gnb_h.rings.at(gnb_tx0_name);
  rte_ring* ue_in   = ue_h.rings.at(ue_in_name);
  rte_ring* gnb_in  = gnb_h.rings.at(gnb_in_name);

  rte_mempool* ue_tx_pool  = ue_h.pools.at(cfg.ue_app.tx_pool);
  rte_mempool* gnb_tx_pool = gnb_h.pools.at(cfg.gnb_app.tx_pool);

  uint32_t ue_room  = pool_data_room(ue_tx_pool);
  uint32_t gnb_room = pool_data_room(gnb_tx_pool);
  uint32_t safe_len = 128;
  if (ue_room && ue_room < safe_len)  safe_len = ue_room / 2;
  if (gnb_room && gnb_room < safe_len) safe_len = gnb_room / 2;

  // UE -> gNB
  {
    rte_mbuf* m = alloc_packet(ue_tx_pool, safe_len, 0x11);
    if (!m) return 7;
    std::printf("[UE] enqueue on %s: len=%u; first bytes: ", ue_tx0->name, m->pkt_len);
    hex_dump_first(rte_pktmbuf_mtod(m, void*), m->pkt_len);
    if (enqueue_one(ue_tx0, m, "UE")) { rte_pktmbuf_free(m); return 8; }

    void* got = nullptr;
    if (dequeue_one(ue_tx0, &got, "CP-UE->gNB")) return 9;
    if (enqueue_one(gnb_in, got, "CP-UE->gNB")) { rte_pktmbuf_free((rte_mbuf*)got); return 10; }

    void* rx = nullptr;
    if (dequeue_one(gnb_in, &rx, "gNB-RX")) return 11;
    rte_mbuf* mg = static_cast<rte_mbuf*>(rx);
    std::printf("[gNB] received: len=%u; first bytes: ", mg->pkt_len);
    hex_dump_first(rte_pktmbuf_mtod(mg, void*), mg->pkt_len);
    rte_pktmbuf_free(mg);
  }

  // gNB -> UE
  {
    rte_mbuf* m = alloc_packet(gnb_tx_pool, safe_len, 0xA5);
    if (!m) return 12;
    std::printf("[gNB] enqueue on %s: len=%u; first bytes: ", gnb_tx0->name, m->pkt_len);
    hex_dump_first(rte_pktmbuf_mtod(m, void*), m->pkt_len);
    if (enqueue_one(gnb_tx0, m, "gNB")) { rte_pktmbuf_free(m); return 13; }

    void* got = nullptr;
    if (dequeue_one(gnb_tx0, &got, "CP-gNB->UE")) return 14;
    if (enqueue_one(ue_in, got, "CP-gNB->UE")) { rte_pktmbuf_free((rte_mbuf*)got); return 15; }

    void* rx = nullptr;
    if (dequeue_one(ue_in, &rx, "UE-RX")) return 16;
    rte_mbuf* mu = static_cast<rte_mbuf*>(rx);
    std::printf("[UE] received: len=%u; first bytes: ", mu->pkt_len);
    hex_dump_first(rte_pktmbuf_mtod(mu, void*), mu->pkt_len);
    rte_pktmbuf_free(mu);
  }

  std::printf("=== DPDK infra test PASSED ===\n");
  return 0;
}
