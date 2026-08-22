/** @fileoverview Captures native monitor outputs through Wayland protocols. */
#include "capture.hpp"

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

// Named (not anonymous) so OutputCapture::State can derive from it without
// giving an exported type internal-linkage members.
namespace capture_detail {
struct OutputInfo {
  wl_output *output = nullptr;
  std::string name;
};

struct CaptureState {
  wl_display *display = nullptr;
  wl_registry *registry = nullptr;
  wl_shm *shm = nullptr;
  ext_output_image_capture_source_manager_v1 *outputSourceManager = nullptr;
  ext_image_copy_capture_manager_v1 *captureManager = nullptr;
  ext_image_capture_source_v1 *source = nullptr;
  ext_image_copy_capture_session_v1 *session = nullptr;
  ext_image_copy_capture_frame_v1 *frame = nullptr;
  wl_shm_pool *pool = nullptr;
  wl_buffer *buffer = nullptr;
  std::vector<std::unique_ptr<OutputInfo>> outputs;
  std::vector<uint32_t> shmFormats;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
  /// Geometry/format the current shm buffer was actually created with. The
  /// session can re-announce width/height/formats at any dispatch (mode or
  /// scale change), even in the same batch as a frame's ready event, so
  /// everything that touches the mapped buffer must use these, not the live
  /// session values above.
  uint32_t bufferWidth = 0;
  uint32_t bufferHeight = 0;
  uint32_t bufferFormat = 0;
  uint32_t transform = WL_OUTPUT_TRANSFORM_NORMAL;
  bool constraintsDone = false;
  /// Set when the session re-announces its constraints (buffer_size after the
  /// first done); the next frame needs a fresh buffer.
  bool constraintsChanged = false;
  bool stopped = false;
  bool frameDone = false;
  bool frameReady = false;
  uint32_t failureReason = 0;
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
    for (const auto &output : outputs) {
      if (output->output)
        wl_output_release(output->output);
    }
    if (outputSourceManager)
      ext_output_image_capture_source_manager_v1_destroy(outputSourceManager);
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

void outputGeometry(void *, wl_output *, int32_t, int32_t, int32_t, int32_t,
                    int32_t, const char *, const char *, int32_t) {}
void outputMode(void *, wl_output *, uint32_t, int32_t, int32_t, int32_t) {}
void outputDone(void *, wl_output *) {}
void outputScale(void *, wl_output *, int32_t) {}
void outputName(void *data, wl_output *, const char *name) {
  static_cast<OutputInfo *>(data)->name = name ? name : "";
}
void outputDescription(void *, wl_output *, const char *) {}
constexpr wl_output_listener kOutputListener{outputGeometry, outputMode,
                                             outputDone,     outputScale,
                                             outputName,     outputDescription};

void registryGlobal(void *data, wl_registry *registry, uint32_t name,
                    const char *interface, uint32_t version) {
  auto &state = *static_cast<CaptureState *>(data);
  if (std::strcmp(interface, wl_shm_interface.name) == 0) {
    state.shm = static_cast<wl_shm *>(wl_registry_bind(
        registry, name, &wl_shm_interface, std::min(version, 1U)));
  } else if (std::strcmp(interface,
                         ext_image_copy_capture_manager_v1_interface.name) ==
             0) {
    state.captureManager =
        static_cast<ext_image_copy_capture_manager_v1 *>(wl_registry_bind(
            registry, name, &ext_image_copy_capture_manager_v1_interface,
            std::min(version, 1U)));
  } else if (std::strcmp(
                 interface,
                 ext_output_image_capture_source_manager_v1_interface.name) ==
             0) {
    state.outputSourceManager =
        static_cast<ext_output_image_capture_source_manager_v1 *>(
            wl_registry_bind(
                registry, name,
                &ext_output_image_capture_source_manager_v1_interface,
                std::min(version, 1U)));
  } else if (std::strcmp(interface, wl_output_interface.name) == 0 &&
             version >= 4) {
    auto output = std::make_unique<OutputInfo>();
    output->output = static_cast<wl_output *>(
        wl_registry_bind(registry, name, &wl_output_interface, 4));
    wl_output_add_listener(output->output, &kOutputListener, output.get());
    state.outputs.push_back(std::move(output));
  }
}
void registryRemoved(void *, wl_registry *, uint32_t) {}
constexpr wl_registry_listener kRegistryListener{registryGlobal,
                                                 registryRemoved};

void sessionBufferSize(void *data, ext_image_copy_capture_session_v1 *,
                       uint32_t width, uint32_t height) {
  auto &state = *static_cast<CaptureState *>(data);
  if (state.constraintsDone &&
      (width != state.width || height != state.height)) {
    state.constraintsChanged = true;
    // A re-announcement replaces the constraint set; drop the old formats so
    // the rebuild chooses from the current ones.
    state.shmFormats.clear();
  }
  state.width = width;
  state.height = height;
}
void sessionShmFormat(void *data, ext_image_copy_capture_session_v1 *,
                      uint32_t format) {
  auto &state = *static_cast<CaptureState *>(data);
  if (state.constraintsDone)
    state.constraintsChanged = true;
  state.shmFormats.push_back(format);
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
void frameFailed(void *data, ext_image_copy_capture_frame_v1 *,
                 uint32_t reason) {
  auto &state = *static_cast<CaptureState *>(data);
  state.failureReason = reason;
  state.frameDone = true;
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

  state.bufferWidth = state.width;
  state.bufferHeight = state.height;
  state.bufferFormat = state.format;

  const std::size_t stride = static_cast<std::size_t>(state.bufferWidth) * 4;
  state.memorySize = stride * state.bufferHeight;
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
      state.pool, 0, static_cast<int32_t>(state.bufferWidth),
      static_cast<int32_t>(state.bufferHeight), static_cast<int32_t>(stride),
      state.bufferFormat);
  if (!state.buffer) {
    error = QStringLiteral("Could not create Wayland surface capture buffer");
    return false;
  }
  return true;
}

/** Releases the shm buffer so createShmBuffer() can size a new one. */
void destroyShmBuffer(CaptureState &state) {
  if (state.buffer)
    wl_buffer_destroy(state.buffer);
  if (state.pool)
    wl_shm_pool_destroy(state.pool);
  if (state.memory != MAP_FAILED)
    munmap(state.memory, state.memorySize);
  if (state.fd >= 0)
    close(state.fd);
  state.buffer = nullptr;
  state.pool = nullptr;
  state.memory = MAP_FAILED;
  state.memorySize = 0;
  state.fd = -1;
}

/** Captures one frame from the open session into the shm buffer. */
bool captureFrame(CaptureState &state, QString &error, int timeoutMs) {
  if (!state.buffer) {
    error = QStringLiteral("No capture buffer is available");
    return false;
  }
  state.frameDone = false;
  state.frameReady = false;
  state.failureReason = 0;
  state.transform = WL_OUTPUT_TRANSFORM_NORMAL;
  state.frame = ext_image_copy_capture_session_v1_create_frame(state.session);
  ext_image_copy_capture_frame_v1_add_listener(state.frame, &kFrameListener,
                                               &state);
  ext_image_copy_capture_frame_v1_attach_buffer(state.frame, state.buffer);
  ext_image_copy_capture_frame_v1_damage_buffer(
      state.frame, 0, 0, static_cast<int32_t>(state.bufferWidth),
      static_cast<int32_t>(state.bufferHeight));
  ext_image_copy_capture_frame_v1_capture(state.frame);
  const bool done = dispatchUntil(state, &state.frameDone, timeoutMs, error);
  ext_image_copy_capture_frame_v1_destroy(state.frame);
  state.frame = nullptr;
  return done && state.frameReady;
}

QImage copyCapturedImage(const CaptureState &state) {
  QImage::Format imageFormat = QImage::Format_Invalid;
  switch (state.bufferFormat) {
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
  const int stride = static_cast<int>(state.bufferWidth * 4);
  return QImage(static_cast<uchar *>(state.memory),
                static_cast<int>(state.bufferWidth),
                static_cast<int>(state.bufferHeight), stride, imageFormat)
      .copy();
}
} // namespace capture_detail

using namespace capture_detail;

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

/** Opens the copy-capture session on `state.source` and sizes its buffer.
 *  Frames are captured without the cursor: scroll-capture frames are
 *  stitched, and a painted cursor would become a moving artefact. */
static bool openCaptureSession(CaptureState &state, QString &error,
                               const QString &stoppedError) {
  state.session = ext_image_copy_capture_manager_v1_create_session(
      state.captureManager, state.source, 0);
  ext_image_copy_capture_session_v1_add_listener(state.session,
                                                 &kSessionListener, &state);
  if (!dispatchUntil(state, &state.constraintsDone, 2000, error))
    return false;
  if (state.stopped) {
    error = stoppedError;
    return false;
  }
  return createShmBuffer(state, error);
}

static bool captureCurrentSource(CaptureState &state, QImage &image, QString &error,
                          const QString &stoppedError,
                          const QString &failedError,
                          const QString &decodeError,
                          const QString &transformError) {
  if (!openCaptureSession(state, error, stoppedError))
    return false;
  if (!captureFrame(state, error, 2000)) {
    if (state.stopped)
      error = stoppedError;
    else if (error.isEmpty())
      error = failedError;
    return false;
  }
  const QImage captured = copyCapturedImage(state);
  if (captured.isNull()) {
    error = decodeError;
    return false;
  }
  image = normalizeWaylandCapture(captured, state.transform);
  if (image.isNull()) {
    error = transformError;
    return false;
  }
  return true;
}

/** Connects to the display and binds the output-capture globals. */
static bool connectOutputCaptureDisplay(CaptureState &state, QString &error) {
  state.display = wl_display_connect(nullptr);
  if (!state.display) {
    error = QStringLiteral("Could not connect to Wayland for output capture");
    return false;
  }
  state.registry = wl_display_get_registry(state.display);
  wl_registry_add_listener(state.registry, &kRegistryListener, &state);
  if (wl_display_roundtrip(state.display) < 0 ||
      wl_display_roundtrip(state.display) < 0) {
    error = QStringLiteral("Could not enumerate Wayland capture sources");
    return false;
  }
  if (!state.shm || !state.outputSourceManager || !state.captureManager) {
    error = QStringLiteral(
        "Compositor does not expose ext-image-copy-capture output capture");
    return false;
  }
  return true;
}

bool captureOutputSurface(const MonitorInfo &monitor, QImage &image,
                          QString &error) {
  if (monitor.name.isEmpty()) {
    error = QStringLiteral("Focused monitor has no output name");
    return false;
  }

  CaptureState state;
  if (!connectOutputCaptureDisplay(state, error))
    return false;

  const auto match =
      std::ranges::find_if(state.outputs, [&](const auto &output) {
        return output->name == monitor.name.toStdString();
      });
  if (match == state.outputs.end()) {
    error = QStringLiteral("Could not find Wayland output %1 for capture")
                .arg(monitor.name);
    return false;
  }

  state.source = ext_output_image_capture_source_manager_v1_create_source(
      state.outputSourceManager, (*match)->output);
  return captureCurrentSource(
      state, image, error,
      QStringLiteral("Compositor stopped native output capture"),
      QStringLiteral("Compositor could not capture the focused output"),
      QStringLiteral("Could not decode native output capture"),
      QStringLiteral("Unsupported transform in native output capture"));
}

struct OutputCapture::State : capture_detail::CaptureState {};

OutputCapture::OutputCapture() = default;
OutputCapture::~OutputCapture() = default;

bool OutputCapture::open(const QString &outputName, QString &error) {
  close();
  auto state = std::make_unique<State>();
  if (!connectOutputCaptureDisplay(*state, error))
    return false;
  const std::string wanted = outputName.toStdString();
  const auto match =
      std::ranges::find_if(state->outputs, [&](const auto &output) {
        return output->name == wanted;
      });
  if (match == state->outputs.end()) {
    error = QStringLiteral("Output %1 is not available for native capture")
                .arg(outputName);
    return false;
  }
  state->source = ext_output_image_capture_source_manager_v1_create_source(
      state->outputSourceManager, (*match)->output);
  if (!openCaptureSession(
          *state, error,
          QStringLiteral("Compositor stopped native output capture")))
    return false;
  state_ = std::move(state);
  return true;
}

bool OutputCapture::grab(QImage &image, QString &error, int timeoutMs) {
  if (!state_) {
    error = QStringLiteral("Output capture is not open");
    return false;
  }
  State &state = *state_;
  // The buffer is reused frame after frame; only a constraint change (mode or
  // format switch, announced by the session or reported by a failed frame)
  // sizes a new one.
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (state.constraintsChanged) {
      state.constraintsChanged = false;
      destroyShmBuffer(state);
      if (!createShmBuffer(state, error)) {
        // The next grab must retry the rebuild; without this it would attach
        // a null buffer.
        state.constraintsChanged = true;
        return false;
      }
    }
    if (captureFrame(state, error, timeoutMs))
      break;
    if (state.stopped) {
      error = QStringLiteral("Compositor stopped native output capture");
      return false;
    }
    if (state.failureReason ==
            EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS &&
        attempt == 0) {
      state.constraintsChanged = true;
      continue;
    }
    if (error.isEmpty())
      error = QStringLiteral("Compositor could not capture the output");
    return false;
  }
  if (!state.frameReady) {
    if (error.isEmpty())
      error = QStringLiteral("Compositor could not capture the output");
    return false;
  }
  const QImage captured = copyCapturedImage(state);
  if (captured.isNull()) {
    error = QStringLiteral("Could not decode native output capture");
    return false;
  }
  image = normalizeWaylandCapture(captured, state.transform);
  if (image.isNull()) {
    error = QStringLiteral("Unsupported transform in native output capture");
    return false;
  }
  return true;
}

bool OutputCapture::isOpen() const { return state_ != nullptr; }

bool OutputCapture::sessionStopped() const { return state_ && state_->stopped; }

QSize OutputCapture::bufferSize() const {
  if (!state_)
    return {};
  return {static_cast<int>(state_->bufferWidth),
          static_cast<int>(state_->bufferHeight)};
}

void OutputCapture::close() { state_.reset(); }
