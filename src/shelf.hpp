#pragma once

#include <QImage>
#include <QString>

/** Persists a capture and starts the Shelf process that will display it. */
[[nodiscard]] bool queueCaptureOnShelf(const QImage &image,
                                       const QString &screenName,
                                       QString &error);

/** Hides/restores an existing Shelf while the compositor captures the screen.
 */
[[nodiscard]] bool setCaptureShelfHidden(bool hidden);

/** Runs or notifies the single per-session capture Shelf process. */
[[nodiscard]] int runCaptureShelf(const QString &path,
                                  const QString &screenName = {});
