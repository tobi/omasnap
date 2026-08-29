/** @fileoverview Clip-tool mapping: marquee, lift, hole, undo. */
#pragma once

#include <QApplication>
#include <QString>

[[nodiscard]] bool runClipMappingSmoke(QApplication &application,
                                       const QString &outputRoot,
                                       QString &error);
