/** @fileoverview Opt-in, low-overhead launch latency tracing. */
#include "startup-timing.hpp"

#include <QByteArray>

#include <cstdio>
#include <mutex>

namespace {
using Clock = std::chrono::steady_clock;

const Clock::time_point &processEntryTime() {
  static const Clock::time_point started = Clock::now();
  return started;
}

std::mutex &outputMutex() {
  static std::mutex mutex;
  return mutex;
}

double milliseconds(Clock::duration duration) {
  return std::chrono::duration<double, std::milli>(duration).count();
}

void printTiming(const char *kind, const char *label, Clock::time_point now,
                 Clock::duration duration) {
  std::lock_guard lock(outputMutex());
  std::fprintf(stderr, "OMASNAP_STARTUP +%9.3f ms  %s %-36s %9.3f ms\n",
               milliseconds(now - processEntryTime()), kind, label,
               milliseconds(duration));
  std::fflush(stderr);
}
} // namespace

bool startupTimingEnabled() {
  static const bool enabled = [] {
    const QByteArray value = qgetenv("OMASNAP_PROFILE_STARTUP");
    return !value.isEmpty() && value != "0" && value.toLower() != "false";
  }();
  return enabled;
}

void startupTimingMark(const char *label) {
  if (!startupTimingEnabled())
    return;
  static_cast<void>(processEntryTime());
  const Clock::time_point now = Clock::now();
  printTiming("MARK", label, now, Clock::duration::zero());
}

StartupTimingScope::StartupTimingScope(const char *label)
    : label_(label), enabled_(startupTimingEnabled()) {
  if (enabled_) {
    static_cast<void>(processEntryTime());
    started_ = Clock::now();
  }
}

StartupTimingScope::~StartupTimingScope() {
  if (!enabled_)
    return;
  const Clock::time_point now = Clock::now();
  printTiming("DONE", label_, now, now - started_);
}
