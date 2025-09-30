#include "device/flexsdr_rx_streamer.hpp"
#include "workers/rx_worker.hpp"
#include "workers/channel_fifo.hpp"

extern "C" {
#include <rte_ring.h>
#include <rte_pause.h>
}

#include <algorithm>
#include <chrono>
#include <cstring>

namespace flexsdr {

static inline void tsf_to_time(double tick_rate, uint64_t tsf, uhd::time_spec_t& ts) {
    const double secs = double(tsf) / tick_rate;
    ts = uhd::time_spec_t(secs);
}

flexsdr_rx_streamer::flexsdr_rx_streamer(rte_ring* ring,
                                         std::size_t num_chans,
                                         double tick_rate_hz,
                                         std::size_t vrt_hdr_bytes,
                                         unsigned   packets_per_chan,
                                         unsigned   /*burst_unused*/,
                                         std::size_t tsf_offset,
                                         RxFraming mode)
: ring_(ring)
, num_chans_(num_chans ? num_chans : 1)
, tick_rate_(tick_rate_hz > 0 ? tick_rate_hz : 30.72e6)
, vrt_hdr_bytes_(vrt_hdr_bytes ? vrt_hdr_bytes : 32)
, pkts_per_chan_(packets_per_chan ? packets_per_chan : 8)
, tsf_offset_(tsf_offset)
, mode_(mode)
, carry_.resize(num_chans_);
{
    // one FIFO per channel (16k packets/ch by default)
    fifos_.resize(num_chans_);
    for (auto& f : fifos_) {
        f = std::make_shared<fifo_t>(1u << 14);
    }

    // worker configuration
    worker_cfg_.ring                 = ring_;
    worker_cfg_.num_channels         = num_chans_;
    worker_cfg_.packets_per_channel  = pkts_per_chan_;
    worker_cfg_.vrt_hdr_bytes        = vrt_hdr_bytes_;
    worker_cfg_.tsf_offset           = tsf_offset_;
    worker_cfg_.tsf_present          = true; // TSF is always present in your flow
    worker_cfg_.mode                 = mode_;
    worker_cfg_.fifos                = &fifos_;
    worker_cfg_.run_flag             = &run_flag_;

    run_flag_.store(true, std::memory_order_relaxed);
    rx_worker_ = start_rx_worker(worker_cfg_);
}

flexsdr_rx_streamer::~flexsdr_rx_streamer() {
    run_flag_.store(false, std::memory_order_relaxed);
    stop_rx_worker(rx_worker_);
}

size_t flexsdr_rx_streamer::get_num_channels() const { return num_chans_; }
size_t flexsdr_rx_streamer::get_max_num_samps() const { return 1u << 16; }

void flexsdr_rx_streamer::issue_stream_cmd(const uhd::stream_cmd_t& cmd) {
    switch (cmd.stream_mode) {
        case uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS:
        case uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE:
            running_.store(true, std::memory_order_release);
            break;
        case uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS:
            running_.store(false, std::memory_order_release);
            break;
        default: break;
    }
}

size_t flexsdr_rx_streamer::recv(const buffs_type& buffs,
                                 const size_t nsamps_per_buff,
                                 uhd::rx_metadata_t& md,
                                 const double timeout,
                                 const bool /*one_packet*/)
{
    md = uhd::rx_metadata_t{};
    if (!running_.load(std::memory_order_acquire)
        || buffs.size() < num_chans_
        || nsamps_per_buff == 0)
    {
        md.error_code = uhd::rx_metadata_t::ERROR_CODE_TIMEOUT;
        return 0;
    }

    std::vector<size_t> wr(num_chans_, 0);

    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::duration<double>(timeout);

    bool any_sob = false, any_eob = false;
    bool tsf_set = false;
    uhd::time_spec_t ts0;

    while (true) {
        bool all_full = true;

        for (size_t ch = 0; ch < num_chans_; ++ch) {
            if (wr[ch] >= nsamps_per_buff) continue;

            auto* dst = static_cast<int16_t*>(buffs[ch]);

            // 1) Consume carry first
            auto& cb = carry_[ch];
            if (cb.read_samps < (cb.iq.size() / 2)) {
                const size_t avail = (cb.iq.size() / 2) - cb.read_samps;
                const size_t take  = std::min(avail, nsamps_per_buff - wr[ch]);
                if (take) {
                    std::memcpy(&dst[2 * wr[ch]],
                                cb.iq.data() + 2 * cb.read_samps,
                                take * 2 * sizeof(int16_t));
                    wr[ch]       += take;
                    cb.read_samps += take;
                }
                if (wr[ch] >= nsamps_per_buff) continue;
            }

            // 2) Pop new packets until we fill the user buffer (or run out)
            while (wr[ch] < nsamps_per_buff) {
                RxPacket pkt;
                if (!fifos_[ch]->pop(pkt)) { all_full = false; break; }

                // Latch metadata flags
                any_sob |= pkt.sob;
                any_eob |= pkt.eob;
                if (!tsf_set && pkt.have_tsf) {
                    tsf_to_time(tick_rate_, pkt.tsf_ticks, ts0);
                    tsf_set = true;
                }

                const size_t need = nsamps_per_buff - wr[ch];
                if (pkt.nsamps <= need) {
                    // Full packet fits
                    std::memcpy(&dst[2 * wr[ch]], pkt.iq.data(), pkt.nsamps * 2 * sizeof(int16_t));
                    wr[ch] += pkt.nsamps;
                } else {
                    // Partial — take 'need' now, stash remainder to carry
                    std::memcpy(&dst[2 * wr[ch]], pkt.iq.data(), need * 2 * sizeof(int16_t));
                    wr[ch] += need;

                    cb.iq.assign(pkt.iq.begin() + need * 2, pkt.iq.end());
                    cb.read_samps = 0;
                }
            }

            if (wr[ch] < nsamps_per_buff) all_full = false;
        }

        if (all_full) break;

        if (std::chrono::steady_clock::now() >= deadline) {
            size_t got_min = SIZE_MAX;
            for (size_t c = 0; c < num_chans_; ++c) got_min = std::min(got_min, wr[c]);
            if (got_min == 0 || got_min == SIZE_MAX) {
                md.error_code = uhd::rx_metadata_t::ERROR_CODE_TIMEOUT;
                return 0;
            }
            md.error_code     = uhd::rx_metadata_t::ERROR_CODE_NONE;
            md.has_time_spec  = tsf_set;
            if (tsf_set) md.time_spec = ts0;
            md.start_of_burst = any_sob;
            md.end_of_burst   = any_eob;
            return got_min;
        }

        rte_pause();
    }

    md.error_code     = uhd::rx_metadata_t::ERROR_CODE_NONE;
    md.has_time_spec  = tsf_set;
    if (tsf_set) md.time_spec = ts0;
    md.start_of_burst = any_sob;
    md.end_of_burst   = any_eob;

    size_t got_min = SIZE_MAX;
    for (size_t c = 0; c < num_chans_; ++c) got_min = std::min(got_min, wr[c]);
    return (got_min == SIZE_MAX) ? 0 : got_min;
}


} // namespace flexsdr
