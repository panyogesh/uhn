#include "runtime/affinity.hpp"

extern "C" {
#include <rte_lcore.h>
#include <rte_version.h>
}

#include <pthread.h>
#include <sched.h>
#include <cstdio>

namespace flexsdr {

static inline int lcore_to_cpu(unsigned lcore_id) {
  int cpu = rte_lcore_to_cpu_id(lcore_id);
  return (cpu < 0) ? -1 : cpu;
}

void log_pin(const char* tag, unsigned lcore_id) {
  int cpu = lcore_to_cpu(lcore_id);
  if (cpu >= 0) std::printf("[PIN] %s pinned: lcore=%u cpu=%d\n", tag, lcore_id, cpu);
  else          std::printf("[PIN] %s: invalid lcore=%u\n", tag, lcore_id);
}

// Use pthread affinity for app-created threads (safe in multi-process)
bool pin_current_thread_to_lcore(unsigned lcore_id) {
  int cpu = lcore_to_cpu(lcore_id);
  if (cpu < 0) return false;

  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET((unsigned)cpu, &set);
  int rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
  return rc == 0;
}

} // namespace flexsdr
