// src/device/flexsdr_rx_streamer.cpp
#include "device/flexsdr_rx_streamer.hpp"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <thread>

#include <rte_mbuf.h>
#include <rte_ring.h>
#include <rte_errno.h>

namespace flexsdr {

flexsdr_rx_streamer::flexsdr_rx_streamer(const options& opt)
  : opt_(opt)
{
  if (!opt_.ring) {
    std::fprintf(stderr, "[flexsdr_rx_streamer] WARNING: ring is nullptr\n");
  }
  
  std::fprintf(stderr, "[flexsdr_rx_streamer] Created: %zu channels, max_samps=%zu, burst=%u\n",
               opt_.num_channels, opt_.max_samps, opt_.burst_size);
}

void flexsdr_rx_streamer::issue_stream_cmd(const uhd::stream_cmd_t& cmd) {
  switch (cmd.stream_mode) {
    case uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS:
      running_.store(true);
      std::fprintf(stderr, "[flexsdr_rx_streamer] Stream started (continuous)\n");
      break;
      
    case uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS:
      running_.store(false);
      std::fprintf(stderr, "[flexsdr_rx_streamer] Stream stopped\n");
      break;
      
    case uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE:
      running_.store(true);
      std::fprintf(stderr, "[flexsdr_rx_streamer] Stream started (num_samps=%zu)\n",
                   cmd.num_samps);
      break;
      
    case uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_MORE:
      running_.store(true);
      std::fprintf(stderr, "[flexsdr_rx_streamer] Stream started (num_samps=%zu, more)\n",
                   cmd.num_samps);
      break;
      
    default:
      std::fprintf(stderr, "[flexsdr_rx_streamer] WARNING: Unknown stream mode\n");
      break;
  }
}

size_t flexsdr_rx_streamer::recv(
    const buffs_type& buffs,
    const size_t nsamps_per_buff,
    uhd::rx_metadata_t& metadata,
    const double timeout,
    const bool one_packet)
{
  (void)one_packet;
  
  if (!opt_.ring) {
    metadata.error_code = uhd::rx_metadata_t::ERROR_CODE_TIMEOUT;
    return 0;
  }
  
  if (!running_.load()) {
    metadata.error_code = uhd::rx_metadata_t::ERROR_CODE_TIMEOUT;
    return 0;
  }
  
  // Poll the ring repeatedly until data is available or timeout expires
  void* mbuf_ptrs[opt_.burst_size];
  unsigned n_dequeued = 0;
  
  // Calculate timeout in microseconds for polling
  const auto timeout_us = static_cast<uint64_t>(timeout * 1e6);
  const auto start_time = std::chrono::steady_clock::now();
  
  // Optimized polling strategy:
  // 1. Tight busy-poll for fast path (first ~1000 attempts)
  // 2. Check timeout periodically (every 1000 iterations)
  // 3. Small sleep only after many failed attempts
  // 4. Aggressive ring draining with multiple dequeue attempts
  
  uint64_t poll_attempts = 0;
  const uint64_t TIGHT_POLL_LIMIT = 1000;       // Busy-poll this many times
  const uint64_t TIMEOUT_CHECK_INTERVAL = 1000; // Check timeout every N iterations
  const unsigned MAX_DRAIN_ATTEMPTS = 4;        // Try to drain ring this many times
  
  // Poll loop - keep trying to dequeue until we get data or timeout
  while (n_dequeued == 0 && running_.load()) {
    poll_attempts++;
    
    // Aggressive ring draining - try multiple dequeues to empty the ring
    for (unsigned drain = 0; drain < MAX_DRAIN_ATTEMPTS && n_dequeued < opt_.burst_size; drain++) {
      unsigned n = rte_ring_dequeue_burst(
          opt_.ring,
          &mbuf_ptrs[n_dequeued],
          opt_.burst_size - n_dequeued,
          nullptr);
      
      n_dequeued += n;
      
      // If we got some data, continue draining
      if (n > 0) {
        continue;
      }
      
      // No data on this attempt, break out of drain loop
      break;
    }
    
    if (n_dequeued > 0) {
      break;  // Got data!
    }
    
    // Check timeout periodically (not every iteration - too expensive)
    if (poll_attempts % TIMEOUT_CHECK_INTERVAL == 0) {
      auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - start_time);
      
      if (elapsed.count() >= static_cast<int64_t>(timeout_us)) {
        // Timeout expired
        underruns_++;
        metadata.error_code = uhd::rx_metadata_t::ERROR_CODE_TIMEOUT;
        return 0;
      }
    }
    
    // Sleep strategy: tight polling initially, then small sleeps
    if (poll_attempts > TIGHT_POLL_LIMIT) {
      // Only sleep after many failed attempts (reduces latency)
      // Use 1us sleep instead of 10us for better responsiveness
      std::this_thread::sleep_for(std::chrono::microseconds(1));
    }
    // else: tight busy-poll (no sleep) for fast path
  }
  
  if (n_dequeued == 0) {
    // Still no data (stream was stopped)
    metadata.error_code = uhd::rx_metadata_t::ERROR_CODE_TIMEOUT;
    return 0;
  }
  
  bursts_cons_++;
  
  // Convert buffs_type to vector<void*>
  std::vector<void*> ch_buffs;
  ch_buffs.reserve(buffs.size());
  for (size_t i = 0; i < buffs.size(); i++) {
    ch_buffs.push_back(buffs[i]);
  }
  
  // Use custom unpacker if provided, otherwise default
  size_t samples_written;
  if (opt_.iq_unpack) {
    // Custom SIMD unpacker
    samples_written = opt_.iq_unpack(
        ch_buffs,
        nsamps_per_buff,
        reinterpret_cast<rte_mbuf**>(mbuf_ptrs),
        n_dequeued,
        metadata);
  } else {
    // Default unpacker
    samples_written = default_unpack_sc16_interleaved_(
        ch_buffs,
        nsamps_per_buff,
        reinterpret_cast<rte_mbuf**>(mbuf_ptrs),
        n_dequeued,
        metadata);
  }
  
  // Free mbufs back to pool
  for (unsigned i = 0; i < n_dequeued; i++) {
    if (mbuf_ptrs[i]) {
      rte_pktmbuf_free(static_cast<rte_mbuf*>(mbuf_ptrs[i]));
    }
  }
  
  samples_out_ += samples_written;
  
  // Set metadata
  metadata.error_code = uhd::rx_metadata_t::ERROR_CODE_NONE;
  metadata.has_time_spec = opt_.parse_tsf;
  
  return samples_written;
}

size_t flexsdr_rx_streamer::default_unpack_sc16_interleaved_(
    const std::vector<void*>& ch_buffs,
    size_t nsamps_target,
    rte_mbuf* const* mbufs,
    uint16_t count,
    uhd::rx_metadata_t& md)
{
  const size_t num_ch = get_num_channels();
  size_t total_samples = 0;
  
  // Extract timestamp from first packet if enabled
  if (opt_.parse_tsf && count > 0 && mbufs[0]) {
    uint64_t tsf = extract_tsf_(mbufs[0]);
    md.time_spec = uhd::time_spec_t::from_ticks(tsf, 1.0); // Placeholder tick rate
    md.has_time_spec = true;
  } else {
    md.has_time_spec = false;
  }
  
  // Process each mbuf
  for (uint16_t i = 0; i < count && total_samples < nsamps_target; i++) {
    rte_mbuf* m = mbufs[i];
    
    if (!m || !m->buf_addr) {
      mbuf_errors_++;
      continue;
    }
    
    // Skip VRT header to get to IQ data
    const int16_t* src = rte_pktmbuf_mtod_offset(m, int16_t*, opt_.vrt_hdr_bytes);
    const size_t payload_bytes = m->data_len - opt_.vrt_hdr_bytes;
    
    // Calculate samples in this packet
    // Format: interleaved [CH0_I, CH0_Q, CH1_I, CH1_Q, ...]
    const size_t values_per_sample = 2;  // I + Q
    const size_t total_values = payload_bytes / sizeof(int16_t);
    const size_t samps_in_pkt = total_values / (num_ch * values_per_sample);
    
    // Copy samples to output buffers (deinterleave if multi-channel)
    for (size_t s = 0; s < samps_in_pkt && total_samples < nsamps_target; s++) {
      for (size_t ch = 0; ch < num_ch; ch++) {
        int16_t* out = static_cast<int16_t*>(ch_buffs[ch]);
        const size_t src_idx = s * num_ch * values_per_sample + ch * values_per_sample;
        const size_t dst_idx = total_samples * values_per_sample;
        
        // Copy I and Q
        out[dst_idx] = src[src_idx];         // I
        out[dst_idx + 1] = src[src_idx + 1]; // Q
      }
      total_samples++;
    }
  }
  
  md.error_code = uhd::rx_metadata_t::ERROR_CODE_NONE;
  return total_samples;
}

uint64_t flexsdr_rx_streamer::extract_tsf_(rte_mbuf* m) {
  if (!m || !m->buf_addr || m->data_len < opt_.tsf_offset + sizeof(uint64_t)) {
    return 0;
  }
  
  // Extract 64-bit timestamp at specified offset
  const uint64_t* tsf_ptr = rte_pktmbuf_mtod_offset(m, uint64_t*, opt_.tsf_offset);
  return *tsf_ptr;  // May need endian conversion depending on format
}

} // namespace flexsdr
