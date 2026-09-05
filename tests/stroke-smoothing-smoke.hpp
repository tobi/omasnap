#pragma once

#include <QString>

class QApplication;

/** Freehand input, geometry, rendering, selection and history checks. */
bool runStrokeSmoothingSmoke(QApplication &application, QString &error);
