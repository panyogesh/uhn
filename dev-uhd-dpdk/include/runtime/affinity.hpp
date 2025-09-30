#pragma once
#include <cstdint>

namespace flexsdr {
// Pin current thread to a DPDK lcore (returns false on failure)
bool pin_current_thread_to_lcore(unsigned lcore_id);

// Optional: resolve lcore→cpu and print a friendly line
//int  lcore_to_cpu(unsigned lcore_id);   // -1 on error
void log_pin(const char* tag, unsigned lcore_id);
} // namespace flexsdr
