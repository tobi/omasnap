#pragma once

#include <chrono>

/**
 * Opt-in startup tracing for launch-to-overlay profiling. Set
 * OMASNAP_PROFILE_STARTUP=1 to print cumulative and per-stage timings to
 * stderr. Disabled runs pay only one cached boolean check per trace point.
 */
[[nodiscard]] bool startupTimingEnabled();
void startupTimingMark(const char *label);

class StartupTimingScope final {
public:
  explicit StartupTimingScope(const char *label);
  ~StartupTimingScope();

  StartupTimingScope(const StartupTimingScope &) = delete;
  StartupTimingScope &operator=(const StartupTimingScope &) = delete;

private:
  const char *label_ = nullptr;
  std::chrono::steady_clock::time_point started_{};
  bool enabled_ = false;
};
