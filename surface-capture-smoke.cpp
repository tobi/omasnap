/** @fileoverview Exercises native Wayland capture cleanup with fake proxies. */
#include "surface-capture-smoke.hpp"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"

#include <wayland-client.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace {
enum class CleanupCall {
  Frame,
  Session,
  Source,
  Toplevel,
  ToplevelList,
  SourceManager,
  CaptureManager,
  Buffer,
  Pool,
  Shm,
  Registry,
  Display,
};

std::array<CleanupCall, 13> cleanupCalls;
std::size_t cleanupCallCount = 0;
bool recordingCleanup = false;

/** Records one cleanup call while the lifecycle test is active. */
template <CleanupCall Call, typename Proxy> void recordCleanup(Proxy *) {
  if (cleanupCallCount < cleanupCalls.size())
    cleanupCalls[cleanupCallCount++] = Call;
}
} // namespace

/** Records or forwards frame destruction. */
void recordFrameDestroy(ext_image_copy_capture_frame_v1 *proxy) {
  if (recordingCleanup)
    recordCleanup<CleanupCall::Frame>(proxy);
  else
    ::ext_image_copy_capture_frame_v1_destroy(proxy);
}
/** Records or forwards session destruction. */
void recordSessionDestroy(ext_image_copy_capture_session_v1 *proxy) {
  if (recordingCleanup)
    recordCleanup<CleanupCall::Session>(proxy);
  else
    ::ext_image_copy_capture_session_v1_destroy(proxy);
}
/** Records or forwards source destruction. */
void recordSourceDestroy(ext_image_capture_source_v1 *proxy) {
  if (recordingCleanup)
    recordCleanup<CleanupCall::Source>(proxy);
  else
    ::ext_image_capture_source_v1_destroy(proxy);
}
/** Records or forwards toplevel destruction. */
void recordToplevelDestroy(ext_foreign_toplevel_handle_v1 *proxy) {
  if (recordingCleanup)
    recordCleanup<CleanupCall::Toplevel>(proxy);
  else
    ::ext_foreign_toplevel_handle_v1_destroy(proxy);
}
/** Records or forwards toplevel-list destruction. */
void recordToplevelListDestroy(ext_foreign_toplevel_list_v1 *proxy) {
  if (recordingCleanup)
    recordCleanup<CleanupCall::ToplevelList>(proxy);
  else
    ::ext_foreign_toplevel_list_v1_destroy(proxy);
}
/** Records or forwards source-manager destruction. */
void recordSourceManagerDestroy(
    ext_foreign_toplevel_image_capture_source_manager_v1 *proxy) {
  if (recordingCleanup)
    recordCleanup<CleanupCall::SourceManager>(proxy);
  else
    ::ext_foreign_toplevel_image_capture_source_manager_v1_destroy(proxy);
}
/** Records or forwards capture-manager destruction. */
void recordCaptureManagerDestroy(ext_image_copy_capture_manager_v1 *proxy) {
  if (recordingCleanup)
    recordCleanup<CleanupCall::CaptureManager>(proxy);
  else
    ::ext_image_copy_capture_manager_v1_destroy(proxy);
}
/** Records or forwards buffer destruction. */
void recordBufferDestroy(wl_buffer *proxy) {
  if (recordingCleanup)
    recordCleanup<CleanupCall::Buffer>(proxy);
  else
    ::wl_buffer_destroy(proxy);
}
/** Records or forwards pool destruction. */
void recordPoolDestroy(wl_shm_pool *proxy) {
  if (recordingCleanup)
    recordCleanup<CleanupCall::Pool>(proxy);
  else
    ::wl_shm_pool_destroy(proxy);
}
/** Records or forwards shared-memory destruction. */
void recordShmDestroy(wl_shm *proxy) {
  if (recordingCleanup)
    recordCleanup<CleanupCall::Shm>(proxy);
  else
    ::wl_shm_destroy(proxy);
}
/** Records or forwards registry destruction. */
void recordRegistryDestroy(wl_registry *proxy) {
  if (recordingCleanup)
    recordCleanup<CleanupCall::Registry>(proxy);
  else
    ::wl_registry_destroy(proxy);
}
/** Records or forwards display disconnection. */
void recordDisplayDisconnect(wl_display *proxy) {
  if (recordingCleanup)
    recordCleanup<CleanupCall::Display>(proxy);
  else
    ::wl_display_disconnect(proxy);
}

#define ext_image_copy_capture_frame_v1_destroy recordFrameDestroy
#define ext_image_copy_capture_session_v1_destroy recordSessionDestroy
#define ext_image_capture_source_v1_destroy recordSourceDestroy
#define ext_foreign_toplevel_handle_v1_destroy recordToplevelDestroy
#define ext_foreign_toplevel_list_v1_destroy recordToplevelListDestroy
#define ext_foreign_toplevel_image_capture_source_manager_v1_destroy           \
  recordSourceManagerDestroy
#define ext_image_copy_capture_manager_v1_destroy recordCaptureManagerDestroy
#define wl_buffer_destroy recordBufferDestroy
#define wl_shm_pool_destroy recordPoolDestroy
#define wl_shm_destroy recordShmDestroy
#define wl_registry_destroy recordRegistryDestroy
#define wl_display_disconnect recordDisplayDisconnect
#include "surface-capture.cpp"
#undef ext_image_copy_capture_frame_v1_destroy
#undef ext_image_copy_capture_session_v1_destroy
#undef ext_image_capture_source_v1_destroy
#undef ext_foreign_toplevel_handle_v1_destroy
#undef ext_foreign_toplevel_list_v1_destroy
#undef ext_foreign_toplevel_image_capture_source_manager_v1_destroy
#undef ext_image_copy_capture_manager_v1_destroy
#undef wl_buffer_destroy
#undef wl_shm_pool_destroy
#undef wl_shm_destroy
#undef wl_registry_destroy
#undef wl_display_disconnect

/** Runs the native capture cleanup lifecycle check. */
bool runWaylandCleanupChecks() {
  cleanupCallCount = 0;
  recordingCleanup = true;
  {
    CaptureState state;
    state.display = reinterpret_cast<wl_display *>(1);
    state.registry = reinterpret_cast<wl_registry *>(2);
    state.shm = reinterpret_cast<wl_shm *>(3);
    state.toplevelList = reinterpret_cast<ext_foreign_toplevel_list_v1 *>(4);
    state.sourceManager = reinterpret_cast<
        ext_foreign_toplevel_image_capture_source_manager_v1 *>(5);
    state.captureManager =
        reinterpret_cast<ext_image_copy_capture_manager_v1 *>(6);
    state.source = reinterpret_cast<ext_image_capture_source_v1 *>(7);
    state.session = reinterpret_cast<ext_image_copy_capture_session_v1 *>(8);
    state.frame = reinterpret_cast<ext_image_copy_capture_frame_v1 *>(9);
    state.buffer = reinterpret_cast<wl_buffer *>(10);
    state.pool = reinterpret_cast<wl_shm_pool *>(11);
    for (const auto handle : {12, 13}) {
      auto toplevel = std::make_unique<Toplevel>();
      toplevel->handle = reinterpret_cast<ext_foreign_toplevel_handle_v1 *>(
          static_cast<std::uintptr_t>(handle));
      state.toplevels.push_back(std::move(toplevel));
    }
  }
  recordingCleanup = false;
  constexpr std::array expected{
      CleanupCall::Frame,         CleanupCall::Session,
      CleanupCall::Source,        CleanupCall::Toplevel,
      CleanupCall::Toplevel,      CleanupCall::ToplevelList,
      CleanupCall::SourceManager, CleanupCall::CaptureManager,
      CleanupCall::Buffer,        CleanupCall::Pool,
      CleanupCall::Shm,           CleanupCall::Registry,
      CleanupCall::Display,
  };
  return cleanupCallCount == expected.size() && cleanupCalls == expected;
}
