#pragma once
#include <cstddef>
#include <cstdint>
#include <thread>
#include <atomic>
#include <memory>
#include <vector>
#include <string>
#include <functional>

extern "C" {
#include <rte_ring.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
}

namespace flexsdr {

// Simple TX item: interleaved SC16 burst + metadata
struct TxItem {
    uint32_t stream_id = 0;
    uint64_t tsf_ticks = 0;        // optional
    const int16_t* iq = nullptr;   // not owning (you can adapt to owning if needed)
    uint32_t nsamps = 0;
};

struct TxWorkerConfig {
    rte_ring*   ring = nullptr;     // enqueue here
    rte_mempool* pool = nullptr;    // allocate mbufs
    std::string name;

    std::size_t vrt_hdr_bytes = 32;
    std::size_t tsf_offset    = 24;
    bool        tsf_present   = true;

    std::atomic<bool>* run_flag = nullptr;
};

struct TxWorkerHandle {
    std::thread thread;
    TxWorkerHandle() = default;
    TxWorkerHandle(TxWorkerHandle&&) = default;
    TxWorkerHandle& operator=(TxWorkerHandle&&) = default;
    TxWorkerHandle(const TxWorkerHandle&) = delete;
    TxWorkerHandle& operator=(const TxWorkerHandle&) = delete;
};

// Start a placeholder TX worker that would pop TxItem from a user-supplied queue
// (left as a stub; you can wire your own queue and lambda to fetch TxItems).
TxWorkerHandle start_tx_worker(const TxWorkerConfig& cfg,
                               std::function<bool(TxItem&)> pop_tx_item);

} // namespace flexsdr
