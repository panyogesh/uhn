// src/device/flexsdr_rx_streamer.cpp
#include "device/flexsdr_rx_streamer.hpp"

extern "C" {
#include <rte_ring.h>
#include <rte_mbuf.h>
}

#include <algorithm>
#include <cstring>
#include <chrono>
#include <cstdint>
#include <arpa/inet.h> // ntohl
#include <endian.h>

namespace flexsdr {

flexsdr_rx_streamer::flexsdr_rx_streamer(rte_ring* ring,
                                         std::size_t num_chans,
                                         double tick_rate_hz,
                                         std::size_t vrt_hdr_bytes,
                                         unsigned packets_per_chan,
                                         unsigned burst,
                                         std::size_t tsf_offset)
: ring_(ring)
, num_chans_(num_chans ? num_chans : 1)
, tick_rate_(tick_rate_hz > 0 ? tick_rate_hz : 30.72e6)
, vrt_hdr_bytes_(vrt_hdr_bytes ? vrt_hdr_bytes : 32)
, pkts_per_chan_(packets_per_chan ? packets_per_chan : 8)
, burst_(burst ? burst : 32)
, tsf_offset_(tsf_offset)
{}

size_t flexsdr_rx_streamer::get_num_channels() const { return num_chans_; }
// pick a conservative “hint”; UHD will pass nsamps_per_buff anyway
size_t flexsdr_rx_streamer::get_max_num_samps() const { return 1024; }

void flexsdr_rx_streamer::issue_stream_cmd(const uhd::stream_cmd_t& cmd) {
  switch (cmd.stream_mode) {
    case uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS:
    case uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE:
      running_ = true; break;
    case uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS:
      running_ = false; break;
    default: break;
  }
}

// Minimal helper: convert VRT TSF (if present as 64-bit after header) to UHD time_spec
static inline void tsf_to_time(double tick_rate, uint64_t tsf, uhd::time_spec_t& ts) {
  // Treat TSF as sample ticks (UHD’s common use); adjust if you encode different units.
  const double secs = double(tsf) / tick_rate;
  ts = uhd::time_spec_t(secs);
}

size_t flexsdr_rx_streamer::recv(const buffs_type& buffs,
                                 const size_t nsamps_per_buff,
                                 uhd::rx_metadata_t& md,
                                 const double timeout,
                                 const bool one_packet)
{
  md = uhd::rx_metadata_t{};
  if (!running_ || !ring_ || nsamps_per_buff == 0 || buffs.size() < num_chans_) {
    md.error_code = uhd::rx_metadata_t::ERROR_CODE_TIMEOUT;
    return 0;
  }

  constexpr unsigned MAX_BURST = 64;
  void* objs[MAX_BURST];
  const unsigned want = std::min<unsigned>(burst_, MAX_BURST);

  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::duration<double>(timeout);

  unsigned got = 0;
  while (got == 0) {
    got = rte_ring_dequeue_burst(ring_, objs, want, nullptr);
    if (got == 0) {
      if (std::chrono::steady_clock::now() >= deadline) {
        md.error_code = uhd::rx_metadata_t::ERROR_CODE_TIMEOUT;
        return 0;
      }
      rte_pause();
    }
  }

  // Per-channel write counters (critical fix)
  std::vector<size_t> wr(num_chans_, 0);
  bool   eob_seen = false;
  bool   sob_seen = false;

  for (unsigned i = 0; i < got; ++i) {
    rte_mbuf* m = static_cast<rte_mbuf*>(objs[i]);

    const uint8_t* base = (const uint8_t*)rte_pktmbuf_mtod(m, const void*);
    const size_t   pkt_bytes = size_t(rte_pktmbuf_data_len(m));

    // Channel group accounting (8 packets per channel)
    const uint64_t pkt_idx = pkt_counter_++;
    const std::size_t chan = std::size_t((pkt_idx / pkts_per_chan_) % num_chans_);
    const bool group_start = (pkt_idx % pkts_per_chan_) == 0;
    const bool group_end   = (pkt_idx % pkts_per_chan_) == (pkts_per_chan_ - 1);

    // Stamp time at group start (TSF always present by your spec)
    if (group_start && (tsf_offset_ + 8 <= pkt_bytes)) {
      uint64_t tsf_be;
      std::memcpy(&tsf_be, base + tsf_offset_, sizeof(tsf_be));
      const uint64_t tsf = be64toh(tsf_be);
      md.has_time_spec = true;
      tsf_to_time(tick_rate_, tsf, md.time_spec);
      sob_seen = true;
    }

    // Payload after VRT header
    const size_t payload_off = vrt_hdr_bytes_;
    if (payload_off >= pkt_bytes || chan >= buffs.size()) {
      rte_pktmbuf_free(m);
      continue;
    }
    const uint8_t* iq = base + payload_off;
    const size_t   payload_bytes = pkt_bytes - payload_off;
    const size_t   samps_in_pkt  = payload_bytes / (sizeof(int16_t) * 2);
    if (samps_in_pkt == 0) { rte_pktmbuf_free(m); continue; }

    // Channel-local remaining room for this call
    size_t remain_ch = nsamps_per_buff - wr[chan];
    size_t take      = std::min(remain_ch, samps_in_pkt);
    if (take > 0) {
      auto* dst = static_cast<uint8_t*>(buffs[chan]);
      const size_t bytes = take * sizeof(int16_t) * 2;
      std::memcpy(dst + wr[chan] * sizeof(int16_t) * 2, iq, bytes);
      wr[chan] += take;
    }

    eob_seen |= group_end;
    rte_pktmbuf_free(m);

    if (one_packet) break;

    // Early exit if all channels filled their quota
    bool all_full = true;
    for (size_t c = 0; c < num_chans_; ++c) {
      if (wr[c] < nsamps_per_buff) { all_full = false; break; }
    }
    if (all_full) break;
  }

  // UHD expects “samples per channel”; report the minimum across channels.
  size_t got_all = SIZE_MAX;
  for (size_t c = 0; c < num_chans_; ++c) got_all = std::min(got_all, wr[c]);
  if (got_all == SIZE_MAX || got_all == 0) {
    md.error_code = uhd::rx_metadata_t::ERROR_CODE_TIMEOUT;
    return 0;
  }

  md.error_code     = uhd::rx_metadata_t::ERROR_CODE_NONE;
  md.start_of_burst = sob_seen;
  md.end_of_burst   = eob_seen;
  return got_all;
}


} // namespace flexsdr