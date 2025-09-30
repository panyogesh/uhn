// test_dpdk_primary_only.cpp  (drop-in)
//
// Primary process that builds ONE multi-channel VRT+SC16 packet per round:
//   payload = [ch0 spp][ch1 spp][ch2 spp][ch3 spp], spp=8 samples each
// Stamps TSF per packet, enqueues to UE_in, and mirrors to a tap ring
// for "on-the-wire" printing (VRT header + IQ).
//
// Geometry summary (defaults in this file):
//   n_channels = 4, spp = 8 (per channel per packet)
//   header bytes = 32, TSF at offset 24..31 (BE64)
//   stream_id: composite 0x1F00 (you will move per-channel SIDs to flexsdr_factory)
//   TSF increments by spp ticks (channels are parallel), given tick_rate==sample_rate.

#include <csignal>
#include <atomic>
#include <chrono>
#include <thread>
#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <cmath>
#include <cstring>
#include <array>
#include <inttypes.h>
#include <endian.h>   // htobe64, be64toh
#include <arpa/inet.h>

extern "C" {
#include <rte_config.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_pause.h>
}

#include "conf/config_params.hpp"
#include "transport/eal_bootstrap.hpp"
#include "transport/dpdk_common.hpp"
#include "transport/flexsdr_primary.hpp"

// Use UHD's official VRT packet info type
#include <uhd/transport/vrt_if_packet.hpp>

using namespace flexsdr;
namespace vrt = uhd::transport::vrt;

static std::atomic<bool> g_run{true};

static void on_sigint(int){ g_run = false; }

static inline unsigned ring_count_safe(rte_ring* r){
  return r ? rte_ring_count(r) : 0U;
}
static inline unsigned ring_free_safe(rte_ring* r){
  return r ? rte_ring_free_count(r) : 0U;
}

static void print_banner(){
  std::cout << "=== DPDK Primary Infra ===\n";
  std::cout << "Ctrl+C to exit; leave this running while secondaries attach.\n";
}

//------------------------------------------------------------------------------
// Fill UHD's vrt::if_packet_info_t from our on-wire packet layout (VRT header)
//------------------------------------------------------------------------------
static bool fill_if_packet_info_from_mbuf(
    const rte_mbuf* m,
    std::size_t hdr_bytes,
    std::size_t tsf_offset,
    vrt::if_packet_info_t& out)
{
  const uint8_t* base = (const uint8_t*)rte_pktmbuf_mtod(m, const void*);
  const std::size_t pkt_bytes = rte_pktmbuf_data_len(m);
  if (pkt_bytes < hdr_bytes || pkt_bytes < 8) return false;

  // First 8 bytes: [0..3]=total_words32 (BE), [4..7]=stream_id (BE)
  uint32_t size_words_be = 0, stream_id_be = 0;
  std::memcpy(&size_words_be, base + 0, 4);
  std::memcpy(&stream_id_be, base + 4, 4);
  const uint32_t size_words = ntohl(size_words_be);
  const uint32_t sid_host   = ntohl(stream_id_be);

  // Payload
  const std::size_t payload_bytes   = (pkt_bytes > hdr_bytes) ? (pkt_bytes - hdr_bytes) : 0;
  const std::size_t payload_words32 = payload_bytes / 4;

  // TSF (if present)
  bool     has_tsf   = false;
  uint64_t tsf_ticks = 0;
  if (tsf_offset + 8 <= pkt_bytes) {
    uint64_t tsf_be = 0;
    std::memcpy(&tsf_be, base + tsf_offset, 8);
    tsf_ticks = be64toh(tsf_be);
    has_tsf = true;
  }

  out = vrt::if_packet_info_t{};
  out.link_type           = vrt::if_packet_info_t::LINK_TYPE_CHDR;
  out.packet_type         = vrt::if_packet_info_t::PACKET_TYPE_DATA;
  out.has_sid             = true;
  out.sid                 = sid_host;
  out.has_tsi             = false;
  out.has_tsf             = has_tsf;
  out.tsf                 = tsf_ticks;
  out.has_cid             = false;
  out.has_tlr             = false;
  out.sob                 = false;
  out.eob                 = false;
  out.error               = false;
  out.fc_ack              = false;
  out.packet_count        = 0;

  out.num_header_words32  = hdr_bytes / 4;
  out.num_payload_words32 = payload_words32;     // SC16: 1 word32 == 1 complex sample
  out.num_payload_bytes   = payload_bytes;
  out.num_packet_words32  = size_words;
  return true;
}

//------------------------------------------------------------------------------
// Dump a MULTI-CHANNEL packet: payload is [ch0 spp][ch1 spp][ch2 spp][ch3 spp].
//------------------------------------------------------------------------------
static void dump_packet_wire_multi_ch(
    const rte_mbuf* m,
    const vrt::if_packet_info_t& info,
    double tick_rate_hz,
    unsigned spp,           // samples per channel per packet
    unsigned nchan)         // 4
{
  const uint8_t* base = (const uint8_t*)rte_pktmbuf_mtod(m, const void*);
  const std::size_t pkt_bytes = rte_pktmbuf_data_len(m);

  std::cout << "[WIRE] MULTI-CH pkt_bytes=" << pkt_bytes
            << " hdr=" << (info.num_header_words32 * 4) << "\n";

  std::cout << "  VRT.size_words=" << info.num_packet_words32
            << " (bytes≈" << (info.num_packet_words32 * 4) << ")"
            << "  stream_id=0x" << std::hex << info.sid << std::dec << "\n";

  if (info.has_tsf) {
    const double tsf_secs = (double)info.tsf / tick_rate_hz;
    std::cout << "  TSF=0x" << std::hex << info.tsf << std::dec
              << " (" << (double)info.tsf << " ticks, "
              << std::fixed << std::setprecision(9) << tsf_secs << " s)\n";
  } else {
    std::cout << "  TSF=(absent)\n";
  }

  const std::size_t payload_bytes   = info.num_payload_bytes;
  const std::size_t payload_words32 = info.num_payload_words32; // SC16: 1 word32 per complex sample
  std::cout << "  payload_bytes=" << payload_bytes
            << " payload_words32=" << payload_words32
            << " total_samples(SC16)=" << payload_words32 << "\n";

  const std::size_t expected_samples = (std::size_t)nchan * spp;
  if (payload_words32 < expected_samples) {
    std::cout << "  [WARN] payload shorter than expected (" << payload_words32
              << " < " << expected_samples << ")\n";
  }

  const uint8_t* iq = base + info.num_header_words32 * 4;

  for (unsigned ch = 0; ch < nchan; ++ch) {
    std::cout << "  CH" << ch << " IQ (" << spp << "/" << spp << "):\n    ";
    for (unsigned i = 0; i < spp; ++i) {
      const std::size_t idx = (std::size_t)ch * spp + i; // block-of-channels layout
      int16_t I, Q;
      std::memcpy(&I, iq + 4*idx + 0, 2);
      std::memcpy(&Q, iq + 4*idx + 2, 2);
      std::cout << "(" << I << "," << Q << ") ";
    }
    std::cout << "\n";
  }
}

//------------------------------------------------------------------------------
// Monitor: read clones from tap, parse into if_packet_info_t, print first N
//------------------------------------------------------------------------------
static void monitor_ring_thread(
    rte_ring* tap,
    std::size_t hdr_bytes,
    std::size_t tsf_offset,
    unsigned spp,
    double tick_rate_hz,
    std::atomic<bool>& run_flag)
{
  constexpr unsigned BURST = 32;
  void* objs[BURST];

  // Print first 2 packets overall (each contains all 4 channels)
  unsigned printed = 0;
  const unsigned max_print = 2;

  while (run_flag.load(std::memory_order_relaxed)) {
    unsigned n = rte_ring_dequeue_burst(tap, objs, BURST, nullptr);
    if (n == 0) { rte_pause(); continue; }

    for (unsigned i = 0; i < n; ++i) {
      rte_mbuf* m = (rte_mbuf*)objs[i];

      vrt::if_packet_info_t info;
      if (fill_if_packet_info_from_mbuf(m, hdr_bytes, tsf_offset, info)) {
        if (printed < max_print) {
          dump_packet_wire_multi_ch(m, info, tick_rate_hz, /*spp*/spp, /*nchan*/4);
          ++printed;
        }
      }

      rte_pktmbuf_free(m); // free the clone from tap
    }
  }

  // Drain on exit
  unsigned n;
  while ((n = rte_ring_dequeue_burst(tap, objs, BURST, nullptr)) != 0) {
    for (unsigned i = 0; i < n; ++i) rte_pktmbuf_free((rte_mbuf*)objs[i]);
  }
}

int main(int argc, char** argv){
  std::signal(SIGINT, on_sigint);
  std::signal(SIGTERM, on_sigint);

  const std::string cfg_path = (argc > 1) ? argv[1] : "conf/configurations.yaml";

  // 1) Load YAML
  FullConfig cfg;
  try {
    cfg = load_full_config(cfg_path);
  } catch (const std::exception& e) {
    std::cerr << "[YAML] load failed: " << e.what() << "\n";
    return 1;
  }

  // 2) Init EAL as PRIMARY
  if (init_eal_from_config(cfg.eal, /*secondary=*/false, {}) < 0) {
    std::cerr << "[EAL] init (primary) failed: " << rte_strerror(rte_errno) << "\n";
    return 2;
  }

  // 3) Create primary-owned global rings/pools per YAML
  Handles primary_h;
  int rc = create_primary_objects(
      cfg.primary, cfg.defaults, primary_h,
      SOCKET_ID_ANY,
      RING_F_SP_ENQ | RING_F_SC_DEQ);
  if (rc != 0) {
    std::cerr << "[PRIMARY] create_primary_objects failed (rc=" << rc << ")\n";
    return 3;
  }

  // 4) Show what we created
  std::cout << "[DPDK] Primary created " << primary_h.pools.size()
            << " pools and " << primary_h.rings.size() << " rings.\n";
  print_banner();
  std::cout << "[PRIMARY] Rings created: " << primary_h.rings.size()
            << " | Pools created: " << primary_h.pools.size() << "\n";

  auto get_ring = [&](const std::string& n) -> rte_ring* {
    if (n.empty()) return nullptr;
    auto it = primary_h.rings.find(n);
    return (it == primary_h.rings.end()) ? nullptr : it->second;
  };

  rte_ring* ue_in   = get_ring(cfg.ue_app.inbound_ring);
  rte_ring* gnb_in  = get_ring(cfg.gnb_app.inbound_ring);
  rte_ring* ue_tx0  = cfg.ue_app.tx_stream.rings.empty() ? nullptr
                    : get_ring(cfg.ue_app.tx_stream.rings.front());
  rte_ring* gnb_tx0 = cfg.gnb_app.tx_stream.rings.empty() ? nullptr
                    : get_ring(cfg.gnb_app.tx_stream.rings.front());

  std::cout << "  UE inbound ring:   " << (ue_in ? ue_in->name : "(none)") << "\n";
  std::cout << "  gNB inbound ring:  " << (gnb_in ? gnb_in->name : "(none)") << "\n";
  std::cout << "  UE TX ring[0]:     " << (ue_tx0 ? ue_tx0->name : "(none)") << "\n";
  std::cout << "  gNB TX ring[0]:    " << (gnb_tx0 ? gnb_tx0->name : "(none)") << "\n";

  // 5) Create a tap ring to mirror traffic for on-wire prints
  rte_ring* ue_tap = nullptr;
  {
    const unsigned tap_size = 256;
    ue_tap = rte_ring_create("ue_inbound_tap", tap_size, SOCKET_ID_ANY,
                             RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (!ue_tap) {
      std::cout << "[PRIMARY] Warning: could not create ue_inbound_tap ("
                << rte_strerror(rte_errno) << ")\n";
    } else {
      std::cout << "[PRIMARY] Created tap ring: ue_inbound_tap (size=" << tap_size << ")\n";
    }
  }

  // 6) Start monitor thread (geometry + rates must match producer)
  std::thread monitor;
  if (ue_tap) {
    const std::size_t hdr_bytes  = 32;
    const std::size_t tsf_offset = 24;
    const unsigned    spp        = 8;        // per-channel per packet
    const double      tick_rate  = 30.72e6;  // device tick rate (mcr)
    monitor = std::thread(monitor_ring_thread, ue_tap, hdr_bytes, tsf_offset, spp, tick_rate, std::ref(g_run));
  }

  // 7) Producer: ONE multi-channel packet per round (4ch block, spp=8), TSF per packet
  std::thread producer;
  if (ue_in && !cfg.ue_app.rx_pool.empty()) {
    rte_mempool* ue_pool = nullptr;
    auto itp = primary_h.pools.find(cfg.ue_app.rx_pool);
    if (itp != primary_h.pools.end()) ue_pool = itp->second;

    if (ue_pool) {
      producer = std::thread([&, ue_tap](){   // capture ue_tap for cloning
        const unsigned n_channels    = 4;
        const unsigned spp           = 8;               // per-channel per packet
        const size_t   hdr_bytes     = 32;              // VRT header bytes
        const size_t   tsf_offset    = 24;              // TSF at bytes 24..31 (BE)
        const size_t   bytes_sc16    = (size_t)n_channels * spp * sizeof(int16_t) * 2; // 128B
        const size_t   pkt_bytes     = hdr_bytes + bytes_sc16;
        const uint32_t words_total   = (uint32_t)((pkt_bytes + 3) / 4);

        // Rates for TSF math
        const double sample_rate     = 30.72e6; // stream sample rate
        const double tick_rate       = 30.72e6; // device tick rate (mcr)
        const double ticks_per_sample= tick_rate / sample_rate;

        auto write_vrt = [&](uint8_t* p, uint32_t stream_id_be){
          std::memset(p, 0, hdr_bytes);
          uint32_t be_words = htonl(words_total);
          std::memcpy(p + 0, &be_words, 4);        // total size in words (debug)
          std::memcpy(p + 4, &stream_id_be, 4);    // composite stream_id
          // bytes 8..23 zero; TSF filled at [24..31]
        };

        // Keep phase per channel (so next packet continues smoothly)
        std::array<double,4> phase{};
        std::array<double,4> dph{};
        for (unsigned ch = 0; ch < 4; ++ch) {
          const double f_cyc_per_sample = 0.01 * (ch + 1); // arbitrary tones
          dph[ch] = 2.0 * M_PI * f_cyc_per_sample;
        }

        // Fill [ch0 spp][ch1 spp][ch2 spp][ch3 spp]
        auto fill_sc16_tone_blocked_4ch =
          [&](int16_t* iq /*len = 2 * nchan * spp*/, unsigned spp_local, double amp){
            for (unsigned ch = 0; ch < n_channels; ++ch) {
              double ph = phase[ch], step = dph[ch];
              for (unsigned i = 0; i < spp_local; ++i) {
                const std::size_t idx = (std::size_t)ch * spp_local + i;
                iq[2*idx]   = (int16_t)std::lrint(amp * std::cos(ph));
                iq[2*idx+1] = (int16_t)std::lrint(amp * std::sin(ph));
                ph += step; if (ph > 2.0 * M_PI) ph -= 2.0 * M_PI;
              }
              phase[ch] = ph;
            }
        };

        // TSF ticks; advance every packet by round(spp * ticks_per_sample); channels are parallel
        uint64_t tsf_ticks = 0;
        const uint64_t delta_ticks =
            (uint64_t)std::llround((double)spp * ticks_per_sample);

        // Composite stream id for “multi-channel packet”
        const uint32_t stream_id_be = htonl(0x1F00);

        std::vector<void*> burst(64, nullptr);
        unsigned burst_i = 0;

        while (g_run.load(std::memory_order_relaxed)) {
          // Build ONE packet that holds ch0..ch3, 8 samples each
          rte_mbuf* m = rte_pktmbuf_alloc(ue_pool);
          if (!m) { rte_pause(); continue; }

          uint8_t* hdr = (uint8_t*)rte_pktmbuf_append(m, hdr_bytes);
          if (!hdr) { rte_pktmbuf_free(m); continue; }
          write_vrt(hdr, stream_id_be);

          // TSF for this packet
          uint64_t tsf_be = htobe64(tsf_ticks);
          std::memcpy(hdr + tsf_offset, &tsf_be, 8);

          int16_t* iq = (int16_t*)rte_pktmbuf_append(m, bytes_sc16);
          if (!iq) { rte_pktmbuf_free(m); continue; }
          fill_sc16_tone_blocked_4ch(iq, spp, /*amp*/20000.0);

          // Mirror to tap for “on-wire” print
          if (ue_tap) {
            rte_mbuf* mc = rte_pktmbuf_clone(m, ue_pool);
            if (mc) {
              if (rte_ring_enqueue(ue_tap, mc) != 0) rte_pktmbuf_free(mc);
            }
          }

          // Enqueue to real ring
          burst[burst_i++] = m;

          // Advance TSF by spp (channels are parallel)
          tsf_ticks += delta_ticks;

          if (burst_i == burst.size()) {
            unsigned enq = rte_ring_enqueue_burst(ue_in, burst.data(), burst_i, nullptr);
            for (unsigned j = enq; j < burst_i; ++j) rte_pktmbuf_free((rte_mbuf*)burst[j]);
            burst_i = 0;
          }

          // Optional small pause for readability:
          // rte_pause();
        }

        // flush tail
        if (burst_i) {
          unsigned enq = rte_ring_enqueue_burst(ue_in, burst.data(), burst_i, nullptr);
          for (unsigned j = enq; j < burst_i; ++j) rte_pktmbuf_free((rte_mbuf*)burst[j]);
        }
      });

      std::cout << "[PRIMARY] UE_in producer running (one multi-ch packet/round, spp=8)\n";
    } else {
      std::cout << "[PRIMARY] Warning: UE pool '" << cfg.ue_app.rx_pool << "' not found; producer disabled\n";
    }
  } else {
    std::cout << "[PRIMARY] UE_in ring not present; producer disabled\n";
  }

  // 8) Periodic ring stats
  using clock = std::chrono::steady_clock;
  auto next = clock::now();
  while (g_run.load(std::memory_order_relaxed)) {
    next += std::chrono::seconds(1);
    std::this_thread::sleep_until(next);

    auto pr = [&](const char* label, rte_ring* r) {
      if (!r) return;
      unsigned used = ring_count_safe(r);
      unsigned free = ring_free_safe(r);
      unsigned cap  = used + free;
      std::cout << std::left << std::setw(12) << label
                << " ring=" << r->name
                << " used=" << used
                << " free=" << free
                << " cap="  << cap
                << "\n";
    };

    std::cout << "--- ring stats ---\n";
    pr("UE_in",   ue_in);
    pr("UE_tx0",  ue_tx0);
    pr("gNB_in",  gnb_in);
    pr("gNB_tx0", gnb_tx0);
    std::cout.flush();
  }

  std::cout << "\n[PRIMARY] shutting down.\n";
  return 0;
}
