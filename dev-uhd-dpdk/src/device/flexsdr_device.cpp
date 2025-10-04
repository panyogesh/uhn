// src/device/flexsdr_device.cpp
#include "device/flexsdr_device.hpp"
#include "device/flexsdr_rx_streamer.hpp"
#include "device/flexsdr_tx_streamer.hpp"

// DPDK headers only in .cpp
extern "C" {
#include <rte_ring.h>
#include <rte_mempool.h>
}

#include "transport/flexsdr_secondary.hpp"
#include "transport/dpdk_common.hpp"

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <uhd/types/ranges.hpp>
#include <iostream>

namespace flexsdr {

//==============================
// Internal state
//==============================
struct flexsdr_device::Impl {
  uhd::device_addr_t args;
  std::shared_ptr<DpdkContext> ctx;
  Role role = Role::UE;
  std::atomic<bool> resolved{false};     // ring/pool resolved once

  // Optional: fallback ring from device args (legacy)
  ::rte_ring* arg_rx_ring = nullptr;
};

// Helpers
static inline rte_ring* ring_lookup(const std::string& n) {
  return n.empty() ? nullptr : rte_ring_lookup(n.c_str());
}
static inline rte_mempool* mp_lookup(const std::string& n) {
  return n.empty() ? nullptr : rte_mempool_lookup(n.c_str());
}

//==============================
// Property tree
//==============================
void flexsdr_device::_init_tree() {
  const std::string mb = "/mboards/0";

  // Essential nodes to satisfy multi_usrp paths you use
  _tree->create<std::string>(mb + "/name").set("FlexSDR");
  _tree->create<double>(mb + "/tick_rate").set(_mcr);

  // Add more nodes as consumers require them:
  // _tree->create<double>(mb + "/rx_frontends/0/rate/value").set(_rxr);
  // _tree->create<double>(mb + "/tx_frontends/0/rate/value").set(_txr);
}

//==============================
// Ctor / Dtor
//==============================
flexsdr_device::flexsdr_device(const uhd::device_addr_t& args)
  : p_(new Impl{}),
    _tree(uhd::property_tree::make()),
    _endpoint(args.get("addr", "127.0.0.1:50051"))
{
  // sensible defaults before touching tree
  _mcr = 30.72e6;   // tick rate (samples/sec)
  _rxr = 5e6;
  _txr = 5e6;
  _rxf = 3.5e9;
  _txf = 3.5e9;
  _rxg = 0.0;
  _txg = 0.0;

  // optional args
  _ring_name   = args.get("ring", _ring_name);
  _file_prefix = args.get("file_prefix", _file_prefix);

  p_->args = args;

  _client = GetClient(_endpoint);
  _init_tree();
}

flexsdr_device::~flexsdr_device() = default;

//==============================
// DPDK context & ingress
//==============================
void flexsdr_device::attach_dpdk_context(std::shared_ptr<DpdkContext> ctx, Role role) {
  p_->ctx = std::move(ctx);
  p_->role = role;
  p_->resolved.store(false, std::memory_order_release);
}

void flexsdr_device::_start_ingress_if_needed() {
  if (_ingress_started.load(std::memory_order_acquire)) return;

  // Optional legacy arg ring lookup
  if (!_ring_name.empty()) {
    _rx_ring = ring_lookup(_ring_name);
    p_->arg_rx_ring = _rx_ring;
  }
  _ingress_started.store(true, std::memory_order_release);
}

//==============================
// UHD streamers
//==============================
uhd::rx_streamer::sptr
flexsdr_device::get_rx_stream(const uhd::stream_args_t& args)
{
  _start_ingress_if_needed();

  // choose the primary ingress ring by role
  ::rte_ring* rx_ring = nullptr;
  if (p_->ctx) {
    rx_ring = (p_->role == Role::UE) ? p_->ctx->ue_in : p_->ctx->gnb_in;
  }
  if (!rx_ring && !_ring_name.empty()) {
    rx_ring = ring_lookup(_ring_name); // last-resort fallback
  }
  if (!rx_ring) {
    throw std::runtime_error("RX: no DPDK ring attached; primary must create UE_in/GNB_in and secondary must attach");
  }

  // channels from args (default 1)
  std::size_t num_chans = args.channels.empty() ? 1 : args.channels.size();

  // Create options for new API
  flexsdr_rx_streamer::options opts;
  opts.ring = rx_ring;
  opts.num_channels = num_chans;
  opts.cpu_fmt = "sc16";
  opts.otw_fmt = "sc16";
  opts.max_samps = 32768;
  opts.burst_size = 32;
  opts.parse_tsf = false;
  opts.vrt_hdr_bytes = 32;
  opts.qid = 0;

  return flexsdr_rx_streamer::make(opts);
}

uhd::tx_streamer::sptr
flexsdr_device::get_tx_stream(const uhd::stream_args_t& args)
{
  (void)args;

  // One-time resolve of ring/pool from context (and/or names)
  bool expected = false;
  if (p_->resolved.compare_exchange_strong(expected, true)) {
    if (p_->ctx) {
      if (!p_->ctx->ue_in)   p_->ctx->ue_in   = ring_lookup(p_->ctx->ue_inbound_ring_name);
      if (!p_->ctx->ue_tx0)  p_->ctx->ue_tx0  = ring_lookup(p_->ctx->ue_tx_ring0_name);
      if (!p_->ctx->gnb_in)  p_->ctx->gnb_in  = ring_lookup(p_->ctx->gnb_inbound_ring_name);
      if (!p_->ctx->gnb_tx0) p_->ctx->gnb_tx0 = ring_lookup(p_->ctx->gnb_tx_ring0_name);
      if (!p_->ctx->ue_mp)   p_->ctx->ue_mp   = mp_lookup(p_->ctx->ue_pool_name);
      if (!p_->ctx->gnb_mp)  p_->ctx->gnb_mp  = mp_lookup(p_->ctx->gnb_pool_name);
    } else if (!_ring_name.empty()) {
      // last-resort legacy fallback: only a single ring from args
      p_->arg_rx_ring = ring_lookup(_ring_name);
    }
  }

  // Get backend from context - should be FlexSDRSecondary which implements TxBackend
  TxBackend* backend = nullptr;
  if (p_->ctx && p_->ctx->secondary) {
    backend = p_->ctx->secondary;
  }
  
  if (!backend) {
    throw std::runtime_error("TX: no TxBackend available; ensure FlexSDRSecondary is attached to context");
  }

  return std::make_shared<flexsdr_tx_streamer>(backend);
}

bool flexsdr_device::recv_async_msg(uhd::async_metadata_t&, double) {
  return false;
}

//==============================
// UHD parameter surface
//==============================
void flexsdr_device::set_rx_rate(double rate, size_t chan) {
  (void)chan;
  rate = _clamp(rate, 1e3, 100e6);
  if (_client) _client->set_rx_rate(rate, chan);
  _rxr = rate;
}

double flexsdr_device::get_rx_rate(size_t chan) const {
  return _client ? _client->get_rx_rate(chan) : _rxr;
}

void flexsdr_device::set_tx_rate(double rate, size_t chan) {
  (void)chan;
  rate = _clamp(rate, 1e3, 100e6);
  if (_client) _client->set_tx_rate(rate, chan);
  _txr = rate;
}

double flexsdr_device::get_tx_rate(size_t chan) const {
  return _client ? _client->get_tx_rate(chan) : _txr;
}

void flexsdr_device::set_rx_freq(const uhd::tune_request_t& req, size_t chan) {
  (void)chan;
  _rxf = _clamp(req.target_freq, 1e6, 6e9);
  if (_client) { (void)_client->set_rx_freq(req, chan); }
  // Optionally reflect to tree if you add that node
}

double flexsdr_device::get_rx_freq(size_t chan) const {
  return _client ? _client->get_rx_freq(chan) : _rxf;
}

void flexsdr_device::set_tx_freq(const uhd::tune_request_t& req, size_t chan) {
  (void)chan;
  _txf = _clamp(req.target_freq, 1e6, 6e9);
  if (_client) { (void)_client->set_tx_freq(req, chan); }
}

double flexsdr_device::get_tx_freq(size_t chan) const {
  return _client ? _client->get_tx_freq(chan) : _txf;
}

void flexsdr_device::set_rx_gain(double gain, size_t chan) {
  (void)chan;
  gain = _clamp(gain, 0.0, 70.0);
  if (_client) _client->set_rx_gain(gain, chan);
  _rxg = gain;
}

double flexsdr_device::get_rx_gain(size_t chan) const {
  return _client ? _client->get_rx_gain(chan) : _rxg;
}

void flexsdr_device::set_tx_gain(double gain, size_t chan) {
  (void)chan;
  gain = _clamp(gain, 0.0, 70.0);
  if (_client) _client->set_tx_gain(gain, chan);
  _txg = gain;
}

double flexsdr_device::get_tx_gain(size_t chan) const {
  return _client ? _client->get_tx_gain(chan) : _txg;
}

void flexsdr_device::set_clock_rate(double rate) {
  rate = _clamp(rate, 1e6, 1e9);
  if (_client) _client->set_clock_rate(rate);
  _mcr = rate;
  try {
    _tree->access<double>("/mboards/0/tick_rate").set(rate);
  } catch (...) {
    // node may not exist yet; safe to ignore during bring-up
  }
}

} // namespace flexsdr
