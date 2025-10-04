#include "device/flexsdr_tx_streamer.hpp"

#include <cstring>
#include <stdexcept>

namespace flexsdr {

size_t flexsdr_tx_streamer::get_num_channels() const {
    return num_chans_;
}


size_t flexsdr_tx_streamer::get_max_num_samps() const {
    // keep simple for now; you can compute from spp_ * burst_ if you wish
    return spp_;
}

size_t flexsdr_tx_streamer::send(const buffs_type& buffs,
                                 size_t nsamps_per_buff,
                                 const uhd::tx_metadata_t& md,
                                 const double /*timeout*/) {
  if (!backend_) {
    // Legacy mode not fully implemented - return 0 for now
    // TODO: Implement direct ring/mempool send for backward compatibility
    return 0;
  }
  
  const bool sob = md.start_of_burst;
  const bool eob = md.end_of_burst;
  const uint64_t tsf = md.has_time_spec ? md.time_spec.to_ticks(1.0) : 0;
  const uint16_t fmt = 1; // SC16 format
  const uint32_t spp = static_cast<uint32_t>(nsamps_per_buff);

  // Assume SC16 (complex int16): 2 samples (I+Q) * 2 bytes = 4 bytes per complex sample
  const size_t bytes_per_sample = 4;
  
  size_t samples_sent = 0;
  for (size_t ch = 0; ch < buffs.size(); ++ch) {
    const void* data = buffs[ch];
    const size_t bytes = nsamps_per_buff * bytes_per_sample;
    
    if (!backend_->send_burst(ch, data, bytes, tsf, spp, fmt, sob, eob)) {
      // Back-pressure or error - stop here
      if (!allow_partial_ || samples_sent == 0) {
        return samples_sent;
      }
      break;
    }
    samples_sent = nsamps_per_buff;
  }
  
  return samples_sent;
}


} //flexsdr
