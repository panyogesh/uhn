#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <iostream>

#include "conf/config_params.hpp"
#include "eal_bootstrap.hpp"
#include "flexsdr_primary.hpp"
#include "flexsdr_secondary.hpp"

// print helper
static void print_list(const char* tag, const std::vector<std::string>& xs) {
  std::printf("%s (%zu):\n", tag, xs.size());
  for (const auto& s : xs) std::printf("  - %s\n", s.c_str());
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
      "usage: %s <conf YAML> [-- <extra EAL flags>]\n"
      "examples:\n"
      "  %s conf/configuration-ue.yaml -- --vdev=net_null0\n"
      "  %s conf/configuration-gnb.yaml -- --vdev=net_null0\n",
      argv[0], argv[0], argv[0]);
    return 2;
  }

  // 0) split extra flags after "--"
  std::string yaml_path = argv[1];
  std::vector<std::string> extra_flags;
  for (int i = 2; i < argc; ++i) {
    if (std::string(argv[i]) == "--") {
      for (int j = i + 1; j < argc; ++j) extra_flags.emplace_back(argv[j]);
      break;
    }
  }

  // 1) load config
  flexsdr::conf::PrimaryConfig cfg;
  try {
    cfg = flexsdr::conf::PrimaryConfig::load(yaml_path);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "config load failed: %s\n", e.what());
    return 1;
  }

  std::printf("[test] role = %s\n",
              flexsdr::conf::PrimaryConfig::to_string(cfg.defaults.role).c_str());

  // 2) EAL init from config
  flexsdr::EalBootstrap eal(cfg, "test_dpdk_infra");
  eal.build_args(extra_flags);
  if (eal.init() < 0) {
    std::fprintf(stderr, "EAL init failed\n");
    return 1;
  }

  // 3) run infra based on role
  int rc = 0;
  switch (cfg.defaults.role) {
    case flexsdr::conf::Role::primary_ue:
    case flexsdr::conf::Role::primary_gnb: {
      std::printf("[test] primary path\n");
      flexsdr::FlexSDRPrimary app(yaml_path);
      app.load_config();
      rc = app.init_resources();
      if (rc < 0) {
        std::fprintf(stderr, "primary init_resources failed: %d\n", rc);
        return 1;
      }
      print_list("[created/found] pools", app.created_or_found_pools());
      print_list("[created/found] rings", app.created_or_found_rings());
      break;
    }
    case flexsdr::conf::Role::ue:
    case flexsdr::conf::Role::gnb: {
      std::printf("[test] secondary path\n");
      flexsdr::FlexSDRSecondary app(yaml_path);
      app.load_config();
      rc = app.init_resources();
      if (rc < 0) {
        std::fprintf(stderr, "secondary init_resources failed: %d\n", rc);
        return 1;
      }
      print_list("[found] pools", app.found_pools());
      print_list("[found] rings", app.found_rings());
      break;
    }
  }

  std::printf("[test] OK\n");
  return 0;
}
