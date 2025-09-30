#include <uhd/version.hpp>
#include <uhd/device.hpp>
#include <uhd/types/device_addr.hpp>
#include <uhd/usrp/multi_usrp.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <functional>
#include <iomanip>
#include <iostream>

extern "C" {
#include <rte_config.h>
#include <rte_eal.h>
#include <rte_errno.h>
}

#include "flexsdr.grpc.pb.h"               // sanity include

#include "conf/config_params.hpp"
#include "transport/dpdk_common.hpp"
#include "transport/flexsdr_secondary.hpp"
#include "transport/eal_bootstrap.hpp"
#include "device/flexsdr_device.hpp"

// registry.cpp
extern "C" void flexsdr_register_with_uhd();

// -----------------------------------------------------------------------------
// CLI
// -----------------------------------------------------------------------------
struct Cli {
    std::string role = ""; // "ue" or "gnb"
    std::string cfg  = "conf/configurations.yaml";
    std::string args = "type=flexsdr,addr=127.0.0.1,port=50051";
    int hold_secs    = 0;
};

static void usage(const char* prog) {
    std::cout
      << "Usage: " << prog
      << " --role {ue|gnb} [--cfg <yaml>] [--args <uhd_args>] [--hold <sec>]\n";
}

static bool parse_cli(int argc, char** argv, Cli& cli) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* opt)->bool {
            if (i + 1 >= argc) { std::cerr << "Missing value for " << opt << "\n"; return false; }
            return true;
        };
        if (a == "--role") {
            if (!need("--role")) return false; cli.role = argv[++i];
        } else if (a == "--cfg") {
            if (!need("--cfg")) return false; cli.cfg = argv[++i];
        } else if (a == "--args") {
            if (!need("--args")) return false; cli.args = argv[++i];
        } else if (a == "--hold") {
            if (!need("--hold")) return false; cli.hold_secs = std::stoi(argv[++i]);
        } else if (a == "-h" || a == "--help") {
            usage(argv[0]); std::exit(0);
        } else {
            std::cerr << "Unknown option: " << a << "\n"; return false;
        }
    }
    if (cli.role != "ue" && cli.role != "gnb") {
        std::cerr << "ERROR: --role must be 'ue' or 'gnb'\n"; return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
// Pretty dump of discovered addresses
// -----------------------------------------------------------------------------
static void dump_addr(const uhd::device_addr_t& a) {
    auto keys = a.keys();
    std::cout << "  { ";
    for (size_t i = 0; i < keys.size(); ++i) {
        const auto& k = keys[i];
        std::cout << k << "=" << a.get(k, "");
        if (i + 1 < keys.size()) std::cout << ", ";
    }
    std::cout << " }\n";
}

// -----------------------------------------------------------------------------
// Simple RX burst printer
// -----------------------------------------------------------------------------
static void print_rx_burst(const std::vector<void*>& buffs,
                           size_t nsamps_per_buff,
                           const uhd::rx_metadata_t& md,
                           size_t nshow = 8)
{
    const size_t nchan = buffs.size();

    std::printf("[RX] got=%zu ch=%zu ts=%s eob=%d",
                nsamps_per_buff, nchan,
                md.has_time_spec ? "yes" : "no",
                int(md.end_of_burst));
    if (md.has_time_spec) {
        const double t = md.time_spec.get_full_secs() + md.time_spec.get_frac_secs();
        std::printf(" t=%.9f", t);
    }
    std::printf("\n");

    for (size_t c = 0; c < nchan; ++c) {
        const int16_t* iq = static_cast<const int16_t*>(buffs[c]);
        const size_t show = std::min(nshow, nsamps_per_buff);

        long double acc = 0.0L;
        std::printf("  ch%zu: IQ[0..%zu): ", c, show);
        for (size_t i = 0; i < show; ++i) {
            const int16_t I = iq[2*i+0];
            const int16_t Q = iq[2*i+1];
            std::printf("(%d,%d) ", I, Q);
            acc += (long double)I*I + (long double)Q*Q;
        }
        const double rms = std::sqrt((double)(acc / (2.0L*std::max<size_t>(1, show))));
        std::printf("| rms(0..%zu)=%.1f\n", show, rms);
    }
}

// -----------------------------------------------------------------------------
// Spawn a short-lived sniffer thread that prints first N packets (one_packet=true)
// -----------------------------------------------------------------------------
static std::thread launch_rx_sniffer(uhd::rx_streamer::sptr rx,
                                     size_t nsamps_per_buff,
                                     size_t packets_to_print = 5,
                                     size_t nshow_pairs = 8)
{
    return std::thread([rx, nsamps_per_buff, packets_to_print, nshow_pairs]() {
        if (!rx) { std::fprintf(stderr, "[sniffer] null rx_streamer\n"); return; }
        const size_t nchan = rx->get_num_channels();
        if (nchan == 0) { std::fprintf(stderr, "[sniffer] zero channels\n"); return; }

        // allocate buffers (SC16: I/Q interleaved => 2 int16 per sample)
        std::vector<std::vector<int16_t>> ch(nchan);
        for (auto& v : ch) v.resize(nsamps_per_buff * 2);
        std::vector<void*> buffs(nchan);
        for (size_t i = 0; i < nchan; ++i) buffs[i] = ch[i].data();

        // start streaming
        try {
            uhd::stream_cmd_t start(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
            rx->issue_stream_cmd(start);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[sniffer] start failed: %s\n", e.what());
            return;
        }

        uhd::rx_metadata_t md;
        size_t printed = 0;

        while (printed < packets_to_print) {
            size_t got = 0;
            try {
                // one_packet=true => one packet per recv()
                got = rx->recv(buffs, nsamps_per_buff, md, /*timeout*/0.5, /*one_packet*/true);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "[sniffer] recv threw: %s\n", e.what());
                break;
            }

            if (got == 0) {
                if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) continue;
                std::fprintf(stderr, "[sniffer] got=0 err=%d\n", int(md.error_code));
                continue;
            }

            print_rx_burst(buffs, got, md, nshow_pairs);
            ++printed;
        }

        // stop streaming
        try {
            uhd::stream_cmd_t stop(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
            rx->issue_stream_cmd(stop);
        } catch (...) {}
    });
}

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------
int main(int argc, char** argv) {
    std::cout << "UHD version: " << uhd::get_version_string() << "\n";
    std::cout << "gRPC/protobuf generated headers included OK.\n";

    Cli cli;
    if (!parse_cli(argc, argv, cli)) {
        usage(argv[0]);
        return 64;
    }

    // Load YAML
    flexsdr::FullConfig cfg;
    try {
        cfg = flexsdr::load_full_config(cli.cfg);
    } catch (const std::exception& e) {
        std::cerr << "[YAML] load failed: " << e.what() << "\n";
        return 1;
    }

    // Init EAL as SECONDARY
    if (flexsdr::init_eal_from_config(cfg.eal, /*secondary=*/true, {}) < 0) {
        std::cerr << "[EAL] init (secondary) failed: " << rte_strerror(rte_errno) << "\n";
        return 2;
    }

    // Attach to primary-owned rings/pools
    flexsdr::Handles h;
    std::string err;
    if (cli.role == "ue") {
        if (attach_secondary_ue(cfg.ue_app, cfg.defaults, h, err) != 0) {
            std::cerr << "[UE] attach failed: " << err << "\n"; return 3;
        }
        std::cout << "[UE] attached: rings=" << h.rings.size() << " pools=" << h.pools.size() << "\n";
    } else {
        if (attach_secondary_gnb(cfg.gnb_app, cfg.defaults, h, err) != 0) {
            std::cerr << "[gNB] attach failed: " << err << "\n"; return 4;
        }
        std::cout << "[gNB] attached: rings=" << h.rings.size() << " pools=" << h.pools.size() << "\n";
    }

    // Build a DPDK context for the UHD device
    auto ctx = std::make_shared<flexsdr::DpdkContext>();
    if (cli.role == "ue") {
        ctx->ue_inbound_ring_name = cfg.ue_app.inbound_ring;
        ctx->ue_tx_ring0_name     = cfg.ue_app.tx_stream.rings.front();
        ctx->ue_pool_name         = cfg.ue_app.rx_pool;
        ctx->ue_in  = h.rings.count(ctx->ue_inbound_ring_name) ? h.rings.at(ctx->ue_inbound_ring_name) : nullptr;
        ctx->ue_tx0 = h.rings.count(ctx->ue_tx_ring0_name)     ? h.rings.at(ctx->ue_tx_ring0_name)     : nullptr;
        ctx->ue_mp  = h.pools.count(ctx->ue_pool_name)         ? h.pools.at(ctx->ue_pool_name)         : nullptr;
    } else {
        ctx->gnb_inbound_ring_name = cfg.gnb_app.inbound_ring;
        ctx->gnb_tx_ring0_name     = cfg.gnb_app.tx_stream.rings.front();
        ctx->gnb_pool_name         = cfg.gnb_app.rx_pool;
        ctx->gnb_in  = h.rings.count(ctx->gnb_inbound_ring_name) ? h.rings.at(ctx->gnb_inbound_ring_name) : nullptr;
        ctx->gnb_tx0 = h.rings.count(ctx->gnb_tx_ring0_name)     ? h.rings.at(ctx->gnb_tx_ring0_name)     : nullptr;
        ctx->gnb_mp  = h.pools.count(ctx->gnb_pool_name)         ? h.pools.at(ctx->gnb_pool_name)         : nullptr;
    }

    // Register our device factory with UHD
    flexsdr_register_with_uhd();

    try {
        // Discovery (optional)
        uhd::device_addr_t hint; hint.set("type", "flexsdr");
        auto devs = uhd::device::find(hint);
        std::cout << "[DISCOVERY] found " << devs.size() << " candidate(s)\n";
        for (const auto& a : devs) dump_addr(a);

        // Create device via UHD and attach DPDK context
        std::cout << "[UHD] Creating device with args: " << cli.args << "\n";
        auto base = uhd::device::make(uhd::device_addr_t(cli.args));
        auto fdev = std::dynamic_pointer_cast<flexsdr::flexsdr_device>(base);
        if (!fdev) {
            std::cerr << "[ERROR] underlying device is not flexsdr_device\n"; return 6;
        }
        fdev->attach_dpdk_context(ctx, cli.role == "ue" ? flexsdr::Role::UE : flexsdr::Role::GNB);
        std::cout << "[UHD] device OK.\n";

        // === Bring up RX streamer (4 channels) and sniff a few packets ===
        uhd::stream_args_t sargs("sc16", "sc16");
        sargs.channels = {0,1,2,3};
        auto rx = fdev->get_rx_stream(sargs);

        const size_t nsamps_per_buff = 256;  // single-packet sized in your primary path
        auto sniffer = launch_rx_sniffer(rx, nsamps_per_buff, /*packets_to_print=*/5, /*nshow_pairs=*/8);
        sniffer.join();
    }
    catch (const std::exception& e) {
        std::cerr << "[UHD] bring-up failed: " << e.what() << "\n";
        return 7;
    }

    // Optional: keep the process alive for interactive testing
    if (cli.hold_secs > 0) {
        std::cout << "[HOLD] role=" << cli.role << " | sleeping " << cli.hold_secs << "s (Ctrl+C to kill)\n";
        for (int i = 0; i < cli.hold_secs; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    std::cout << "[DONE] secondary(" << cli.role << ") init complete.\n";
    return 0;
}
