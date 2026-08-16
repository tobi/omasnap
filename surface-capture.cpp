/** @fileoverview Captures native window surfaces through Wayland protocols. */
#include "capture.hpp"

#include "kwin.hpp"

#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"

#include <QImage>
#include <QTransform>

#include <wayland-client.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <poll.h>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace {
struct Toplevel {
  ext_foreign_toplevel_handle_v1 *handle = nullptr;
  std::string identifier;
};

struct CaptureState {
  wl_display *display = nullptr;
  wl_registry *registry = nullptr;
  wl_shm *shm = nullptr;
  ext_foreign_toplevel_list_v1 *toplevelList = nullptr;
  ext_foreign_toplevel_image_capture_source_manager_v1 *sourceManager = nullptr;
  ext_image_copy_capture_manager_v1 *captureManager = nullptr;
  ext_image_capture_source_v1 *source = nullptr;
  ext_image_copy_capture_session_v1 *session = nullptr;
  ext_image_copy_capture_frame_v1 *frame = nullptr;
  wl_shm_pool *pool = nullptr;
  wl_buffer *buffer = nullptr;
  std::vector<std::unique_ptr<Toplevel>> toplevels;
  std::vector<uint32_t> shmFormats;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
  uint32_t transform = WL_OUTPUT_TRANSFORM_NORMAL;
  bool constraintsDone = false;
  bool stopped = false;
  bool frameDone = false;
  bool frameReady = false;
  int fd = -1;
  void *memory = MAP_FAILED;
  std::size_t memorySize = 0;

  /** Releases capture protocol objects before disconnecting the display. */
  ~CaptureState() {
    if (frame)
      ext_image_copy_capture_frame_v1_destroy(frame);
    if (session)
      ext_image_copy_capture_session_v1_destroy(session);
    if (source)
      ext_image_capture_source_v1_destroy(source);
    for (const auto &toplevel : toplevels) {
      if (toplevel->handle)
        ext_foreign_toplevel_handle_v1_destroy(toplevel->handle);
    }
    if (toplevelList)
      ext_foreign_toplevel_list_v1_destroy(toplevelList);
    if (sourceManager)
      ext_foreign_toplevel_image_capture_source_manager_v1_destroy(
          sourceManager);
    if (captureManager)
      ext_image_copy_capture_manager_v1_destroy(captureManager);
    if (buffer)
      wl_buffer_destroy(buffer);
    if (pool)
      wl_shm_pool_destroy(pool);
    if (memory != MAP_FAILED)
      munmap(memory, memorySize);
    if (fd >= 0)
      close(fd);
    if (shm)
      wl_shm_destroy(shm);
    if (registry)
      wl_registry_destroy(registry);
    if (display)
      wl_display_disconnect(display);
  }
};

void handleClosed(void *, ext_foreign_toplevel_handle_v1 *) {}
void handleDone(void *, ext_foreign_toplevel_handle_v1 *) {}
void handleTitle(void *, ext_foreign_toplevel_handle_v1 *, const char *) {}
void handleAppId(void *, ext_foreign_toplevel_handle_v1 *, const char *) {}
void handleIdentifier(void *data, ext_foreign_toplevel_handle_v1 *,
                      const char *identifier) {
  static_cast<Toplevel *>(data)->identifier = identifier ? identifier : "";
}

constexpr ext_foreign_toplevel_handle_v1_listener kHandleListener{
    handleClosed, handleDone, handleTitle, handleAppId, handleIdentifier};

void listToplevel(void *data, ext_foreign_toplevel_list_v1 *,
                  ext_foreign_toplevel_handle_v1 *handle) {
  auto &state = *static_cast<CaptureState *>(data);
  auto toplevel = std::make_unique<Toplevel>();
  toplevel->handle = handle;
  ext_foreign_toplevel_handle_v1_add_listener(handle, &kHandleListener,
                                              toplevel.get());
  state.toplevels.push_back(std::move(toplevel));
}
void listFinished(void *, ext_foreign_toplevel_list_v1 *) {}
constexpr ext_foreign_toplevel_list_v1_listener kListListener{listToplevel,
                                                              listFinished};

void registryGlobal(void *data, wl_registry *registry, uint32_t name,
                    const char *interface, uint32_t version) {
  auto &state = *static_cast<CaptureState *>(data);
  if (std::strcmp(interface, wl_shm_interface.name) == 0) {
    state.shm = static_cast<wl_shm *>(wl_registry_bind(
        registry, name, &wl_shm_interface, std::min(version, 1U)));
  } else if (std::strcmp(interface,
                         ext_foreign_toplevel_list_v1_interface.name) == 0) {
    state.toplevelList =
        static_cast<ext_foreign_toplevel_list_v1 *>(wl_registry_bind(
            registry, name, &ext_foreign_toplevel_list_v1_interface,
            std::min(version, 1U)));
    ext_foreign_toplevel_list_v1_add_listener(state.toplevelList,
                                              &kListListener, &state);
  } else if (std::strcmp(
                 interface,
                 ext_foreign_toplevel_image_capture_source_manager_v1_interface
                     .name) == 0) {
    state.sourceManager =
        static_cast<ext_foreign_toplevel_image_capture_source_manager_v1 *>(
            wl_registry_bind(
                registry, name,
                &ext_foreign_toplevel_image_capture_source_manager_v1_interface,
                std::min(version, 1U)));
  } else if (std::strcmp(interface,
                         ext_image_copy_capture_manager_v1_interface.name) ==
             0) {
    state.captureManager =
        static_cast<ext_image_copy_capture_manager_v1 *>(wl_registry_bind(
            registry, name, &ext_image_copy_capture_manager_v1_interface,
            std::min(version, 1U)));
  }
}
void registryRemoved(void *, wl_registry *, uint32_t) {}
constexpr wl_registry_listener kRegistryListener{registryGlobal,
                                                 registryRemoved};

void sessionBufferSize(void *data, ext_image_copy_capture_session_v1 *,
                       uint32_t width, uint32_t height) {
  auto &state = *static_cast<CaptureState *>(data);
  state.width = width;
  state.height = height;
}
void sessionShmFormat(void *data, ext_image_copy_capture_session_v1 *,
                      uint32_t format) {
  static_cast<CaptureState *>(data)->shmFormats.push_back(format);
}
void sessionDmabufDevice(void *, ext_image_copy_capture_session_v1 *,
                         wl_array *) {}
void sessionDmabufFormat(void *, ext_image_copy_capture_session_v1 *, uint32_t,
                         wl_array *) {}
void sessionDone(void *data, ext_image_copy_capture_session_v1 *) {
  static_cast<CaptureState *>(data)->constraintsDone = true;
}
void sessionStopped(void *data, ext_image_copy_capture_session_v1 *) {
  auto &state = *static_cast<CaptureState *>(data);
  state.stopped = true;
  state.constraintsDone = true;
  state.frameDone = true;
}
constexpr ext_image_copy_capture_session_v1_listener kSessionListener{
    sessionBufferSize,   sessionShmFormat, sessionDmabufDevice,
    sessionDmabufFormat, sessionDone,      sessionStopped};

void frameTransform(void *data, ext_image_copy_capture_frame_v1 *,
                    uint32_t transform) {
  static_cast<CaptureState *>(data)->transform = transform;
}
void frameDamage(void *, ext_image_copy_capture_frame_v1 *, int32_t, int32_t,
                 int32_t, int32_t) {}
void framePresentation(void *, ext_image_copy_capture_frame_v1 *, uint32_t,
                       uint32_t, uint32_t) {}
void frameReady(void *data, ext_image_copy_capture_frame_v1 *) {
  auto &state = *static_cast<CaptureState *>(data);
  state.frameReady = true;
  state.frameDone = true;
}
void frameFailed(void *data, ext_image_copy_capture_frame_v1 *, uint32_t) {
  static_cast<CaptureState *>(data)->frameDone = true;
}
constexpr ext_image_copy_capture_frame_v1_listener kFrameListener{
    frameTransform, frameDamage, framePresentation, frameReady, frameFailed};

bool dispatchUntil(CaptureState &state, const bool *done, int timeoutMs,
                   QString &error) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (!*done) {
    if (wl_display_dispatch_pending(state.display) < 0) {
      error = QStringLiteral("Wayland surface capture dispatch failed");
      return false;
    }
    if (*done)
      return true;

    while (wl_display_prepare_read(state.display) != 0) {
      if (wl_display_dispatch_pending(state.display) < 0) {
        error = QStringLiteral("Wayland surface capture dispatch failed");
        return false;
      }
      if (*done)
        return true;
    }
    if (wl_display_flush(state.display) < 0 && errno != EAGAIN) {
      wl_display_cancel_read(state.display);
      error = QStringLiteral("Could not flush Wayland surface capture request");
      return false;
    }

    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) {
      wl_display_cancel_read(state.display);
      error = QStringLiteral("Wayland surface capture timed out");
      return false;
    }
    pollfd descriptor{wl_display_get_fd(state.display), POLLIN, 0};
    const int result =
        poll(&descriptor, 1, static_cast<int>(remaining.count()));
    if (result <= 0 || !(descriptor.revents & POLLIN)) {
      wl_display_cancel_read(state.display);
      error = result == 0
                  ? QStringLiteral("Wayland surface capture timed out")
                  : QStringLiteral("Wayland surface capture connection failed");
      return false;
    }
    if (wl_display_read_events(state.display) < 0) {
      error = QStringLiteral("Could not read Wayland surface capture response");
      return false;
    }
  }
  return true;
}

bool createShmBuffer(CaptureState &state, QString &error) {
  constexpr uint32_t formats[]{WL_SHM_FORMAT_ARGB8888, WL_SHM_FORMAT_XRGB8888,
                               WL_SHM_FORMAT_ABGR8888, WL_SHM_FORMAT_XBGR8888};
  const auto format = std::ranges::find_first_of(formats, state.shmFormats);
  if (format == std::end(formats)) {
    error = QStringLiteral(
        "Compositor offered no supported surface capture format");
    return false;
  }
  state.format = *format;
  if (state.width == 0 || state.height == 0 || state.width > 32768 ||
      state.height > 32768) {
    error = QStringLiteral("Compositor returned an invalid surface size");
    return false;
  }

  const std::size_t stride = static_cast<std::size_t>(state.width) * 4;
  state.memorySize = stride * state.height;
  if (state.memorySize >
      static_cast<std::size_t>(std::numeric_limits<int32_t>::max())) {
    error = QStringLiteral("Surface capture buffer is too large");
    return false;
  }
  char name[96];
  std::snprintf(name, sizeof(name), "/omarchy-capture-%d-%p", getpid(),
                static_cast<void *>(&state));
  state.fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (state.fd < 0) {
    error = QStringLiteral("Could not allocate surface capture memory");
    return false;
  }
  shm_unlink(name);
  if (ftruncate(state.fd, static_cast<off_t>(state.memorySize)) < 0) {
    error = QStringLiteral("Could not size surface capture memory");
    return false;
  }
  state.memory = mmap(nullptr, state.memorySize, PROT_READ | PROT_WRITE,
                      MAP_SHARED, state.fd, 0);
  if (state.memory == MAP_FAILED) {
    error = QStringLiteral("Could not map surface capture memory");
    return false;
  }
  std::memset(state.memory, 0, state.memorySize);

  state.pool = wl_shm_create_pool(state.shm, state.fd,
                                  static_cast<int32_t>(state.memorySize));
  if (!state.pool) {
    error = QStringLiteral("Could not create Wayland surface capture pool");
    return false;
  }
  state.buffer = wl_shm_pool_create_buffer(
      state.pool, 0, static_cast<int32_t>(state.width),
      static_cast<int32_t>(state.height), static_cast<int32_t>(stride),
      state.format);
  if (!state.buffer) {
    error = QStringLiteral("Could not create Wayland surface capture buffer");
    return false;
  }
  return true;
}

QImage copyCapturedImage(const CaptureState &state) {
  QImage::Format imageFormat = QImage::Format_Invalid;
  switch (state.format) {
  case WL_SHM_FORMAT_ARGB8888:
    imageFormat = QImage::Format_ARGB32_Premultiplied;
    break;
  case WL_SHM_FORMAT_XRGB8888:
    imageFormat = QImage::Format_RGB32;
    break;
  case WL_SHM_FORMAT_ABGR8888:
    imageFormat = QImage::Format_RGBA8888_Premultiplied;
    break;
  case WL_SHM_FORMAT_XBGR8888:
    imageFormat = QImage::Format_RGBX8888;
    break;
  default:
    return {};
  }
  const int stride = static_cast<int>(state.width * 4);
  return QImage(static_cast<uchar *>(state.memory),
                static_cast<int>(state.width), static_cast<int>(state.height),
                stride, imageFormat)
      .copy();
}
} // namespace

QImage normalizeWaylandCapture(const QImage &image, std::uint32_t transform) {
  const auto rotated = [&image](qreal degrees) {
    return image.transformed(QTransform().rotate(degrees));
  };
  const auto flipped = [](const QImage &source) {
    return source.transformed(QTransform().scale(-1, 1));
  };
  switch (transform) {
  case WL_OUTPUT_TRANSFORM_NORMAL:
    return image;
  case WL_OUTPUT_TRANSFORM_90:
    return rotated(90);
  case WL_OUTPUT_TRANSFORM_180:
    return rotated(180);
  case WL_OUTPUT_TRANSFORM_270:
    return rotated(-90);
  case WL_OUTPUT_TRANSFORM_FLIPPED:
    return flipped(image);
  case WL_OUTPUT_TRANSFORM_FLIPPED_90:
    return flipped(rotated(90));
  case WL_OUTPUT_TRANSFORM_FLIPPED_180:
    return flipped(rotated(180));
  case WL_OUTPUT_TRANSFORM_FLIPPED_270:
    return flipped(rotated(-90));
  default:
    return {};
  }
}

bool captureWindowSurface(const WindowTarget &window, QImage &image,
                          QString &error) {
  if (window.stableId.isEmpty()) {
    error =
        QStringLiteral("Compositor did not provide a stable window identifier");
    return false;
  }
  if (kwinSession())
    return kwinCaptureWindowSurface(window, image, error);

  CaptureState state;
  state.display = wl_display_connect(nullptr);
  if (!state.display) {
    error = QStringLiteral("Could not connect to Wayland for window capture");
    return false;
  }
  state.registry = wl_display_get_registry(state.display);
  wl_registry_add_listener(state.registry, &kRegistryListener, &state);
  if (wl_display_roundtrip(state.display) < 0 ||
      wl_display_roundtrip(state.display) < 0) {
    error = QStringLiteral("Could not enumerate Wayland capture sources");
    return false;
  }
  if (!state.shm || !state.toplevelList || !state.sourceManager ||
      !state.captureManager) {
    error = QStringLiteral("Compositor does not expose native window capture");
    return false;
  }

  const auto match =
      std::ranges::find_if(state.toplevels, [&](const auto &toplevel) {
        return toplevel->identifier == window.stableId;
      });
  if (match == state.toplevels.end()) {
    error = QStringLiteral(
        "Selected window is no longer available for native capture");
    return false;
  }

  state.source =
      ext_foreign_toplevel_image_capture_source_manager_v1_create_source(
          state.sourceManager, (*match)->handle);
  state.session = ext_image_copy_capture_manager_v1_create_session(
      state.captureManager, state.source, 0);
  ext_image_copy_capture_session_v1_add_listener(state.session,
                                                 &kSessionListener, &state);
  if (!dispatchUntil(state, &state.constraintsDone, 2000, error))
    return false;
  if (state.stopped) {
    error = QStringLiteral("Compositor stopped native window capture");
    return false;
  }
  if (!createShmBuffer(state, error))
    return false;

  state.frame = ext_image_copy_capture_session_v1_create_frame(state.session);
  ext_image_copy_capture_frame_v1_add_listener(state.frame, &kFrameListener,
                                               &state);
  ext_image_copy_capture_frame_v1_attach_buffer(state.frame, state.buffer);
  ext_image_copy_capture_frame_v1_damage_buffer(
      state.frame, 0, 0, static_cast<int32_t>(state.width),
      static_cast<int32_t>(state.height));
  ext_image_copy_capture_frame_v1_capture(state.frame);
  if (!dispatchUntil(state, &state.frameDone, 2000, error) ||
      !state.frameReady) {
    if (error.isEmpty())
      error = QStringLiteral(
          "Compositor could not capture the selected window surface");
    return false;
  }
  const QImage captured = copyCapturedImage(state);
  if (captured.isNull()) {
    error = QStringLiteral("Could not decode native window capture");
    return false;
  }
  image = normalizeWaylandCapture(captured, state.transform);
  if (image.isNull()) {
    error = QStringLiteral("Unsupported transform in native window capture");
    return false;
  }
  return true;
}
