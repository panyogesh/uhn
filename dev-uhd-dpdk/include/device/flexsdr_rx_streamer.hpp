#pragma once
#include <uhd/stream.hpp>
#include <uhd/types/metadata.hpp>
#include <memory>
#include <vector>
#include <cstdint>

#include "workers/channel_fifo.hpp"
#include "workers/rx_worker.hpp"

namespace flexsdr {

// Per-channel carry
struct carry_buf_t {
    std::vector<int16_t> iq;  // interleaved I/Q
    size_t read_samps = 0;    // samples already consumed from 'iq' (pairs)
};

// UHD rx_streamer shim that reads from per-channel FIFOs fed by RxWorker
class flexsdr_rx_streamer final : public uhd::rx_streamer {
public:
    using fifo_t = SpscQueue<RxPacket>;

    struct Params {
        std::vector<std::shared_ptr<fifo_t>> fifos; // size = num_channels
        unsigned     num_channels   = 1;
        unsigned     spp            = 1024;     // preferred chunk (not enforced)
        double       tick_rate      = 30.72e6;  // for time conversions
        RxFraming    mode           = RxFraming::Planar; // currently implemented
        // planner mapping hints
        unsigned     pkts_per_chan  = 8;        // how worker groups
    };

    explicit flexsdr_rx_streamer(const Params& p);
    ~flexsdr_rx_streamer();

    // --- UHD interface ---
    size_t get_num_channels() const override;
    size_t get_max_num_samps() const override;

    // We don't implement RFNoC hooks
    void post_input_action(const std::shared_ptr<uhd::rfnoc::action_info>&, size_t) override {}

    // Burst pull: Pops one packet per channel (Planar) or appropriate slices (Interleaved placeholder)
    size_t recv(const buffs_type& buffs,
                size_t nsamps_per_buff,
                uhd::rx_metadata_t& md,
                double timeout = 0.1,
                bool one_packet = false) override;

    // Stream command interface - required by uhd::rx_streamer
    void issue_stream_cmd(const uhd::stream_cmd_t& stream_cmd) override;

private:
    std::vector<std::shared_ptr<fifo_t>> _fifos;
    unsigned     _nch;
    unsigned     _spp;
    double       _tick_rate;
    RxFraming    _mode;
    unsigned     _pkts_per_chan;
    std::vector<carry_buf_t> carry_;
};

} // namespace flexsdr
