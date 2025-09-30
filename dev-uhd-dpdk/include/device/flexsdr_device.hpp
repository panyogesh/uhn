#pragma once
#include <uhd/device.hpp>
#include <uhd/property_tree.hpp>
#include <uhd/stream.hpp>
#include <uhd/types/metadata.hpp>
#include <uhd/types/time_spec.hpp>
#include <uhd/types/tune_request.hpp>
#include <uhd/rfnoc/actions.hpp>
#include <memory>
#include <string>
#include <vector>
#include <iostream>
#include <thread>
#include <memory>
#include <atomic>


#include "device/flexsdr_client_impl.h"   // FlexSDRClient, GetClient(endpoint)

struct rte_ring; // fwd decl - use global scope
struct rte_mempool;

namespace flexsdr {

enum class Role { UE, GNB };

// DPDK-native context (non-owning views of primary-owned objects)
struct DpdkContext {
  // Prefer names + pointers (names allow late re-resolve if primary restarts)
  std::string ue_inbound_ring_name;
  std::string ue_tx_ring0_name;
  std::string gnb_inbound_ring_name;
  std::string gnb_tx_ring0_name;
  std::string ue_pool_name;
  std::string gnb_pool_name;

  // Optional cached pointers (non-owning; primary owns lifetime)
  rte_ring*    ue_in   = nullptr;
  rte_ring*    ue_tx0  = nullptr;
  rte_ring*    gnb_in  = nullptr;
  rte_ring*    gnb_tx0 = nullptr;
  rte_mempool* ue_mp   = nullptr;
  rte_mempool* gnb_mp  = nullptr;
};


class flexsdr_device : public uhd::device {
public:
    static constexpr size_t ALL_CHANS = size_t(~0);

    explicit flexsdr_device(const uhd::device_addr_t& args);
    ~flexsdr_device() noexcept  override;

    // Attach DPDK handles after EAL init (secondary). Non-owning.
    void attach_dpdk_context(std::shared_ptr<DpdkContext> ctx, Role role);

    // UHD interface
    uhd::rx_streamer::sptr get_rx_stream(const uhd::stream_args_t&) override;
    uhd::tx_streamer::sptr get_tx_stream(const uhd::stream_args_t&) override;
    bool recv_async_msg(uhd::async_metadata_t& md, double timeout = 0.1) override;

    // Clock
    void   set_clock_rate(double rate_hz);

    // Gains
    void   set_rx_gain(double gain, size_t chan = 0);
    double get_rx_gain(size_t chan = 0) const;

    void   set_tx_gain(double gain, size_t chan = 0);
    double get_tx_gain(size_t chan = 0) const;

    // Rates
    void   set_rx_rate(double rate, size_t chan = ALL_CHANS);
    double get_rx_rate(size_t chan = 0) const;

    void   set_tx_rate(double rate, size_t chan = ALL_CHANS);
    double get_tx_rate(size_t chan = 0) const;

    // Frequencies
    void   set_rx_freq(const uhd::tune_request_t& tune_req, size_t chan = 0);
    double get_rx_freq(size_t chan = 0) const;

    void   set_tx_freq(const uhd::tune_request_t& tune_req, size_t chan = 0);
    double get_tx_freq(size_t chan = 0) const;

    std::string get_endpoint() const { return _endpoint; }

private:
    struct Impl;  
    std::unique_ptr<Impl> p_;

    void _init_tree();

    void _start_ingress_if_needed();

    // DPDK ring name (from args or default)
    std::string _ring_name{"ue_inbound_ring"};

    // FIFO + ingress thread for RX
    std::atomic<bool> _ingress_started{false};
    ::rte_ring* _rx_ring{nullptr};

    // (optional) remember file-prefix to remind users
    std::string _file_prefix{"shm1"};

    template <typename T> static inline T _clamp(T v, T lo, T hi) {
        return (v < lo) ? lo : (v > hi) ? hi : v;
    }

    // cached state
    double _mcr  = 200e6;                 // master clock (Hz)
    double _rxr  = 10e6, _txr = 10e6;     // rates
    double _rxf  = 2.45e9, _txf = 2.45e9; // freqs
    double _rxg  = 10.0,  _txg = 10.0;    // gains

    uhd::property_tree::sptr _tree;

    // backend client (keep intact)
    std::shared_ptr<FlexSDRClient> _client;
    std::string _endpoint;
};

}
