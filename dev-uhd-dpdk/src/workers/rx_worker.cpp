#include "workers/rx_worker.hpp"

extern "C" {
#include <rte_ring.h>
#include <rte_mbuf.h>
#include <rte_pause.h>
}

#include <cstring>
#include <arpa/inet.h>  // ntohl
#include <endian.h>     // be64toh
#include <atomic>
#include <cstdio>

namespace flexsdr {

// Copy the entire packet payload into a single contiguous SC16 buffer.
// Safe for chained mbufs.
static inline bool copy_payload_sc16(const rte_mbuf* m,
                                     std::size_t hdr_bytes,
                                     RxPacket& out)
{
    const uint32_t pkt_len = rte_pktmbuf_pkt_len(m);
    if (pkt_len < hdr_bytes) return false;

    const uint32_t payload_bytes = pkt_len - static_cast<uint32_t>(hdr_bytes);
    if ((payload_bytes & 0x3) != 0) return false; // SC16 alignment

    const uint32_t nsamps = payload_bytes / 4;
    if (nsamps == 0) return false;

    out.nsamps = nsamps;
    out.iq.resize(static_cast<size_t>(nsamps) * 2);

    // Walk segments
    uint32_t to_copy = payload_bytes;
    uint32_t copied  = 0;
    const uint32_t dst_bytes = payload_bytes;
    uint8_t* dst = reinterpret_cast<uint8_t*>(out.iq.data());

    // First segment: start at header offset
    const rte_mbuf* seg = m;
    uint32_t seg_off = 0;
    {
        // advance seg/seg_off to header_bytes
        uint32_t skip = static_cast<uint32_t>(hdr_bytes);
        while (seg && skip > 0) {
            const uint32_t seg_len = seg->data_len;
            if (skip < seg_len) { seg_off = skip; break; }
            skip -= seg_len;
            seg = seg->next;
        }
        if (!seg && skip > 0) return false; // header beyond packet
    }

    while (seg && to_copy) {
        const uint8_t* src = rte_pktmbuf_mtod_offset(seg, const uint8_t*, seg_off);
        const uint32_t seg_len = seg->data_len - seg_off;
        const uint32_t chunk = (seg_len < to_copy) ? seg_len : to_copy;
        std::memcpy(dst + copied, src, chunk);
        copied  += chunk;
        to_copy -= chunk;
        seg      = seg->next;
        seg_off  = 0;
    }
    return copied == dst_bytes;
}

static inline bool parse_vrt(const rte_mbuf* m,
                             std::size_t hdr_bytes,
                             std::size_t tsf_off,
                             bool tsf_present,
                             RxPacket& out)
{
    const uint32_t pkt_len = rte_pktmbuf_pkt_len(m);
    if (pkt_len < 8) {
        out.stream_id = 0;
    } else {
        // First 8 bytes assumed (words + stream_id), both BE (for debug/visibility)
        // We only keep stream_id here (device identity) — channel comes from position.
        uint8_t first8[8];
        // copy first 8 from the start of the packet; handle chained mbufs
        uint32_t need = 8, got = 0;
        const rte_mbuf* seg = m;
        while (seg && need) {
            const uint32_t chunk = std::min<uint32_t>(need, seg->data_len);
            std::memcpy(first8 + got, rte_pktmbuf_mtod_offset(seg, const uint8_t*, 0), chunk);
            got  += chunk;
            need -= chunk;
            seg   = seg->next;
        }
        if (got == 8) {
            out.stream_id = load_u32_be(first8 + 4);
        } else {
            out.stream_id = 0;
        }
    }

    if (tsf_present) {
        // TSF at fixed offset (BE 64) from start of packet
        uint64_t tsf_be = 0;
        uint32_t need = 8, got = 0;
        const uint32_t start = static_cast<uint32_t>(tsf_off);
        const rte_mbuf* seg = m;

        // advance to tsf_off
        uint32_t skip = start;
        while (seg && skip >= seg->data_len) {
            skip -= seg->data_len;
            seg = seg->next;
        }
        if (!seg && start) {
            out.have_tsf = false;
        } else {
            // copy 8 bytes across segments if needed
            while (seg && need) {
                const uint8_t* src = rte_pktmbuf_mtod_offset(seg, const uint8_t*, skip);
                const uint32_t seg_len = seg->data_len - skip;
                const uint32_t chunk = std::min<uint32_t>(need, seg_len);
                std::memcpy(reinterpret_cast<uint8_t*>(&tsf_be) + got, src, chunk);
                got  += chunk;
                need -= chunk;
                seg   = seg->next;
                skip  = 0;
            }
            if (got == 8) {
                out.tsf_ticks = be64toh(tsf_be);
                out.have_tsf  = true;
            } else {
                out.have_tsf  = false;
            }
        }
    } else {
        out.have_tsf = false;
        out.tsf_ticks = 0;
    }

    // Payload copy (SC16 interleaved)
    return copy_payload_sc16(m, hdr_bytes, out);
}

RxWorkerHandle start_rx_worker(const RxWorkerConfig& cfg)
{
    RxWorkerHandle h;
    h.run_flag = cfg.run_flag;

    if (!cfg.ring || !cfg.run_flag || cfg.num_channels == 0 || !cfg.fifos || cfg.fifos->size() != cfg.num_channels) {
        std::fprintf(stderr, "[rx_worker] invalid config (ring/run_flag/fifos)\n");
        return h;
    }

    auto drops   = std::make_shared<std::atomic<uint64_t>>(0);
    auto handled = std::make_shared<std::atomic<uint64_t>>(0);

    h.thread = std::thread([cfg, drops, handled]() {
        constexpr unsigned BURST = 64;
        void* objs[BURST];

        const unsigned N = cfg.pkts_per_chan ? cfg.pkts_per_chan : 8;
        uint64_t pkt_idx = 0;

        bool     block_tsf_valid = false;
        uint64_t block_tsf_ticks = 0;

        while (cfg.run_flag->load(std::memory_order_relaxed)) {
            const unsigned n = rte_ring_dequeue_burst(cfg.ring, objs, BURST, nullptr);
            if (n == 0) { rte_pause(); continue; }

            for (unsigned i = 0; i < n; ++i) {
                rte_mbuf* m = static_cast<rte_mbuf*>(objs[i]);

                RxPacket p{};
                if (!parse_vrt(m, cfg.vrt_hdr_bytes, cfg.tsf_offset, cfg.tsf_present, p)) {
                    rte_pktmbuf_free(m);
                    ++pkt_idx;
                    continue;
                }

                // Start of the whole 4ch block? (pkt 0 of ch0)
                const bool block_beg = (pkt_idx % (uint64_t)(cfg.num_channels * N)) == 0;
                const bool block_end = (pkt_idx % (uint64_t)(cfg.num_channels * N)) == (uint64_t)(cfg.num_channels * N - 1);

                if (block_beg) {
                    // Reset for the new block; if first packet carries TSF, capture it
                    block_tsf_valid = p.have_tsf;
                    block_tsf_ticks = p.have_tsf ? p.tsf_ticks : 0;
                } else {
                    // Propagate captured TSF to every packet in the block
                    if (block_tsf_valid) {
                        p.have_tsf  = true;
                        p.tsf_ticks = block_tsf_ticks;
                    }
                }

                if (cfg.mode == RxFraming::Planar) {
                    const std::size_t ch        = std::size_t((pkt_idx / N) % cfg.num_channels);
                    const bool        group_beg = (pkt_idx % N) == 0;
                    const bool        group_end = (pkt_idx % N) == (N - 1);

                    p.chan = ch;
                    p.sob  = group_beg;
                    p.eob  = group_end;

                    auto& q = *cfg.fifos->at(ch);
                    if (!q.push(std::move(p))) drops->fetch_add(1, std::memory_order_relaxed);
                    else                       handled->fetch_add(1, std::memory_order_relaxed);
                } else {
                    // INTERLEAVED placeholder
                    p.chan = 0;
                    auto& q0 = *cfg.fifos->at(0);
                    if (!q0.push(std::move(p))) drops->fetch_add(1, std::memory_order_relaxed);
                    else                       handled->fetch_add(1, std::memory_order_relaxed);
                }

                rte_pktmbuf_free(m);
                ++pkt_idx;

                if (block_end) {
                    // Next packet will be next block; reset happens on block_beg
                }
            }
        }

        std::fprintf(stderr, "[rx_worker] exit: handled=%" PRIu64 " drops=%" PRIu64 "\n",
                     handled->load(), drops->load());
    });

    return h;
}

void stop_rx_worker(RxWorkerHandle& h)
{
    if (h.thread.joinable()) h.thread.join();
}

} // namespace flexsdr
