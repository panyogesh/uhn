#pragma once
extern "C" {
#include <rte_ring.h>
#include <rte_mempool.h>
}
#include <uhd/stream.hpp>
#include <uhd/types/metadata.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <atomic>

// DPDK forward decls to avoid heavy includes in headers
struct rte_ring;
struct rte_mempool;

namespace flexsdr {
/// Minimal UHD TX streamer that forwards SC16 interleaved samples to a DPDK ring
/// via dpdk_egress. This satisfies the UHD API and lets apps call send().
class flexsdr_tx_streamer final : public uhd::tx_streamer {
public:
    using buffs_type = uhd::tx_streamer::buffs_type;

   // DPDK-native ctor (preferred)
    explicit flexsdr_tx_streamer(
        rte_ring* tx_ring,
        rte_mempool* pool,
        std::size_t spp      = 1024,   // samples per packet (default)
        unsigned    burst    = 32,     // mbufs per burst
        bool        allow_partial = true
    );

    // (Optional) keep the old ctor if other code still uses it
    flexsdr_tx_streamer() = default;

    ~flexsdr_tx_streamer() override;

    // UHD::tx_streamer
    size_t get_num_channels() const override;
    size_t get_max_num_samps() const override;

    size_t send(const buffs_type& buffs,
                const size_t nsamps_per_buff,
                const uhd::tx_metadata_t& metadata,
                const double timeout = 0.1) override;
  
    bool recv_async_msg(uhd::async_metadata_t& /*md*/, double /*timeout*/ = 0.1) override {
        return false;
    }

    void post_output_action(
        const std::shared_ptr<uhd::rfnoc::action_info>& ,
        const size_t ) override {};

private:
    // Raw DPDK handles (non-owning)
    rte_ring*    tx_ring_ = nullptr;
    rte_mempool* pool_    = nullptr;

   // Basic TX parameters
    std::size_t spp_   = 1024;
    unsigned    burst_ = 32;
    bool        allow_partial_ = true;
    std::size_t  num_chans_ = 1;   // default one channel
    std::vector<void*> staged_;
    unsigned           staged_i_{0};
    void flush_();
};

} //namespace flexsdr
