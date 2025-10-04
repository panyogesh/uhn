// include/device/flexsdr_rx_streamer.hpp
#pragma once
#include <uhd/stream.hpp>
#include <memory>
#include <functional>
#include <atomic>
#include <vector>

// Forward declarations
namespace uhd { namespace rfnoc { struct action_info; } }
struct rte_mbuf;
struct rte_ring;

namespace flexsdr {

/**
 * High-performance RX streamer for DPDK-based IQ data path
 * Optimized for 50-60 Gbps throughput with optional SIMD unpacking
 */
class flexsdr_rx_streamer final : public uhd::rx_streamer {
public:
  using sptr = std::shared_ptr<flexsdr_rx_streamer>;

  /**
   * Configuration options for RX streamer
   * Supports flexible format conversion and custom SIMD unpackers
   */
  struct options {
    // REQUIRED: DPDK ring from FlexSDRSecondary::rx_ring_for_queue()
    rte_ring*   ring            = nullptr;
    
    // Format specification
    std::string cpu_fmt         = "sc16";   // OAI expects SC16 (complex int16)
    std::string otw_fmt         = "sc16";   // Over-the-wire format
    size_t      num_channels    = 1;        // 1=planar, >=2 often interleaved
    
    // Performance tuning
    size_t      max_samps       = 32768;    // Max samples per recv() call
    uint32_t    burst_size      = 32;       // DPDK ring burst dequeue size
    uint16_t    qid             = 0;        // Queue ID for statistics
    
    // Payload parsing
    bool        parse_tsf       = false;    // Extract timestamp from payload
    size_t      tsf_offset      = 24;       // Byte offset to TSF/timestamp
    size_t      vrt_hdr_bytes   = 32;       // Header bytes to skip before IQ data
    
    /**
     * CRITICAL: Custom SIMD unpacker for high throughput (50-60 Gbps)
     * 
     * If null, uses default_unpack_sc16_interleaved_() which handles
     * basic SC16 deinterleaving but without SIMD optimization.
     * 
     * For maximum performance, provide AVX2/AVX512 implementation that:
     * - Processes multiple samples per instruction
     * - Minimizes cache misses
     * - Handles interleaved → planar conversion efficiently
     * 
     * @param ch_buffs Output buffers, one per channel (planar)
     * @param nsamps_target Requested sample count
     * @param mbufs Input DPDK mbufs containing IQ data
     * @param count Number of mbufs
     * @param md Metadata to populate (timestamp, error flags)
     * @return Number of samples written to each channel
     */
    std::function<size_t(
        const std::vector<void*>& ch_buffs,
        size_t nsamps_target,
        rte_mbuf* const* mbufs,
        uint16_t count,
        uhd::rx_metadata_t& md)> iq_unpack = nullptr;
  };

  // Factory method for shared_ptr construction
  static sptr make(const options& opt) { 
    return sptr(new flexsdr_rx_streamer(opt)); 
  }

  explicit flexsdr_rx_streamer(const options& opt);
  ~flexsdr_rx_streamer() override = default;

  // UHD rx_streamer interface
  size_t get_num_channels() const override { 
    return opt_.num_channels ? opt_.num_channels : 1; 
  }
  
  size_t get_max_num_samps() const override { 
    return opt_.max_samps; 
  }

  size_t recv(const buffs_type& buffs,
              const size_t nsamps_per_buff,
              uhd::rx_metadata_t& metadata,
              const double timeout = 0.1,
              const bool one_packet = false) override;

  void issue_stream_cmd(const uhd::stream_cmd_t& cmd) override;

  // RFNoC hook (no-op for non-RFNoC architecture)
  void post_input_action(
      const std::shared_ptr<uhd::rfnoc::action_info>&, 
      const size_t) override {}

  // Accessors
  uint16_t queue_id() const { return opt_.qid; }
  rte_ring* ring() const { return opt_.ring; }
  size_t num_channels() const { return get_num_channels(); }

  // Statistics (atomic for thread-safety)
  uint64_t samples_out() const { return samples_out_.load(); }
  uint64_t bursts_consumed() const { return bursts_cons_.load(); }
  uint64_t mbuf_errors() const { return mbuf_errors_.load(); }
  uint64_t underruns() const { return underruns_.load(); }
  
  void reset_stats() {
    samples_out_.store(0);
    bursts_cons_.store(0);
    mbuf_errors_.store(0);
    underruns_.store(0);
  }

private:
  /**
   * Default unpacker: SC16 interleaved → planar
   * 
   * Handles multi-channel deinterleaving without SIMD.
   * For production 50-60 Gbps, replace with AVX2/AVX512 version
   * via options::iq_unpack.
   * 
   * Format: Input:  [CH0_I, CH0_Q, CH1_I, CH1_Q, CH2_I, CH2_Q, ...]
   *         Output: ch_buffs[0]: [CH0_I, CH0_Q, CH0_I, CH0_Q, ...]
   *                 ch_buffs[1]: [CH1_I, CH1_Q, CH1_I, CH1_Q, ...]
   */
  size_t default_unpack_sc16_interleaved_(
      const std::vector<void*>& ch_buffs,
      size_t nsamps_target,
      rte_mbuf* const* mbufs,
      uint16_t count,
      uhd::rx_metadata_t& md);

  // Helper: Extract timestamp from packet payload
  uint64_t extract_tsf_(rte_mbuf* m);

private:
  options               opt_{};
  std::atomic<uint64_t> samples_out_{0};
  std::atomic<uint64_t> bursts_cons_{0};
  std::atomic<uint64_t> mbuf_errors_{0};
  std::atomic<uint64_t> underruns_{0};
  std::atomic<bool>     running_{false};
};

} // namespace flexsdr
