// === tx_worker.cpp (surgical edits) ===
#include "workers/tx_worker.hpp"
#include <cstring>
#include <arpa/inet.h>
#include <endian.h>

namespace flexsdr {

static inline void write_vrt(uint8_t* p, std::size_t hdr_bytes,
                             uint32_t stream_id, uint64_t tsf_ticks,
                             std::size_t payload_bytes,
                             std::size_t tsf_offset = 24)  // <— make configurable
{
    const uint32_t words = (uint32_t)((hdr_bytes + payload_bytes + 3) / 4);
    const uint32_t be_words = htonl(words);
    const uint32_t be_sid   = htonl(stream_id);
    std::memcpy(p + 0, &be_words, 4);
    std::memcpy(p + 4, &be_sid, 4);
    if (hdr_bytes >= tsf_offset + 8) {
        uint64_t be_tsf = htobe64(tsf_ticks);
        std::memcpy(p + tsf_offset, &be_tsf, 8);
    }
}

TxWorkerHandle start_tx_worker(const TxWorkerConfig& cfg,
                               std::function<bool(TxItem&)> pop_tx_item)
{
    TxWorkerHandle h;
    if (!cfg.ring || !cfg.pool || !cfg.run_flag) {
        std::fprintf(stderr, "[tx_worker] invalid config\n");
        return h;
    }

    h.thread = std::thread([cfg, pop_tx_item](){
        constexpr unsigned BURST = 32;
        void* objs[BURST];
        unsigned bi = 0;

        while (cfg.run_flag->load(std::memory_order_relaxed)) {
            TxItem item;
            if (!pop_tx_item(item)) { rte_pause(); continue; }

            const std::size_t payload_bytes = (std::size_t)item.nsamps * 4;
            const std::size_t need_bytes    = cfg.vrt_hdr_bytes + payload_bytes;

            rte_mbuf* m = rte_pktmbuf_alloc(cfg.pool);
            if (!m) { rte_pause(); continue; }
            if (rte_pktmbuf_tailroom(m) < need_bytes) { rte_pktmbuf_free(m); continue; }

            uint8_t* hdr = (uint8_t*)rte_pktmbuf_append(m, cfg.vrt_hdr_bytes);
            if (!hdr) { rte_pktmbuf_free(m); continue; }

            write_vrt(hdr, cfg.vrt_hdr_bytes, item.stream_id, item.tsf_ticks, payload_bytes /*, 24*/);

            int16_t* dst = (int16_t*)rte_pktmbuf_append(m, payload_bytes);
            if (!dst) { rte_pktmbuf_free(m); continue; }
            std::memcpy(dst, item.iq, payload_bytes);

            objs[bi++] = m;
            if (bi == BURST) {
                unsigned enq = rte_ring_enqueue_burst(cfg.ring, objs, BURST, nullptr);
                for (unsigned i = enq; i < BURST; ++i) rte_pktmbuf_free((rte_mbuf*)objs[i]);
                bi = 0;
            }
        }
        if (bi) {
            unsigned enq = rte_ring_enqueue_burst(cfg.ring, objs, bi, nullptr);
            for (unsigned i = enq; i < bi; ++i) rte_pktmbuf_free((rte_mbuf*)objs[i]);
        }
    });

    return h;
}

} // namespace flexsdr
