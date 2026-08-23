#pragma once

#include <QString>

class QApplication;

[[nodiscard]] bool runCodeImageChecks(QApplication &application,
                                      QString &error);
