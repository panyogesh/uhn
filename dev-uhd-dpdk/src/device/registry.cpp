// src/registry.cpp
#include <uhd/device.hpp>
#include <uhd/types/device_addr.hpp>   // <-- needed for device_addr_t / device_addrs_t
#include <uhd/utils/static.hpp>

#include "device/log_compat.hpp"
#include "device/flexsdr_device.hpp"

// ---------- finder ----------
static uhd::device_addrs_t find_flexsdr(const uhd::device_addr_t& hint) {
    uhd::device_addrs_t results;
    const bool type_ok = !hint.has_key("type") || hint.get("type") == "flexsdr";
    if (!type_ok) return results;

    uhd::device_addr_t addr;
    addr["type"] = "flexsdr";
    addr["addr"] = hint.get("addr", "127.0.0.1");
    addr["port"] = hint.get("port", "50051");
    results.push_back(addr);
    return results;
}

// ---------- maker ----------
static uhd::device::sptr make_flexsdr(const uhd::device_addr_t& args) {
    YX_LOG_INFO("FLEXSDR", "Factory creating flexsdr device for " << args.get("addr", ""));
    return std::make_shared<flexsdr::flexsdr_device>(args);
}

// ---------- explicit registration (recommended long-term) ----------
extern "C" void flexsdr_register_with_uhd() {
    static std::atomic<bool> once{false};
    bool expected = false;
    if (once.compare_exchange_strong(expected, true)) {
        // In UHD 4.8: (find_t, make_t, device_filter_t enum)
        uhd::device::register_device(&find_flexsdr, &make_flexsdr, uhd::device::USRP);
    }
}

// Optional: keep static init for convenience (explicit call still recommended)
UHD_STATIC_BLOCK(flexsdr_auto_register) {
    flexsdr_register_with_uhd();
}