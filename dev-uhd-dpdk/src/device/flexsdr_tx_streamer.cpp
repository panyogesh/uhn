
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

// Helper function to write VRT header
static inline void write_vrt_header(uint8_t* p, std::size_t hdr_bytes,
                                     uint32_t stream_id, uint64_t tsf_ticks,
                                     std::size_t payload_bytes,
                                     std::size_t tsf_offset = 24)
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
  burst_(burst),
  allow_partial_(allow_partial)
{
    if (!tx_ring_ || !pool_) {
        throw std::invalid_argument("flexsdr_tx_streamer: null tx_ring or mempool");
    }

    // If you’ll support multi-channel later, wire num_chans_ from args
    // TODO
    num_chans_ = 1;
}

size_t flexsdr_tx_streamer::get_num_channels() const {
    return num_chans_;
}


size_t flexsdr_tx_streamer::get_max_num_samps() const {
    // keep simple for now; you can compute from spp_ * burst_ if you wish
    return spp_;
}

size_t flexsdr_tx_streamer::send(
    const buffs_type &buffs,
    const size_t nsamps_per_buff,
    const uhd::tx_metadata_t &metadata,
    const double /*timeout*/
) {
    // Validate inputs
    if (!tx_ring_ || !pool_) {
        return 0;
    }
    if (buffs.size() == 0 || nsamps_per_buff == 0) {
        return 0;
    }
    
    // Check partial send policy
    if (!allow_partial_ && nsamps_per_buff != spp_) {
        return 0;
    }
    
    // SC16 format: 2 shorts per sample (I and Q), 2 bytes per short = 4 bytes per sample
    const std::size_t payload_bytes = nsamps_per_buff * 4;
    const std::size_t need_bytes = vrt_hdr_bytes_ + payload_bytes;
    
    // Allocate mbuf from pool
    rte_mbuf* m = rte_pktmbuf_alloc(pool_);
    if (!m) {
        // Allocation failed - pool may be empty
        return 0;
    }
    
    // Check if we have enough tailroom
    if (rte_pktmbuf_tailroom(m) < need_bytes) {
        rte_pktmbuf_free(m);
        return 0;
    }
    
    // Add VRT header if configured
    if (vrt_hdr_bytes_ > 0) {
        uint8_t* hdr = (uint8_t*)rte_pktmbuf_append(m, vrt_hdr_bytes_);
        if (!hdr) {
            rte_pktmbuf_free(m);
            return 0;
        }
        
        // Extract timestamp from metadata (if available and has_time_spec is set)
        uint64_t tsf_ticks = 0;
        if (metadata.has_time_spec) {
            // Convert time_spec to TSF ticks (assuming some tick rate)
            // This is a simplified conversion - adjust based on your system
            tsf_ticks = metadata.time_spec.to_ticks(1e9); // nanoseconds
        }
        
        write_vrt_header(hdr, vrt_hdr_bytes_, stream_id_, tsf_ticks, payload_bytes);
    }
    
    // Append and copy IQ data
    int16_t* dst = (int16_t*)rte_pktmbuf_append(m, payload_bytes);
    if (!dst) {
        rte_pktmbuf_free(m);
        return 0;
    }
    
    // Copy samples from buffer (assuming single channel for now)
    const int16_t* src = reinterpret_cast<const int16_t*>(buffs[0]);
    std::memcpy(dst, src, payload_bytes);
    
    // Enqueue to TX ring
    // For now, enqueue single packet. Could be optimized to batch multiple packets
    void* objs[1] = {m};
    unsigned enqueued = rte_ring_enqueue_burst(tx_ring_, objs, 1, nullptr);
    
    if (enqueued == 0) {
        // Ring is full, free the mbuf
        rte_pktmbuf_free(m);
        return 0;
    }
    
    // Successfully sent all samples
    return nsamps_per_buff;
}


} //flexsdr
