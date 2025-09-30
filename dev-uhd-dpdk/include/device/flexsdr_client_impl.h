#ifndef FLEXSDR_CLIENT_IMPL_H
#define FLEXSDR_CLIENT_IMPL_H

#include <iostream>
#include <memory>
#include <string>
#include <algorithm>
#include <cctype>
#include <grpcpp/grpcpp.h>
#include "flexsdr.grpc.pb.h"
#include <uhd/types/tune_request.hpp>
#include <uhd/types/tune_result.h>

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using flexsdr::FlexSDRControl;
using flexsdr::ChannelRequest;
using flexsdr::ClockRateRequestParams;
using flexsdr::ClockRateResponseParams;
using flexsdr::ClockRateResponseParams;
using flexsdr::DeviceInfoRequest;
using flexsdr::DeviceInfoResponse;
using flexsdr::FrequencyRequestParams;
using flexsdr::FrequencyResponseParams;
using flexsdr::FrequencyValue;
using flexsdr::GainRequestParams;
using flexsdr::GainResponseParams;
using flexsdr::RateRequestParams;
using flexsdr::RateResponseParams;
using flexsdr::TuneRequest;
using flexsdr::TuneResult;

class FlexSDRClient {
public:
    static constexpr size_t ALL_CHANS = size_t(~0);
    
    //! tells the host which unit to use
    enum unit_t {
        UNIT_RX   = int('r'),
        UNIT_TX   = int('t'),
        UNIT_BOTH = int('b'),
    };

    FlexSDRClient(std::shared_ptr<Channel> channel);

    // Set and Get clock rate
    bool set_clock_rate(double rate, unit_t unit = UNIT_BOTH);
    double get_clock_rate(unit_t unit = UNIT_BOTH);

    // UHD-style API methods
    bool set_rx_gain(double gain, size_t chan=0, const std::string& name = "");
    double get_rx_gain(size_t chan = 0, const std::string& name = "");

    bool set_tx_gain(double gain, size_t chan = 0, const std::string& name = "");
    double get_tx_gain(size_t chan = 0, const std::string& name = "");

    uhd_tune_result_t set_rx_freq(const uhd::tune_request_t& tune_request, size_t chan = 0);
    double get_rx_freq(size_t chan = 0);

    uhd_tune_result_t set_tx_freq(const uhd::tune_request_t& tune_request, size_t chan = 0);
    double get_tx_freq(size_t chan = 0);

    bool set_rx_rate(double rate, size_t chan = ALL_CHANS);
    double get_rx_rate(size_t chan);

    bool set_tx_rate(double rate, size_t chan = ALL_CHANS);
    double get_tx_rate(size_t chan);

    DeviceInfoResponse get_device_info();

private:
    std::unique_ptr<FlexSDRControl::Stub> stub_;
};

// Function declarations
std::unique_ptr<FlexSDRClient> GetClient(const std::string& server_address = "127.0.0.1:50051");

bool server_request(FlexSDRClient& client, const std::string& op_type);

void PrintUsage(const char* program_name);

// Helper function
std::string to_lower(const std::string& str);

#endif // FLEXSDR_CLIENT_IMPL_H
