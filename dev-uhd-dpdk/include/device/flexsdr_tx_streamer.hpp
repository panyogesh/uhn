#pragma once
#include <uhd/stream.hpp>
#include <uhd/types/metadata.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <atomic>

// Forward declarations for DPDK types
struct rte_ring;
struct rte_mempool;

namespace flexsdr {

struct TxBackend {
  virtual ~TxBackend() = default;
  // Sends one channel burst; returns true if fully enqueued.
  // 'data' is the raw IQ payload for this channel (already interleaved/planar as configured).
  virtual bool send_burst(std::size_t chan,
                          const void* data,
                          std::size_t bytes,
                          uint64_t tsf,
                         uint32_t spp,
                          uint16_t fmt,
                          bool sob,
                          bool eob) = 0;
};

/// Minimal UHD TX streamer that forwards SC16 interleaved samples to a DPDK ring
/// via dpdk_egress. This satisfies the UHD API and lets apps call send().
class flexsdr_tx_streamer final : public uhd::tx_streamer {
public:
    using buffs_type = uhd::tx_streamer::buffs_type;

    // Constructor that accepts a backend
    explicit flexsdr_tx_streamer(TxBackend *backend) : backend_(backend) {}

    ~flexsdr_tx_streamer() override = default;

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
    TxBackend* backend_ = nullptr; // non-owning

   // Basic TX parameters
    std::size_t spp_   = 1024;
    unsigned    burst_ = 32;
    bool        allow_partial_ = true;
    std::size_t  num_chans_ = 1;   // default one channel
    
    // VRT header configuration (set to 0 to disable VRT headers)
    std::size_t vrt_hdr_bytes_ = 32;  // default VRT header size
    uint32_t    stream_id_ = 0;       // default stream ID
};

} //namespace flexsdr
