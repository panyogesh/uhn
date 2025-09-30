#include "device/flexsdr_client_impl.h"
#include <uhd/types/tune_request.hpp>
#include <uhd/types/tune_result.h>

const std::string ALL_GAINS = "";
const std::string ALL_LOS   = "all";

// FlexSDRClient constructor
FlexSDRClient::FlexSDRClient(std::shared_ptr<Channel> channel)
    : stub_(FlexSDRControl::NewStub(channel)) {}

bool FlexSDRClient::set_clock_rate(double rate, unit_t unit) {
    // Map unit_t to protobuf UnitType
    flexsdr::UnitType proto_unit;
    switch (unit) {
        case UNIT_RX:
            proto_unit = flexsdr::UnitType::UNIT_RX;
            break;
        case UNIT_TX:
            proto_unit = flexsdr::UnitType::UNIT_TX;
            break;
        case UNIT_BOTH:
            proto_unit = flexsdr::UnitType::UNIT_BOTH;
            break;
        default:
            std::cout << "SetClockRate failed: Invalid unit type" << std::endl;
            return false;
        }

    flexsdr::ClockRateRequestParams request;
    request.set_unit(proto_unit);
    request.set_rate(rate);

    flexsdr::ClockRateResponseParams reply;
    grpc::ClientContext context;

    grpc::Status status = stub_->SetClockRate(&context, request, &reply);

    if (status.ok()) {
        std::cout << "Clock rate set successfully for unit: " << unit << std::endl;
        return true;
    } else {
        std::cout << "SetClockRate failed: " << status.error_message() << std::endl;
        return false;
    }
}

double FlexSDRClient::get_clock_rate(unit_t unit) {
    flexsdr::ClockRateRequestParams request;
    flexsdr::UnitType proto_unit;

    switch (unit) {
        case UNIT_RX:
            proto_unit = flexsdr::UnitType::UNIT_RX;
            break;
        case UNIT_TX:
            proto_unit = flexsdr::UnitType::UNIT_TX;
            break;
        case UNIT_BOTH:
            proto_unit = flexsdr::UnitType::UNIT_BOTH;
            break;
        default:
            std::cout << "SetClockRate failed: Invalid unit type" << std::endl;
            return false;
    }

    request.set_unit(proto_unit);

    flexsdr::ClockRateResponseParams reply;
    grpc::ClientContext context;

    grpc::Status status = stub_->GetClockRate(&context, request, &reply);

    if (status.ok()) {
        return reply.actual_rate();
    } else {
        std::cout << "GetClockRate failed: " << status.error_message() << std::endl;
        return 0.0;
    }
}

bool FlexSDRClient::set_rx_gain(double gain, size_t chan, const std::string& name) {
    flexsdr::GainRequestParams request;
    request.set_gain(gain);
    request.set_name(name);
    request.set_chan(chan);

    flexsdr::GainResponseParams reply;
    grpc::ClientContext context;

    grpc::Status status = stub_->SetRxGain(&context, request, &reply);

    if (status.ok() && reply.success()) {
        std::cout << "RX Gain set successfully: " << reply.actual_gain() << " dB" << std::endl;
        return true;
    } else {
        std::cout << "SetRxGain failed: " << reply.error_message() << std::endl;
    }
    
    return false;
}

double FlexSDRClient::get_rx_gain(size_t chan, const std::string& name) {
    flexsdr::GainRequestParams request;
    request.set_name(name);
    request.set_chan(chan);

    flexsdr::GainResponseParams reply;
    grpc::ClientContext context;

    grpc::Status status = stub_->GetRxGain(&context, request, &reply);

    if (status.ok() && reply.success()) {
        return reply.actual_gain();
    } else {
        std::cout << "GetRxGain failed: " << reply.error_message() << std::endl;
        return 0.0;
    }
}

bool FlexSDRClient::set_tx_gain(double gain, size_t chan, const std::string& name) {
    flexsdr::GainRequestParams request;
    request.set_gain(gain);
    request.set_name(name);
    request.set_chan(chan);

    flexsdr::GainResponseParams reply;
    grpc::ClientContext context;

    grpc::Status status = stub_->SetTxGain(&context, request, &reply);

    if (status.ok() && reply.success()) {
        std::cout << "TX Gain set successfully: " << reply.actual_gain() << " dB" << std::endl;
        return true;
    } else {
        std::cout << "SetTxGain failed: " << reply.error_message() << std::endl;
        return false;
    }
}

double FlexSDRClient::get_tx_gain(size_t chan, const std::string& name) {
    flexsdr::GainRequestParams request;
    request.set_name(name.empty() ? "" : name); // Handle ALL_GAINS or default
    request.set_chan(chan);

    flexsdr::GainResponseParams reply;
    grpc::ClientContext context;

    grpc::Status status = stub_->GetTxGain(&context, request, &reply);

    if (status.ok() && reply.success()) {
        return reply.actual_gain();
    } else {
        std::cout << "GetTxGain failed: " << reply.error_message() << std::endl;
        return 0.0;
    }
}
// Util function
bool convert_to_tune_request(flexsdr::TuneRequest* proto_request,
                             const uhd::tune_request_t& uhd_request) {
    proto_request->set_target_freq(uhd_request.target_freq);
    proto_request->set_rf_freq(uhd_request.rf_freq);
    proto_request->set_dsp_freq(uhd_request.dsp_freq);

    switch (uhd_request.rf_freq_policy) {
        case uhd::tune_request_t::POLICY_NONE:
            proto_request->set_rf_freq_policy(flexsdr::TuneRequest::POLICY_NONE);
            break;
        case uhd::tune_request_t::POLICY_AUTO:
            proto_request->set_rf_freq_policy(flexsdr::TuneRequest::POLICY_AUTO);
            break;
        case uhd::tune_request_t::POLICY_MANUAL:
            proto_request->set_rf_freq_policy(flexsdr::TuneRequest::POLICY_MANUAL);
            break;
        default:
            std::cout << "Invalid rf_freq_policy value: " << uhd_request.rf_freq_policy << std::endl;
            return false;
    }

    switch (uhd_request.dsp_freq_policy) {
        case uhd::tune_request_t::POLICY_NONE:
            proto_request->set_dsp_freq_policy(flexsdr::TuneRequest::POLICY_NONE);
            break;
        case uhd::tune_request_t::POLICY_AUTO:
            proto_request->set_dsp_freq_policy(flexsdr::TuneRequest::POLICY_AUTO);
            break;
        case uhd::tune_request_t::POLICY_MANUAL:
            proto_request->set_dsp_freq_policy(flexsdr::TuneRequest::POLICY_MANUAL);
            break;
        default:
            std::cout << "Invalid dsp_freq_policy value: " << uhd_request.dsp_freq_policy << std::endl;
            return false;
    }

    return true;
}

//Util function
uhd_tune_result_t to_tune_result(const TuneResult& proto_result) {
    uhd_tune_result_t cpp_result;

    cpp_result.clipped_rf_freq = proto_result.clipped_rf_freq();
    cpp_result.target_rf_freq = proto_result.target_rf_freq();
    cpp_result.actual_rf_freq = proto_result.actual_rf_freq();
    cpp_result.target_dsp_freq = proto_result.target_dsp_freq();
    cpp_result.actual_dsp_freq = proto_result.actual_dsp_freq();

    return cpp_result;
}

uhd_tune_result_t FlexSDRClient::set_rx_freq(const uhd::tune_request_t& uhd_request, size_t chan) {
    flexsdr::FrequencyRequestParams request;
    if (!convert_to_tune_request(request.mutable_tune_request(), uhd_request)) {
        std::cout << "SetRxFreq failed: Invalid tune request parameters" << std::endl;
        return uhd_tune_result_t(); // Return default struct on failure
    }
    request.mutable_channel_request()->set_chan(chan);

    flexsdr::FrequencyResponseParams reply;
    grpc::ClientContext context;

    grpc::Status status = stub_->SetRxFreq(&context, request, &reply);

    if (status.ok() && reply.success()) {
        std::cout << "RX Frequency set successfully: " << reply.tune_result().actual_rf_freq()/1e6 << " MHz" << std::endl;
        return to_tune_result(reply.tune_result());
    } else {
        std::cout << "SetRxFreq failed: " << reply.error_message() << std::endl;
        return uhd_tune_result_t(); // Return default struct on failure
    }
}

double FlexSDRClient::get_rx_freq(size_t chan) {
    flexsdr::ChannelRequest request;
    request.set_chan(chan);

    flexsdr::FrequencyValue reply;
    grpc::ClientContext context;

    grpc::Status status = stub_->GetRxFreq(&context, request, &reply);

    if (status.ok() && reply.success()) {
        return reply.frequency();
    } else {
        std::cout << "GetRxFreq failed: " << reply.error_message() << std::endl;
        return 0.0;
    }
}

uhd_tune_result_t FlexSDRClient::set_tx_freq(const uhd::tune_request_t& uhd_request, size_t chan) {
    flexsdr::FrequencyRequestParams request;
    if (!convert_to_tune_request(request.mutable_tune_request(), uhd_request)) {
        std::cout << "SetRxFreq failed: Invalid tune request parameters" << std::endl;
        return uhd_tune_result_t(); // Return default struct on failure
    }
    request.mutable_channel_request()->set_chan(chan);

    flexsdr::FrequencyResponseParams reply;
    grpc::ClientContext context;

    grpc::Status status = stub_->SetTxFreq(&context, request, &reply);

    if (status.ok() && reply.success()) {
        std::cout << "TX Frequency set successfully: " << reply.tune_result().actual_rf_freq()/1e6 << " MHz" << std::endl;
        return to_tune_result(reply.tune_result());
    } else {
        std::cout << "SetTxFreq failed: " << reply.error_message() << std::endl;
        return uhd_tune_result_t(); // Return default struct on failure
    }
}


double FlexSDRClient::get_tx_freq(size_t chan) {
    flexsdr::ChannelRequest request;
    request.set_chan(chan);

    flexsdr::FrequencyValue reply;
    grpc::ClientContext context;

    grpc::Status status = stub_->GetTxFreq(&context, request, &reply);

    if (status.ok() && reply.success()) {
        return reply.frequency();
    } else {
        std::cout << "GetRxFreq failed: " << reply.error_message() << std::endl;
        return 0.0;
    }
}

bool FlexSDRClient::set_rx_rate(double rate, size_t chan) {
    flexsdr::RateRequestParams request;
    request.set_rate(rate);
    request.set_chan(chan);

    flexsdr::RateResponseParams reply;
    grpc::ClientContext context;

    grpc::Status status = stub_->SetRxRate(&context, request, &reply);

    if (status.ok() && reply.success()) {
        std::cout << "RX Rate set successfully: " << reply.actual_rate() << " dB" << std::endl;
        return true;
    } else {
        std::cout << "SetRxRate failed: " << reply.error_message() << std::endl;
        return false;
    }
}

double FlexSDRClient::get_rx_rate(size_t chan) {
    flexsdr::RateRequestParams request;
    request.set_chan(chan);

    flexsdr::RateResponseParams reply;
    grpc::ClientContext context;

    grpc::Status status = stub_->GetRxRate(&context, request, &reply);

    if (status.ok() && reply.success()) {
        return reply.actual_rate();
    } else {
        std::cout << "GetRxRate failed: " << reply.error_message() << std::endl;
        return 0.0;
    }
}

bool FlexSDRClient::set_tx_rate(double rate, size_t chan) {
    flexsdr::RateRequestParams request;
    request.set_rate(rate);
    request.set_chan(chan);

    flexsdr::RateResponseParams reply;
    grpc::ClientContext context;

    grpc::Status status = stub_->SetTxRate(&context, request, &reply);

    if (status.ok() && reply.success()) {
        std::cout << "TX Rate set successfully: " << reply.actual_rate() << " dB" << std::endl;
        return true;
    } else {
        std::cout << "SetTxRate failed: " << reply.error_message() << std::endl;
        return false;
    }
}

double FlexSDRClient::get_tx_rate(size_t chan) {
    flexsdr::RateRequestParams request;
    request.set_chan(chan);

    flexsdr::RateResponseParams reply;
    grpc::ClientContext context;

    grpc::Status status = stub_->GetTxRate(&context, request, &reply);

    if (status.ok() && reply.success()) {
        return reply.actual_rate();
    } else {
        std::cout << "GetTxRate failed: " << reply.error_message() << std::endl;
        return 0.0;
    }
}

DeviceInfoResponse FlexSDRClient::get_device_info() {
    DeviceInfoRequest request;
    DeviceInfoResponse reply;
    ClientContext context;

    Status status = stub_->GetDeviceInfo(&context, request, &reply);

    if (status.ok()) {
        std::cout << "Device: " << reply.device_name() << std::endl;
        std::cout << "Serial: " << reply.serial_number() << std::endl;
        std::cout << "RX Gain Range: " << reply.min_rx_gain() << " to "
                  << reply.max_rx_gain() << " dB" << std::endl;
        std::cout << "TX Gain Range: " << reply.min_tx_gain() << " to "
                  << reply.max_tx_gain() << " dB" << std::endl;
    } else {
        std::cout << "Failed to get device info" << std::endl;
    }

    return reply;
}

// Function to create and return a FlexSDR client
std::unique_ptr<FlexSDRClient> GetClient(const std::string& server_address) {
    auto channel = grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());
    return std::make_unique<FlexSDRClient>(channel);
}

// Helper function to convert string to lowercase
std::string to_lower(const std::string& str) {
    std::string lower_str = str;
    std::transform(lower_str.begin(), lower_str.end(), lower_str.begin(), ::tolower);
    return lower_str;
}

