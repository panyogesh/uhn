#pragma once
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>
#include <string>
#include <cstring>

extern "C" {
#include <rte_ring.h>
#include <rte_mbuf.h>
#include <rte_pause.h>
}

#include "workers/channel_fifo.hpp"

namespace flexsdr {

// Packet extracted from the wire (one DPDK mbuf → one RxPacket)
struct RxPacket {
    uint32_t              stream_id = 0;     // informational
    uint64_t              tsf_ticks = 0;     // if present
    bool                  have_tsf  = false; // TSF timestamp present
    bool                  sob       = false; // start of burst
    bool                  eob       = true;  // end of burst
    uint32_t              chan      = 0;     // channel index
    uint32_t              nsamps    = 0;     // SC16 samples in this packet
    std::vector<int16_t>  iq;                // interleaved I,Q (len=2*nsamps)
};

// Framing of how primary feeds samples
enum class RxFraming : uint8_t {
    Planar = 0,       // 8 packets of ch0, then 8 of ch1, etc. (pkt position defines channel)
    Interleaved = 1   // placeholder: packets contain multiple channels interleaved
};


// Configuration for the DPDK→FIFO demux worker
struct RxWorkerConfig {
    rte_ring*                  ring           = nullptr; // source ring (SPSC dequeues)
    std::atomic<bool>*         run_flag       = nullptr; // shared stop flag
    std::size_t                vrt_hdr_bytes  = 32;      // bytes to skip at start
    std::size_t                tsf_offset     = 24;      // where 64-bit TSF lives (if present)
    bool                       tsf_present    = true;    // whether TSF is emitted by primary
    unsigned                   num_channels   = 4;       // RX channel count
    unsigned                   pkts_per_chan  = 8;       // group size in Planar mode
    RxFraming                 mode           = RxFraming::Planar;
    double                     tick_rate      = 30.72e6; // for diagnostics if needed

    // Output FIFOs (one per channel). Must be pre-sized to num_channels and non-null.
    std::vector<std::shared_ptr<SpscQueue<RxPacket>>>* fifos = nullptr;
};

// Handle to the running worker thread
struct RxWorkerHandle {
    std::thread thread;
    std::atomic<bool>* run_flag = nullptr;
    void stop_join(std::atomic<bool>& flag) {
        flag.store(false, std::memory_order_release);
        if (thread.joinable()) thread.join();
    }
};

// Start the DPDK→FIFO demux worker. Returns a handle with the running thread.
// Requirements:
//   - cfg.ring != nullptr
//   - cfg.run_flag != nullptr
//   - cfg.fifos.size() == cfg.num_channels and all non-null
RxWorkerHandle start_rx_worker(const RxWorkerConfig& cfg);

// Stop the DPDK→FIFO demux worker
void stop_rx_worker(RxWorkerHandle& h);

// ------------------ helpers used by implementation (kept in header for inlining) ----
static inline uint32_t load_u32_be(const uint8_t* p) {
    uint32_t v; std::memcpy(&v, p, 4);
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8)
         | ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}
static inline uint64_t load_u64_be(const uint8_t* p) {
    uint64_t v; std::memcpy(&v, p, 8);
    return ((uint64_t)load_u32_be(p) << 32) | (uint64_t)load_u32_be(p + 4);
}

// Parse one mbuf as VRT header + SC16 payload.
// - stream_id is treated as BE at bytes [4..7] (debug only).
// - TSF at tsf_offset (BE u64) if present.
// - IQ payload starts at hdr_bytes, must be multiple of 4 bytes.
// Returns true on success; fills out.
inline bool parse_vrt_sc16_packet(const rte_mbuf* m,
                                  std::size_t hdr_bytes,
                                  std::size_t tsf_offset,
                                  bool tsf_present,
                                  RxPacket& out)
{
    const std::size_t pkt_bytes = rte_pktmbuf_data_len(m);
    if (pkt_bytes < hdr_bytes) return false;

    const auto* base = static_cast<const uint8_t*>(rte_pktmbuf_mtod(m, const void*));

    // optional: stream id at +4
    out.stream_id = (pkt_bytes >= 8) ? load_u32_be(base + 4) : 0;

    if (tsf_present && (tsf_offset + 8 <= pkt_bytes)) {
        out.tsf_ticks = load_u64_be(base + tsf_offset);
        out.have_tsf  = true;
    } else {
        out.tsf_ticks = 0;
        out.have_tsf  = false;
    }

    const std::size_t payload_off = hdr_bytes;
    if (payload_off > pkt_bytes) return false;

    const std::size_t payload_bytes = pkt_bytes - payload_off;
    if ((payload_bytes & 0x3) != 0) return false; // not SC16-aligned

    const uint32_t nsamps = static_cast<uint32_t>(payload_bytes / 4);
    out.nsamps = nsamps;
    out.iq.resize(static_cast<std::size_t>(nsamps) * 2);

    const uint8_t* iq = base + payload_off;
    std::memcpy(out.iq.data(), iq, static_cast<std::size_t>(nsamps) * 4);
    out.eob = true;
    return true;
}

} // namespace flexsdr
