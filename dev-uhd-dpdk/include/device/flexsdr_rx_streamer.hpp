// include/device/flexsdr_rx_streamer.hpp
#pragma once
#include <uhd/stream.hpp>

// forward-declare RFNoC action_info to avoid heavy includes
namespace uhd { namespace rfnoc { struct action_info; } }

// only forward-declare DPDK here
struct rte_ring;

namespace flexsdr {

class flexsdr_rx_streamer final : public uhd::rx_streamer {
public:
  // ring: primary’s RX ring (UE_in or GNB_in)
  // num_chans: #OAI antennas
  // tick_rate_hz: your mboard tick rate (e.g., 30.72e6)
  // vrt_hdr_bytes: size of the VRT header up to first iq byte (assume 28 or 32)
  // packets_per_chan: your schedule (=8)
  explicit flexsdr_rx_streamer(rte_ring* ring,
                               std::size_t num_chans,
                               double tick_rate_hz,
                               std::size_t vrt_hdr_bytes = 32,
                               unsigned packets_per_chan = 8,
                               unsigned burst = 32,
                               std::size_t tsf_offset = 24);

  // uhd::rx_streamer
  size_t get_num_channels() const override;
  size_t get_max_num_samps() const override;  // just a hint; we’ll fill up to nsamps_per_buff
  void   issue_stream_cmd(const uhd::stream_cmd_t& cmd) override;
  size_t recv(const buffs_type& buffs,
              const size_t nsamps_per_buff,
              uhd::rx_metadata_t& md,
              const double timeout = 0.1,
              const bool one_packet = false) override;

  // UHD 4.8 RFNoC hook (pure virtual in base). We implement a no-op.
  void post_input_action(
      const std::shared_ptr<uhd::rfnoc::action_info>& ,
      const size_t ) override {};

  //void post_input_action(const std::shared_ptr<uhd::rfnoc::action_info>&, size_t) override;
private:
  rte_ring*    ring_ = nullptr;     // non-owning
  std::size_t  num_chans_ = 1;
  double       tick_rate_ = 30.72e6;

  // VRT specifics
  std::size_t  vrt_hdr_bytes_   = 32;     // bytes to skip before first iq
  unsigned     pkts_per_chan_   = 8;      // schedule: 8 packets per channel
  unsigned     burst_           = 32;
  std::size_t  tsf_offset_      = 24;

  // running state
  bool         running_         = false;
  uint64_t     pkt_counter_     = 0;      // increments per packet; drives channel schedule
};

} // namespace flexsdr
