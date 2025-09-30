
#include "device/flexsdr_tx_streamer.hpp"

extern "C" {
#include <rte_ring.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
}

#include <cstring>
#include <stdexcept>

namespace flexsdr {
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
    // Minimal stub that validates and returns nsamps without doing full copy/DPDK path yet.
    // Replace with your real mbuf allocation + enqueue burst implementation.
    if (!tx_ring_ || !pool_) return 0;
    if (buffs.size() == 0 || nsamps_per_buff == 0) return 0;
    (void)metadata;

    // You can enforce spp_ here if you want:
    // if (nsamps_per_buff != spp_ && !allow_partial_) return 0;

    // TODO: allocate rte_mbuf* from pool_, copy payload, enqueue to tx_ring_ in bursts.
    // For compile-success & plumbing test, pretend success:
    (void)metadata;
    return nsamps_per_buff;
}


} //flexsdr