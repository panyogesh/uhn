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

    explicit flexsdr_rx_streamer(const Params& p)
      : _fifos(p.fifos),
        _nch(p.num_channels ? p.num_channels : (unsigned)p.fifos.size()),
        _spp(p.spp ? p.spp : 1024),
        _tick_rate(p.tick_rate),
        _mode(p.mode),
        _pkts_per_chan(p.pkts_per_chan ? p.pkts_per_chan : 8) {}

    // --- UHD interface ---
    size_t get_num_channels() const override { return _nch; }
    size_t get_max_num_samps() const override { return _spp; }

    // We don’t implement RFNoC hooks
    void post_input_action(const std::shared_ptr<uhd::rfnoc::action_info>&, size_t) override {}

    // Burst pull: Pops one packet per channel (Planar) or appropriate slices (Interleaved placeholder)
    size_t recv(const buffs_type& buffs,
                size_t nsamps_per_buff,
                uhd::rx_metadata_t& md,
                double timeout = 0.1,
                bool one_packet = false) override
    {
        (void)timeout; (void)one_packet;

        if (buffs.size() < _nch) {
            md.error_code = uhd::rx_metadata_t::ERROR_CODE_BAD_PACKET;
            return 0;
        }

        // For Planar: Take one packet from each channel FIFO (non-blocking), require all present.
        // If any channel is empty, return 0 with TIMEOUT to let caller poll.
        if (_mode == RxFraming::Planar) {
            std::vector<RxPacket> pkts(_nch);
            for (unsigned ch = 0; ch < _nch; ++ch) {
                if (!_fifos[ch] || !_fifos[ch]->pop(pkts[ch])) {
                    md.error_code = uhd::rx_metadata_t::ERROR_CODE_TIMEOUT;
                    md.has_time_spec = false;
                    return 0;
                }
            }

            // Copy samples into user buffers (SC16)
            // Limit to the min nsamps across channels and nsamps_per_buff
            uint32_t min_samps = pkts[0].nsamps;
            for (unsigned ch = 1; ch < _nch; ++ch)
                if (pkts[ch].nsamps < min_samps) min_samps = pkts[ch].nsamps;

            const size_t n_out = static_cast<size_t>(std::min<uint32_t>(min_samps, (uint32_t)nsamps_per_buff));
            for (unsigned ch = 0; ch < _nch; ++ch) {
                auto* dst = static_cast<int16_t*>(buffs[ch]);
                const auto& v = pkts[ch].iq;
                std::memcpy(dst, v.data(), n_out * 2 * sizeof(int16_t));
            }

            // Timestamp: use ch0 if present, convert ticks→secs
            if (pkts[0].have_tsf) {
                md.has_time_spec = true;
                const double secs = double(pkts[0].tsf_ticks) / _tick_rate;
                md.time_spec = uhd::time_spec_t(secs);
            } else {
                md.has_time_spec = false;
            }
            md.end_of_burst = true;
            md.error_code   = uhd::rx_metadata_t::ERROR_CODE_NONE;
            return n_out;
        }

        // Interleaved placeholder: not wired yet
        md.error_code = uhd::rx_metadata_t::ERROR_CODE_TIMEOUT;
        md.has_time_spec = false;
        return 0;
    }

    // Stream command interface - required by uhd::rx_streamer
    void issue_stream_cmd(const uhd::stream_cmd_t& stream_cmd) override {
        // For now, we just accept the command but don't act on it
        // In a full implementation, this would control RX worker start/stop/continuous modes
        (void)stream_cmd;
    }

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
