#pragma once
#include <chrono>
namespace lt2 {
using PerfClock = std::chrono::steady_clock;
struct Performance {
  double adapter = 0, draw = 0, wait = 0, present = 0;
  unsigned long long vertices = 0, uploadBytes = 0, descriptorSets = 0;
  unsigned long long directDraws = 0, rawDraws = 0, residentDraws = 0;
};
inline Performance performance;
struct PerfScope {
  double &total;
  PerfClock::time_point start = PerfClock::now();
  explicit PerfScope(double &value) : total(value) {}
  ~PerfScope() { total += std::chrono::duration<double, std::milli>(PerfClock::now() - start).count(); }
};
}
