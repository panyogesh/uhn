#include <uhd/version.hpp>
#include <uhd/device.hpp>
#include <uhd/types/device_addr.hpp>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <functional>
#include <cstring>
#include <iomanip>
#include <cmath>

// Validate generated headers wiring:
#include "flexsdr.grpc.pb.h"

// From your registry.cpp
extern "C" void flexsdr_register_with_uhd();

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

// ======== DPDK/secondary bits (borrowed from your infra test) ========
extern "C" {
#include <rte_config.h>
#include <rte_eal.h>
#include <rte_errno.h>
}

#include "conf/config_params.hpp"
#include "transport/dpdk_common.hpp"
#include "transport/flexsdr_secondary.hpp"
#include "transport/eal_bootstrap.hpp"
#include "device/flexsdr_device.hpp"
#include "workers/streamer_workers.hpp"

// TsfMonitor loop
struct TsfMonitor {
  bool     have_last = false;
  double   last_secs = 0.0;
  uint64_t bursts    = 0;
  uint64_t ok        = 0;
  uint64_t warn      = 0;
  double   tol       = 2e-6; // 2 microseconds tolerance; tune if needed
  double   tick_rate = 30.72e6; // match _mcr

  void check(const uhd::rx_metadata_t& md, size_t got) {
    ++bursts;
    if (!md.has_time_spec) return;  // nothing to check this burst
    const double now = md.time_spec.get_real_secs();
    if (have_last) {
      const double expect = double(got) / tick_rate;
      const double dt     = now - last_secs;
      const double err    = std::abs(dt - expect);
      if (err <= tol) ++ok;
      else {
        ++warn;
        std::cout << "[TSF] Δt=" << std::fixed << std::setprecision(7) << dt
                  << " s, expect=" << expect
                  << " (err=" << std::setprecision(7) << err << ")\n";
      }
    }
    last_secs = now;
    have_last = true;
  }

  void summary() const {
    std::cout << "[TSF] bursts=" << bursts << " ok=" << ok << " warn=" << warn << "\n";
  }
};

// Drop this helper into test_flexsdr_factory.cpp
struct SlotAccumulator {
  const size_t nchan;
  const size_t slot_nsamps; // e.g., 30720
  std::vector<std::vector<int16_t>> buf;  // [ch][2*slot_nsamps]
  std::vector<size_t> wr;                 // [ch] written samples

  explicit SlotAccumulator(size_t nchan_, size_t slot_nsamps_)
  : nchan(nchan_), slot_nsamps(slot_nsamps_), buf(nchan_), wr(nchan_, 0) {
    for (auto& v : buf) v.resize(slot_nsamps * 2);
  }

  // append 'got' samples per channel from burst buffers
  void push(const std::vector<void*>& burst, size_t got) {
    for (size_t ch = 0; ch < nchan; ++ch) {
      auto* src = static_cast<const int16_t*>(burst[ch]);
      auto* dst = buf[ch].data() + (wr[ch] * 2);
      std::memcpy(dst, src, got * 2 * sizeof(int16_t));
      wr[ch] += got;
    }
  }

  bool full() const {
    for (size_t ch = 0; ch < nchan; ++ch)
      if (wr[ch] < slot_nsamps) return false;
    return true;
  }

  // call when full(): hands out pointers & resets for next slot
  void consume(std::function<void(const std::vector<int16_t*>&)> on_slot) {
    std::vector<int16_t*> views(nchan);
    for (size_t ch = 0; ch < nchan; ++ch) views[ch] = buf[ch].data();
    on_slot(views);
    std::fill(wr.begin(), wr.end(), 0);
  }
};

// ======== very small CLI ========
struct Cli {
    std::string role = "";                           // "ue" or "gnb"
    std::string cfg  = "conf/configurations.yaml";   // YAML path
    std::string args = "type=flexsdr,addr=127.0.0.1,port=50051"; // UHD args
    int hold_secs = 0;                                // optional: keep the process alive
};

static void usage(const char* prog) {
    std::cout
      << "Usage: " << prog << " --role {ue|gnb} [--cfg <yaml>] [--args <uhd_args>] [--hold <seconds>]\n"
      << "  Example: " << prog << " --role ue --cfg conf/configurations.yaml --args \"type=flexsdr,addr=127.0.0.1,port=50051\" --hold 0\n";
}

static bool parse_cli(int argc, char** argv, Cli& cli) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* opt)->bool {
            if (i + 1 >= argc) { std::cerr << "Missing value for " << opt << "\n"; return false; }
            return true;
        };
        if (a == "--role") {
            if (!need("--role")) return false;
            cli.role = argv[++i];
        } else if (a == "--cfg") {
            if (!need("--cfg")) return false;
            cli.cfg = argv[++i];
        } else if (a == "--args") {
            if (!need("--args")) return false;
            cli.args = argv[++i];
        } else if (a == "--hold") {
            if (!need("--hold")) return false;
            cli.hold_secs = std::stoi(argv[++i]);
        } else if (a == "-h" || a == "--help") {
            usage(argv[0]);
            std::exit(0);
        } else {
            std::cerr << "Unknown option: " << a << "\n";
            return false;
        }
    }
    if (cli.role != "ue" && cli.role != "gnb") {
        std::cerr << "ERROR: --role must be 'ue' or 'gnb'\n";
        return false;
    }
    return true;
}

int main(int argc, char** argv) {
    std::cout << "UHD version: " << uhd::get_version_string() << "\n";
    std::cout << "gRPC/protobuf generated headers included OK.\n";

    Cli cli;
    if (!parse_cli(argc, argv, cli)) {
        usage(argv[0]);
        return 64;
    }

    // 2) Load YAML
    flexsdr::FullConfig cfg;

    try {
        cfg = flexsdr::load_full_config(cli.cfg);
    } catch (const std::exception& e) {
        std::cerr << "[YAML] load failed: " << e.what() << "\n";
        return 1;
    }

    // 3) Init EAL explicitly as SECONDARY
    if (flexsdr::init_eal_from_config(cfg.eal, /*secondary=*/true, {}) < 0) {
        std::cerr << "[EAL] init (secondary) failed: " << rte_strerror(rte_errno) << "\n";
        return 2;
    }

    // 4) Attach to primary-owned rings/pools based on role
    flexsdr::Handles h;
    std::string err;
    if (cli.role == "ue") {
        if (attach_secondary_ue(cfg.ue_app, cfg.defaults, h, err) != 0) {
            std::cerr << "[UE] attach failed: " << err << "\n";
            return 3;
        }
        std::cout << "[UE] attached: rings=" << h.rings.size() << " pools=" << h.pools.size() << "\n";
    } else {
        if (attach_secondary_gnb(cfg.gnb_app, cfg.defaults, h, err) != 0) {
            std::cerr << "[gNB] attach failed: " << err << "\n";
            return 4;
        }
        std::cout << "[gNB] attached: rings=" << h.rings.size() << " pools=" << h.pools.size() << "\n";
    }

    auto ctx = std::make_shared<flexsdr::DpdkContext>();
    if (cli.role == "ue") {
        ctx->ue_inbound_ring_name = cfg.ue_app.inbound_ring;
        ctx->ue_tx_ring0_name     = cfg.ue_app.tx_stream.rings.front();
        ctx->ue_pool_name         = cfg.ue_app.rx_pool;     // use rx_pool from YAML config

        // pass pointers from Handles (non-owning). It’s OK if some are null; device will resolve by name.
        ctx->ue_in  = h.rings.count(ctx->ue_inbound_ring_name) ? h.rings.at(ctx->ue_inbound_ring_name) : nullptr;
        ctx->ue_tx0 = h.rings.count(ctx->ue_tx_ring0_name)     ? h.rings.at(ctx->ue_tx_ring0_name)     : nullptr;
        ctx->ue_mp  = h.pools.count(ctx->ue_pool_name)         ? h.pools.at(ctx->ue_pool_name)         : nullptr;
    } else { // gnb
        ctx->gnb_inbound_ring_name = cfg.gnb_app.inbound_ring;
        ctx->gnb_tx_ring0_name     = cfg.gnb_app.tx_stream.rings.front();
        ctx->gnb_pool_name         = cfg.gnb_app.rx_pool;     // use rx_pool from YAML config

        ctx->gnb_in  = h.rings.count(ctx->gnb_inbound_ring_name) ? h.rings.at(ctx->gnb_inbound_ring_name) : nullptr;
        ctx->gnb_tx0 = h.rings.count(ctx->gnb_tx_ring0_name)     ? h.rings.at(ctx->gnb_tx_ring0_name)     : nullptr;
        ctx->gnb_mp  = h.pools.count(ctx->gnb_pool_name)         ? h.pools.at(ctx->gnb_pool_name)         : nullptr;
    }

    flexsdr_register_with_uhd();

    // 5)  UHD bring-up using uhd::device::make 
    try {
        // Discovery sanity 
        uhd::device_addr_t hint; hint.set("type", "flexsdr");
        auto devs = uhd::device::find(hint);
        std::cout << "[DISCOVERY] found " << devs.size() << " candidate(s)\n";
        for (const auto& a : devs) dump_addr(a);

        std::cout << "[UHD] Creating device with args: " << cli.args << "\n";
        auto base  = uhd::device::make(uhd::device_addr_t(cli.args)); 
        // downcast to your concrete device and attach context
        auto fdev = std::dynamic_pointer_cast<flexsdr::flexsdr_device>(base);
        if (!fdev) {
            std::cerr << "[ERROR] underlying device is not flexsdr_device\n";
            return 6;
        }
        fdev->attach_dpdk_context(ctx, cli.role == "ue" ? flexsdr::Role::UE : flexsdr::Role::GNB);

        std::cout << "[UHD] device OK.\n";


        // ===== OAI-like RX consumer (4 channels) =====
        uhd::stream_args_t sargs("sc16", "sc16");
        sargs.channels = {0,1,2,3};
        auto rx = fdev->get_rx_stream(sargs);
        auto tx = fdev->get_tx_stream(sargs);

        // lcores from YAML (examples)
        unsigned rx_lcore = cfg.ue_app.rx_cores.empty() ? 2 : cfg.ue_app.rx_cores.front();
        unsigned tx_lcore = cfg.ue_app.tx_cores.empty() ? 3 : cfg.ue_app.tx_cores.front();

        flexsdr::start_rx_worker(rx, rx_lcore, /*nchan=*/4, /*nsamps_per_call=*/4096);
        flexsdr::start_tx_worker(tx, tx_lcore, /*nchan=*/1, /*nsamps_per_call=*/4096);
    } catch (const std::exception& e) {
        std::cerr << "[UHD] bring-up skipped/failed: " << e.what() << "\n";
        // Not fatal for DPDK attach; continue if you prefer
    }

    // 6) Optional: keep this secondary alive for N seconds so you can interact
    if (cli.hold_secs > 0) {
        std::cout << "[HOLD] role=" << cli.role << " | sleeping " << cli.hold_secs << "s (Ctrl+C to kill)\n";
        for (int i = 0; i < cli.hold_secs; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    std::cout << "[DONE] secondary(" << cli.role << ") init complete.\n";
    return 0;
}
