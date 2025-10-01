#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <stdexcept>
#include <ostream>

namespace flexsdr::conf {

enum class DataFormat { cs16, cf32 };
enum class LayoutMode { planar, interleaved };

struct Eal {
  std::string file_prefix = "flexsdr-app";
  std::string huge_dir    = "/dev/hugepages";
  std::string socket_mem  = "1024,1024";
  bool        no_pci      = true;
  std::string iova        = "va";

  // Optional RT/NUMA extras (will remain std::nullopt if not set)
  std::optional<int>         main_lcore;
  std::optional<std::string> lcores;
  std::optional<std::string> isolcpus;
  std::optional<bool>        numa;
  std::optional<std::string> socket_limit;
};

struct Stream {
  // One or more ring names to use at this stage (e.g., ["rx0"], ["tx0"])
  std::vector<std::string> ring;

  // Layout / batching
  LayoutMode  mode          = LayoutMode::planar;
  std::size_t spp           = 64;        // samples per packet
  unsigned    num_channels  = 2;
  bool        allow_partial = true;
  uint32_t    timeout_us    = 100;       // dequeue timeout
  bool        busy_poll     = true;

  // Optional watermarks (percent, integer 0..100), if set in YAML
  std::optional<unsigned> high_watermark_pct; // start shedding work
  std::optional<unsigned> hard_drop_pct;      // drop aggressively
};

struct Defaults {
  // pools / rings
  unsigned    nb_mbuf    = 8192;
  unsigned    mp_cache   = 256;
  unsigned    ring_size  = 512;     // default ring size to create
  DataFormat  data_format = DataFormat::cs16;
  unsigned    num_channels = 2;

  // Provide default stream templates; per-app can override any field
  Stream rx_stream{};
  Stream tx_stream{};
};

// Per-app override container; any field present here overrides Defaults
struct AppSection {
  std::optional<Stream>   rx_stream;
  std::optional<Stream>   tx_stream;
  // Future: mempool overrides, pacing, etc.
};

struct Config {
  Eal        eal{};
  Defaults   defaults{};
  std::optional<AppSection> primary;
  std::optional<AppSection> ue;
  std::optional<AppSection> gnb;

  // Load from YAML path (throws std::runtime_error on fatal schema errors)
  static Config load(const std::string& yaml_path);

  // Convenience: get effective RX/TX stream for a given app ("primary","ue","gnb")
  const Stream& rx_stream_for(const std::string& app) const;
  const Stream& tx_stream_for(const std::string& app) const;

  // Shorthands
  const Stream& primary_rx() const { return rx_stream_for("primary"); }
  const Stream& primary_tx() const { return tx_stream_for("primary"); }
  const Stream& ue_rx() const      { return rx_stream_for("ue"); }
  const Stream& ue_tx() const      { return tx_stream_for("ue"); }
  const Stream& gnb_rx() const     { return rx_stream_for("gnb"); }
  const Stream& gnb_tx() const     { return tx_stream_for("gnb"); }
};

// Pretty-printers (handy for boot logs)
std::ostream& operator<<(std::ostream&, DataFormat);
std::ostream& operator<<(std::ostream&, LayoutMode);
std::ostream& operator<<(std::ostream&, const Eal&);
std::ostream& operator<<(std::ostream&, const Stream&);
std::ostream& operator<<(std::ostream&, const Defaults&);
std::ostream& operator<<(std::ostream&, const Config&);

} // namespace flexsdr::conf
