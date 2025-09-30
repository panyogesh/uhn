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

flexsdr_rx_streamer::flexsdr_rx_streamer(const Params& p)
: _fifos(p.fifos)
, _nch(p.num_channels ? p.num_channels : (unsigned)p.fifos.size())
, _spp(p.spp ? p.spp : 1024)
, _tick_rate(p.tick_rate)
, _mode(p.mode)
, _pkts_per_chan(p.pkts_per_chan ? p.pkts_per_chan : 8)
{
    carry_.resize(_nch);
    
    // If FIFOs were not provided, create them (for legacy compatibility)
    if (_fifos.empty() || _fifos[0] == nullptr) {
        _fifos.resize(_nch);
        for (auto& f : _fifos) {
            f = std::make_shared<fifo_t>(1u << 14);
        }
    }
    
    // Note: Worker configuration would go here when integrating with RxWorker
    // For now, we'll rely on the FIFOs being populated externally
}

flexsdr_rx_streamer::~flexsdr_rx_streamer() {
    // Worker cleanup would go here when integrated
}

size_t flexsdr_rx_streamer::get_num_channels() const { return _nch; }
size_t flexsdr_rx_streamer::get_max_num_samps() const { return _spp; }

void flexsdr_rx_streamer::issue_stream_cmd(const uhd::stream_cmd_t& cmd) {
    // For now, just accept the command
    // In full implementation, this would control RX worker start/stop/continuous modes
    (void)cmd;
}

size_t flexsdr_rx_streamer::recv(const buffs_type& buffs,
                                 const size_t nsamps_per_buff,
                                 uhd::rx_metadata_t& md,
                                 const double timeout,
                                 const bool /*one_packet*/)
{
    md = uhd::rx_metadata_t{};
    if (buffs.size() < _nch
        || nsamps_per_buff == 0)
    {
        md.error_code = uhd::rx_metadata_t::ERROR_CODE_TIMEOUT;
        return 0;
    }

    std::vector<size_t> wr(_nch, 0);

    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::duration<double>(timeout);

    bool any_sob = false, any_eob = false;
    bool tsf_set = false;
    uhd::time_spec_t ts0;

    while (true) {
        bool all_full = true;

        for (size_t ch = 0; ch < _nch; ++ch) {
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
                if (!_fifos[ch]->pop(pkt)) { all_full = false; break; }

                // Latch metadata flags
                any_sob |= pkt.sob;
                any_eob |= pkt.eob;
                if (!tsf_set && pkt.have_tsf) {
                    tsf_to_time(_tick_rate, pkt.tsf_ticks, ts0);
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
            for (size_t c = 0; c < _nch; ++c) got_min = std::min(got_min, wr[c]);
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
    for (size_t c = 0; c < _nch; ++c) got_min = std::min(got_min, wr[c]);
    return (got_min == SIZE_MAX) ? 0 : got_min;
}


} // namespace flexsdr
