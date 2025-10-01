#include "transport/flexsdr_primary.hpp"
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <rte_errno.h>
#include <rte_mempool.h>
#include <rte_ring.h>
#include <rte_mbuf.h>

using std::string;
namespace flexsdr {

// ---------------- helpers ----------------

static inline bool bad_name(const std::string& s) {
  return s.empty();
}

int FlexSDRPrimary::create_ring_(const string& name, unsigned size, rte_ring** out) {
  if (bad_name(name)) {
    std::fprintf(stderr, "[ring] ERROR: empty name for create\n");
    return -EINVAL;
  }
  rte_errno = 0;
  rte_ring* r = rte_ring_create(name.c_str(), size, rte_socket_id(), RING_F_SC_DEQ);
  if (!r) {
    int err = rte_errno;
    if (err == EEXIST) {
      // creator should be idempotent—if exists already, just re-use
      r = rte_ring_lookup(name.c_str());
      if (!r) {
        std::fprintf(stderr, "[ring] create(%s) EEXIST then lookup failed: rte_errno=%d\n",
                     name.c_str(), rte_errno);
        return -rte_errno ? -rte_errno : -EIO;
      }
    } else {
      std::fprintf(stderr, "[ring] create(%s,size=%u) failed: rte_errno=%d\n",
                   name.c_str(), size, err);
      return -err ? -err : -EIO;
    }
  }
  *out = r;
  return 0;
}

int FlexSDRPrimary::lookup_ring_(const string& name, rte_ring** out) {
  if (bad_name(name)) {
    std::fprintf(stderr, "[ring] ERROR: empty name for lookup\n");
    return -EINVAL;
  }
  rte_errno = 0;
  rte_ring* r = rte_ring_lookup(name.c_str());
  if (!r) {
    std::fprintf(stderr, "[ring] lookup(%s) failed: rte_errno=%d\n",
                 name.c_str(), rte_errno);
    return -ENOENT;
  }
  *out = r;
  return 0;
}

int FlexSDRPrimary::lookup_pool_(const string& name, rte_mempool** out) {
  if (bad_name(name)) {
    std::fprintf(stderr, "[pool] ERROR: empty name for lookup\n");
    return -EINVAL;
  }
  rte_errno = 0;
  rte_mempool* mp = rte_mempool_lookup(name.c_str());
  if (!mp) {
    std::fprintf(stderr, "[pool] lookup(%s) failed: rte_errno=%d\n",
                 name.c_str(), rte_errno);
    return -ENOENT;
  }
  *out = mp;
  return 0;
}

// ---------------- lifecycle ----------------

FlexSDRPrimary::FlexSDRPrimary(std::string yaml_path)
: yaml_path_(std::move(yaml_path)) {}

int FlexSDRPrimary::load_config_() {
  int rc = conf::load_from_yaml(yaml_path_.c_str(), cfg_);
  if (rc) {
    std::fprintf(stderr, "[cfg] load_from_yaml(%s) failed rc=%d\n",
                 yaml_path_.c_str(), rc);
  }
  return rc;
}

int FlexSDRPrimary::init_resources() {
  if (int rc = load_config_(); rc) return rc;

  std::fprintf(stderr, "[primary] init_resources: ring_size=%u\n",
               cfg_.defaults.ring_size);

  // --- Always CREATE our local rings/pools from primary-ue ---
  if (!cfg_.primary_ue) {
    std::fprintf(stderr, "[primary] ERROR: missing 'primary-ue' section\n");
    return -EINVAL;
  }

  if (int rc = create_pools_(); rc) return rc;
  if (int rc = create_rings_tx_(); rc) return rc;
  if (int rc = create_rings_rx_(); rc) return rc;

  std::fprintf(stderr, "[primary] resources ready\n");
  return 0;
}

// ---------------- creators ----------------

int FlexSDRPrimary::create_pools_() {
  const auto& rcfg = *cfg_.primary_ue;
  for (const auto& p : rcfg.pools) {
    if (bad_name(p.name)) {
      std::fprintf(stderr, "[pool] ERROR: empty pool name in config\n");
      return -EINVAL;
    }
    // create mempool; layout: n= p.size, elt= p.elt_size, cache = cfg_.defaults.mp_cache (if you keep it)
    rte_errno = 0;
    rte_mempool* mp = rte_pktmbuf_pool_create(
        p.name.c_str(), p.size, cfg_.defaults.mp_cache, 0, p.elt_size, rte_socket_id());
    if (!mp) {
      int err = rte_errno;
      if (err == EEXIST) {
        // reuse
        mp = rte_mempool_lookup(p.name.c_str());
        if (!mp) {
          std::fprintf(stderr, "[pool] EEXIST then lookup failed: %s rte_errno=%d\n",
                       p.name.c_str(), rte_errno);
          return -rte_errno ? -rte_errno : -EIO;
        }
      } else {
        std::fprintf(stderr, "[pool] create(%s) failed: rte_errno=%d\n",
                     p.name.c_str(), err);
        return -err ? -err : -EIO;
      }
    }
    pools_.push_back(mp);
    std::fprintf(stderr, "[pool] created: %s (n=%u elt=%u)\n",
                 p.name.c_str(), p.size, p.elt_size);
  }
  return 0;
}

static inline const std::vector<conf::RingSpec>& pick_tx(const conf::PrimaryConfig& cfg) {
  if (cfg.primary_ue && cfg.primary_ue->tx_stream && !cfg.primary_ue->tx_stream->rings.empty())
    return cfg.primary_ue->tx_stream->rings;
  return cfg.defaults.tx_stream.rings;
}
static inline const std::vector<conf::RingSpec>& pick_rx(const conf::PrimaryConfig& cfg) {
  if (cfg.primary_ue && cfg.primary_ue->rx_stream && !cfg.primary_ue->rx_stream->rings.empty())
    return cfg.primary_ue->rx_stream->rings;
  return cfg.defaults.rx_stream.rings;
}

int FlexSDRPrimary::create_rings_tx_() {
  const auto& rings = pick_tx(cfg_);
  for (const auto& r : rings) {
    rte_ring* ptr = nullptr;
    if (int rc = create_ring_(r.name, r.size ? r.size : cfg_.defaults.ring_size, &ptr); rc) return rc;
    tx_rings_.push_back(ptr);
    std::fprintf(stderr, "[ring] created tx: %s\n", r.name.c_str());
  }
  return 0;
}

int FlexSDRPrimary::create_rings_rx_() {
  const auto& rings = pick_rx(cfg_);
  for (const auto& r : rings) {
    rte_ring* ptr = nullptr;
    if (int rc = create_ring_(r.name, r.size ? r.size : cfg_.defaults.ring_size, &ptr); rc) return rc;
    rx_rings_.push_back(ptr);
    std::fprintf(stderr, "[ring] created rx: %s\n", r.name.c_str());
  }
  return 0;
}

int FlexSDRSecondary::lookup_interconnect_() {
  // defaults.interconnect is a value (not optional) in the current schema.
  const auto& ic = cfg_.defaults.interconnect;

  if (ic.rings.empty()) {
    std::fprintf(stderr, "[secondary] no interconnect rings configured in defaults\n");
    return 0; // not an error
  }

  std::fprintf(stderr, "[secondary] looking up interconnect rings from defaults…\n");
  for (const auto& r : ic.rings) {
    rte_ring* ptr = nullptr;
    int rc = lookup_ring_(r.name, &ptr);
    if (rc) {
      std::fprintf(stderr, "[secondary] interconnect ring lookup failed: %s (rc=%d rte_errno=%d)\n",
                   r.name.c_str(), rc, rte_errno);
      return rc;
    }
    std::fprintf(stderr, "[secondary] interconnect ring ok: %s (size=%u)\n",
                 r.name.c_str(), rte_ring_get_size(ptr));

    // If you want to expose them to the dumper, choose where to store them.
    // For now, we’ll just attach them to rx_rings_ (or tx_rings_) as needed.
    // If you want to keep them separate, add a dedicated container in the class.
    rx_rings_.push_back(ptr);
  }
  return 0;
}

} // namespace flexsdr
