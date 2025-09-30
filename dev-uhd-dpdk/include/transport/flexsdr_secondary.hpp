#pragma once
#include <string>
#include "config_params.hpp"
#include "dpdk_common.hpp"

namespace flexsdr {

/** Generic attach/validate for a secondary app (UE or gNB). */
int attach_secondary_app(const AppConfig& app,
                         const DefaultConfig& defaults,
                         const char* app_name,
                         Handles& out,
                         std::string& error_out);

/** UE-specialized wrapper. */
inline int attach_secondary_ue(const AppConfig& ue,
                               const DefaultConfig& defaults,
                               Handles& out,
                               std::string& error_out) {
  return attach_secondary_app(ue, defaults, "ue-app", out, error_out);
}

/** gNB-specialized wrapper. */
inline int attach_secondary_gnb(const AppConfig& gnb,
                                const DefaultConfig& defaults,
                                Handles& out,
                                std::string& error_out) {
  return attach_secondary_app(gnb, defaults, "gnb-app", out, error_out);
}

} // namespace flexsdr
