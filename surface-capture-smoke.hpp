/** @fileoverview Declares the native Wayland cleanup smoke check. */
#pragma once

/** Verifies CaptureState destroys every owned Wayland object in order. */
bool runWaylandCleanupChecks();
