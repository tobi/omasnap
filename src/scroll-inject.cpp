/** @fileoverview Auto-scroll injection worker (see scroll-inject.hpp). */
#include "scroll-inject.hpp"

#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#include <QDebug>
#include <QProcess>

#include <wayland-client.h>

#include <linux/input-event-codes.h>
#include <linux/uinput.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {
using stitch::Axis;
using stitch::CaptureHandshake;

/// Time between injecting a wheel group and announcing its rendered frame:
/// at 150 ms a 60 Hz client gets ~9 frames to paint while the handshake still
/// prevents the next wheel event racing the screenshot.
constexpr int kScrollSettleMs = 150;
constexpr int kInputRegionSettleMs = 50;
constexpr int kUinputDeviceSettleMs = 150;
constexpr int kPointerNudgeSettleMs = 20;
/// One logical wheel notch for the wlr virtual pointer.
constexpr double kNotchValue = 10.0;

void sleepMs(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

/// Sleeps in short slices so a stop request interrupts the settle.
bool sleepUnlessStopped(int ms, const std::atomic<bool> &stop) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (stop.load(std::memory_order_acquire))
      return false;
    sleepMs(5);
  }
  return !stop.load(std::memory_order_acquire);
}

/// The compositor applies the user's natural-scroll policy to a real kernel
/// mouse, so uinput injection must pre-compensate, and is only safe when the
/// policy is actually known.
std::optional<bool> hyprlandNaturalScroll() {
  if (qEnvironmentVariable("HYPRLAND_INSTANCE_SIGNATURE").isEmpty())
    return std::nullopt;
  QProcess process;
  process.start(QStringLiteral("hyprctl"),
                {QStringLiteral("getoption"), QStringLiteral("input:natural_scroll"),
                 QStringLiteral("-j")});
  if (!process.waitForFinished(2000) || process.exitCode() != 0)
    return std::nullopt;
  const QByteArray out = process.readAllStandardOutput();
  const int key = out.indexOf("\"bool\"");
  if (key < 0)
    return std::nullopt;
  const int colon = out.indexOf(':', key);
  if (colon < 0)
    return std::nullopt;
  const QByteArray tail = out.mid(colon + 1).trimmed();
  if (tail.startsWith("true"))
    return true;
  if (tail.startsWith("false"))
    return false;
  return std::nullopt;
}

// --- uinput kernel mouse ------------------------------------------------------
class UinputMouse {
public:
  bool open(QString &error) {
    fd_ = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd_ < 0) {
      error = QStringLiteral("could not open /dev/uinput");
      return false;
    }
    ioctl(fd_, UI_SET_EVBIT, EV_REL);
    for (const int axis : {REL_X, REL_Y, REL_WHEEL, REL_HWHEEL})
      ioctl(fd_, UI_SET_RELBIT, axis);
    // libinput requires a button before udev classifies the device as a
    // mouse (so the natural-scroll policy applies); advertised, never
    // emitted.
    ioctl(fd_, UI_SET_EVBIT, EV_KEY);
    ioctl(fd_, UI_SET_KEYBIT, BTN_LEFT);
    uinput_setup setup{};
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x1d6b;
    setup.id.product = 0x0001;
    std::strncpy(setup.name, "Omasnap Auto Scroll", sizeof(setup.name) - 1);
    if (ioctl(fd_, UI_DEV_SETUP, &setup) < 0 || ioctl(fd_, UI_DEV_CREATE) < 0) {
      error = QStringLiteral("uinput device setup failed");
      close();
      return false;
    }
    return true;
  }
  bool nudge() {
    // ±1 px so the compositor re-hit-tests the surface under the pointer.
    if (!emitEvent(EV_REL, REL_X, 1) || !emitEvent(EV_SYN, SYN_REPORT, 0))
      return false;
    sleepMs(kPointerNudgeSettleMs);
    return emitEvent(EV_REL, REL_X, -1) && emitEvent(EV_SYN, SYN_REPORT, 0);
  }
  bool scroll(Axis axis, int notches, bool naturalScroll) {
    // REL_WHEEL follows the physical wheel: negative = down; HWHEEL positive
    // = right. The compositor then applies natural-scroll, so pre-negate.
    const int code = axis == Axis::Vertical ? REL_WHEEL : REL_HWHEEL;
    int amount = axis == Axis::Vertical ? -notches : notches;
    if (naturalScroll)
      amount = -amount;
    return emitEvent(EV_REL, code, amount) && emitEvent(EV_SYN, SYN_REPORT, 0);
  }
  void close() {
    if (fd_ >= 0) {
      ioctl(fd_, UI_DEV_DESTROY);
      ::close(fd_);
      fd_ = -1;
    }
  }
  ~UinputMouse() { close(); }

private:
  bool emitEvent(int type, int code, int value) {
    input_event event{};
    event.type = static_cast<std::uint16_t>(type);
    event.code = static_cast<std::uint16_t>(code);
    event.value = value;
    return write(fd_, &event, sizeof(event)) == sizeof(event);
  }
  int fd_ = -1;
};

// --- wlr virtual pointer ------------------------------------------------------
struct WlrPointer {
  wl_display *display = nullptr;
  wl_registry *registry = nullptr;
  wl_seat *seat = nullptr;
  zwlr_virtual_pointer_manager_v1 *manager = nullptr;
  zwlr_virtual_pointer_v1 *pointer = nullptr;
  struct Output {
    wl_output *handle = nullptr;
    std::string name;
    int width = 0;
    int height = 0;
  };
  std::vector<std::unique_ptr<Output>> outputs;
  Output *target = nullptr;
  std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now();

  std::uint32_t timeMs() const {
    return static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
  }
  void park(int x, int y) {
    if (!pointer || !target)
      return;
    const auto width = static_cast<std::uint32_t>(std::max(1, target->width));
    const auto height = static_cast<std::uint32_t>(std::max(1, target->height));
    zwlr_virtual_pointer_v1_motion_absolute(
        pointer, timeMs(),
        static_cast<std::uint32_t>(std::clamp(x, 0, target->width)),
        static_cast<std::uint32_t>(std::clamp(y, 0, target->height)), width,
        height);
    zwlr_virtual_pointer_v1_frame(pointer);
    wl_display_flush(display);
    sleepMs(kPointerNudgeSettleMs);
    zwlr_virtual_pointer_v1_motion(pointer, timeMs(), wl_fixed_from_int(1), 0);
    zwlr_virtual_pointer_v1_frame(pointer);
    wl_display_flush(display);
    sleepMs(kPointerNudgeSettleMs);
    zwlr_virtual_pointer_v1_motion(pointer, timeMs(), wl_fixed_from_int(-1), 0);
    zwlr_virtual_pointer_v1_frame(pointer);
    wl_display_flush(display);
  }
  void scroll(Axis axis, int notches) {
    if (!pointer)
      return;
    // One aggregate discrete event, axis_source AFTER axis_discrete
    // (Hyprland ties the source to the most recent axis), one frame.
    const std::uint32_t wlAxis = axis == Axis::Vertical ? 0 : 1;
    zwlr_virtual_pointer_v1_axis_discrete(
        pointer, timeMs(), wlAxis, wl_fixed_from_double(kNotchValue * notches),
        notches);
    zwlr_virtual_pointer_v1_axis_source(pointer, 0 /* wheel */);
    zwlr_virtual_pointer_v1_frame(pointer);
    wl_display_flush(display);
  }
  ~WlrPointer() {
    if (pointer)
      zwlr_virtual_pointer_v1_destroy(pointer);
    for (const auto &output : outputs)
      if (output->handle)
        wl_output_release(output->handle);
    if (manager)
      zwlr_virtual_pointer_manager_v1_destroy(manager);
    if (seat)
      wl_seat_destroy(seat);
    if (registry)
      wl_registry_destroy(registry);
    if (display) {
      wl_display_flush(display);
      wl_display_disconnect(display);
    }
  }
};

void injectOutputName(void *data, wl_output *, const char *name) {
  static_cast<WlrPointer::Output *>(data)->name = name;
}
void injectOutputMode(void *data, wl_output *, std::uint32_t flags, int32_t w,
                      int32_t h, int32_t) {
  if (flags & WL_OUTPUT_MODE_CURRENT) {
    auto *output = static_cast<WlrPointer::Output *>(data);
    output->width = w;
    output->height = h;
  }
}
void injectOutputGeometry(void *, wl_output *, int32_t, int32_t, int32_t,
                          int32_t, int32_t, const char *, const char *,
                          int32_t) {}
void injectOutputDone(void *, wl_output *) {}
void injectOutputScale(void *, wl_output *, int32_t) {}
void injectOutputDescription(void *, wl_output *, const char *) {}
constexpr wl_output_listener kInjectOutputListener{
    injectOutputGeometry, injectOutputMode,  injectOutputDone,
    injectOutputScale,    injectOutputName,  injectOutputDescription};

void injectRegistryGlobal(void *data, wl_registry *registry, std::uint32_t name,
                          const char *interface, std::uint32_t version) {
  auto &state = *static_cast<WlrPointer *>(data);
  if (std::strcmp(interface, wl_seat_interface.name) == 0 && !state.seat) {
    state.seat = static_cast<wl_seat *>(wl_registry_bind(
        registry, name, &wl_seat_interface, std::min(version, 8u)));
  } else if (std::strcmp(interface, wl_output_interface.name) == 0 &&
             version >= 4) {
    auto output = std::make_unique<WlrPointer::Output>();
    output->handle = static_cast<wl_output *>(
        wl_registry_bind(registry, name, &wl_output_interface, 4));
    wl_output_add_listener(output->handle, &kInjectOutputListener,
                           output.get());
    state.outputs.push_back(std::move(output));
  } else if (std::strcmp(interface,
                         zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
    state.manager =
        static_cast<zwlr_virtual_pointer_manager_v1 *>(wl_registry_bind(
            registry, name, &zwlr_virtual_pointer_manager_v1_interface,
            std::min(version, 2u)));
  }
}
void injectRegistryRemove(void *, wl_registry *, std::uint32_t) {}
constexpr wl_registry_listener kInjectRegistryListener{injectRegistryGlobal,
                                                       injectRegistryRemove};

std::unique_ptr<WlrPointer> connectWlrPointer(const QString &outputName,
                                              QString &error) {
  auto state = std::make_unique<WlrPointer>();
  state->display = wl_display_connect(nullptr);
  if (!state->display) {
    error = QStringLiteral("could not connect to Wayland for injection");
    return nullptr;
  }
  state->registry = wl_display_get_registry(state->display);
  wl_registry_add_listener(state->registry, &kInjectRegistryListener,
                           state.get());
  wl_display_roundtrip(state->display); // globals
  wl_display_roundtrip(state->display); // output names/modes
  if (!state->manager) {
    error = QStringLiteral("compositor does not expose the virtual pointer");
    return nullptr;
  }
  const std::string wanted = outputName.toStdString();
  for (const auto &output : state->outputs) {
    if (wanted.empty() || output->name == wanted) {
      state->target = output.get();
      break;
    }
  }
  if (!state->target) {
    error = QStringLiteral("output %1 not found for injection").arg(outputName);
    return nullptr;
  }
  // motion_absolute coordinates are physical pixels of the bound output; an
  // unbound pointer maps across the whole layout and misses on multi-monitor.
  if (zwlr_virtual_pointer_manager_v1_get_version(state->manager) >= 2) {
    state->pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(
        state->manager, state->seat, state->target->handle);
  } else {
    qInfo().noquote() << QStringLiteral(
        "scroll-inject: virtual pointer v1 only; warps map to the whole layout");
    state->pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(
        state->manager, state->seat);
  }
  wl_display_roundtrip(state->display);
  if (!state->pointer) {
    error = QStringLiteral("could not create the virtual pointer");
    return nullptr;
  }
  return state;
}

/// One tick per acknowledged cycle, forever until stopped.
void runCaptureScrollLoop(const std::shared_ptr<std::atomic<bool>> &stop,
                          const std::shared_ptr<CaptureHandshake> &handshake,
                          const std::function<bool(int)> &inject) {
  std::uint64_t cycle = handshake->publishReady(); // cycle 1: unscrolled frame
  while (true) {
    const std::optional<int> notches = handshake->waitForCapture(cycle, *stop);
    if (!notches)
      break;
    if (stop->load(std::memory_order_acquire))
      break;
    // Parked once at the start and left alone after that. Re-parking every tick
    // kept the wheel on the page, but it also meant the pointer could never be
    // taken anywhere, and moving it away is how a person stops an auto scroll.
    // Left alone, moving out of the region sends the injected wheel somewhere
    // harmless, the scrolling stops, and the buttons and keys are reachable
    // again.
    if (!inject(*notches))
      break;
    if (!sleepUnlessStopped(kScrollSettleMs, *stop))
      break;
    cycle = handshake->publishReady();
  }
}
} // namespace

bool spawnScrollInjector(std::shared_ptr<std::atomic<bool>> stop,
                         std::shared_ptr<stitch::CaptureHandshake> handshake,
                         int parkX, int parkY, stitch::Axis axis,
                         const QString &outputName, QString &error) {
  // Validate the backends synchronously so the caller gets a useful error;
  // the injection itself runs off the UI thread.
  const std::optional<bool> naturalScroll = hyprlandNaturalScroll();
  auto uinput = std::make_shared<UinputMouse>();
  bool haveUinput = false;
  if (naturalScroll) {
    QString uinputError;
    haveUinput = uinput->open(uinputError);
    if (!haveUinput)
      qInfo().noquote()
          << QStringLiteral("scroll-inject: uinput unavailable (%1); using the "
                            "virtual pointer")
                 .arg(uinputError);
  } else {
    qInfo().noquote() << QStringLiteral(
        "scroll-inject: natural-scroll policy unknown; using the virtual "
        "pointer");
  }
  // The wlr pointer parks the cursor for both backends (and scrolls when
  // uinput is unavailable).
  QString wlrError;
  std::shared_ptr<WlrPointer> wlr = connectWlrPointer(outputName, wlrError);
  if (!wlr && !haveUinput) {
    error = wlrError;
    return false;
  }
  if (!wlr)
    qInfo().noquote() << QStringLiteral(
                             "scroll-inject: no virtual pointer (%1); cannot "
                             "park · scrolling wherever the cursor is")
                             .arg(wlrError);
  qInfo().noquote() << QStringLiteral(
                           "scroll-inject: backend=%1 natural=%2 park=%3,%4 on %5")
                           .arg(haveUinput ? QStringLiteral("uinput")
                                           : QStringLiteral("wlr-pointer"))
                           .arg(naturalScroll ? (*naturalScroll ? "true" : "false")
                                              : "unknown")
                           .arg(parkX)
                           .arg(parkY)
                           .arg(outputName);

  std::thread([stop, handshake, uinput, haveUinput, wlr, parkX, parkY, axis,
               natural = naturalScroll.value_or(false)] {
    // Let the overlay's input-region commit land, then settle the device and
    // park inside the selection so wheel events hit the page.
    sleepMs(kInputRegionSettleMs);
    if (haveUinput)
      sleepMs(kUinputDeviceSettleMs);
    if (wlr)
      wlr->park(parkX, parkY);
    if (haveUinput && !uinput->nudge()) {
      qWarning().noquote()
          << QStringLiteral("scroll-inject: pointer nudge failed");
      stop->store(true, std::memory_order_release);
      return;
    }
    const std::function<bool(int)> inject = [&](int notches) {
      qInfo().noquote() << QStringLiteral("scroll-inject: tick %1 notch%2")
                               .arg(notches)
                               .arg(notches == 1 ? QString() : QStringLiteral("es"));
      if (haveUinput)
        return uinput->scroll(axis, notches, natural);
      if (wlr) {
        wlr->scroll(axis, notches);
        return true;
      }
      return false;
    };
    runCaptureScrollLoop(stop, handshake, inject);
    // Worker death is always observable through the one flag.
    stop->store(true, std::memory_order_release);
    qInfo().noquote() << QStringLiteral("scroll-inject: worker exited");
  }).detach();
  return true;
}
