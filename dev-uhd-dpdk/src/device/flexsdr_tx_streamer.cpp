// === flexsdr_tx_streamer.cpp (drop-in) ===
// keeps your includes
#include "device/flexsdr_tx_streamer.hpp"

extern "C" {
#include <rte_ring.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
}

#include <cstring>
#include <stdexcept>
#include <arpa/inet.h>
#include <endian.h>

namespace flexsdr {

namespace {
inline void write_vrt_minimal(uint8_t* p,
                              std::size_t hdr_bytes,
                              uint32_t stream_id,    // host order
                              uint64_t tsf_ticks,    // host order
                              std::size_t payload_bytes)
{
    // Minimal 32B header: [0]=words, [1]=stream_id, [6:7]=tsf (at 24..31)
    // You can extend this later, but keep size stable.
    std::memset(p, 0, hdr_bytes);
    const uint32_t words = static_cast<uint32_t>((hdr_bytes + payload_bytes + 3) / 4);
    const uint32_t be_words = htonl(words);
    const uint32_t be_sid   = htonl(stream_id);
    const uint64_t be_tsf   = htobe64(tsf_ticks);
    std::memcpy(p + 0,  &be_words, 4);
    std::memcpy(p + 4,  &be_sid,   4);
    if (hdr_bytes >= 32) {
        std::memcpy(p + 24, &be_tsf, 8);
    }
}
} // anon

// --- ctor stays the same, but add burst staging members ---
flexsdr_tx_streamer::flexsdr_tx_streamer(
    rte_ring* tx_ring,
    rte_mempool* pool,
    std::size_t spp,
    unsigned    burst,
    bool        allow_partial
)
: tx_ring_(tx_ring),
  pool_(pool),
  spp_(spp),
  burst_(burst ? burst : 32),
  allow_partial_(allow_partial)
{
    if (!tx_ring_ || !pool_) {
        throw std::invalid_argument("flexsdr_tx_streamer: null tx_ring or mempool");
    }
    num_chans_ = 1; // TODO: wire from args if/when multi-channel TX is real

    staged_.resize(burst_, nullptr);
    staged_i_ = 0;
}

flexsdr_tx_streamer::~flexsdr_tx_streamer() {
    flush_();
}

size_t flexsdr_tx_streamer::get_num_channels() const { return num_chans_; }

// For now, cap max to spp_ to keep semantics simple for callers
size_t flexsdr_tx_streamer::get_max_num_samps() const { return spp_; }

// Helper: flush staged mbufs to ring
void flexsdr_tx_streamer::flush_() {
    if (staged_i_ == 0) return;
    unsigned enq = rte_ring_enqueue_burst(tx_ring_, staged_.data(), staged_i_, nullptr);
    for (unsigned i = enq; i < staged_i_; ++i) {
        rte_pktmbuf_free(static_cast<rte_mbuf*>(staged_[i]));
    }
    staged_i_ = 0;
}

size_t flexsdr_tx_streamer::send(
    const buffs_type &buffs,
    const size_t nsamps_per_buff,
    const uhd::tx_metadata_t &metadata,
    const double /*timeout*/
) {
    // Basic validation/sc16 assumption (I16,Q16 → 4 bytes per complex)
    if (!tx_ring_ || !pool_) return 0;
    if (buffs.size() == 0 || nsamps_per_buff == 0) return 0;

    // We only support 1 channel for now. Enforce.
    if (buffs.size() != num_chans_) return 0;

    // Enforce SPP if not allowing partial
    if (!allow_partial_ && nsamps_per_buff != spp_) return 0;

    const std::size_t hdr_bytes = 32; // keep in sync with RX/worker expectations
    const std::size_t payload_bytes = nsamps_per_buff * 4; // sc16
    const std::size_t need_bytes = hdr_bytes + payload_bytes;

    // Allocate mbuf
    rte_mbuf* m = rte_pktmbuf_alloc(pool_);
    if (!m) return 0;

    // Ensure pool element is large enough
    if (rte_pktmbuf_tailroom(m) < need_bytes) {
        rte_pktmbuf_free(m);
        return 0;
    }

    uint8_t* hdr = static_cast<uint8_t*>(rte_pktmbuf_append(m, hdr_bytes));
    if (!hdr) { rte_pktmbuf_free(m); return 0; }

    // TSF: If metadata has a time_spec, convert to ticks; otherwise 0
    // NOTE: "tick_rate" should come from the device/mboard clock, but we don't have it here.
    //       Leave as 0 or wire a setter for tx tick rate.
    uint64_t tsf_ticks = 0;
    if (metadata.has_time_spec) {
        // If you know tick rate elsewhere, store it in the class and multiply:
        // tsf_ticks = static_cast<uint64_t>(metadata.time_spec.to_ticks(tx_tick_rate_));
        // For now, carry the fractional seconds scaled by 1 (noop).
        tsf_ticks = static_cast<uint64_t>(metadata.time_spec.get_real_secs());
    }

    // Pick a stream_id; if you have per-channel IDs, extend this
    const uint32_t stream_id = 0x2000; // arbitrary for TX; adjust as needed

    write_vrt_minimal(hdr, hdr_bytes, stream_id, tsf_ticks, payload_bytes);

    // Copy SC16 payload (Planar not yet supported here; we assume interleaved channel 0)
    const void* src0 = buffs[0]; // uhd::ref_vector element is a raw pointer
    int16_t* dst = static_cast<int16_t*>(rte_pktmbuf_append(m, payload_bytes));
    if (!dst) { rte_pktmbuf_free(m); return 0; }
    std::memcpy(dst, src0, payload_bytes);

    // Stage and flush when full
    staged_[staged_i_++] = m;
    if (staged_i_ == staged_.size()) flush_();

    return nsamps_per_buff;
}

} // namespace flexsdr
