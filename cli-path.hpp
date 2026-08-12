/** @fileoverview Resolves local image targets accepted by the command line. */
#pragma once

#include <QString>

/** Returns an existing local path for a raw path or local file URL. */
QString resolveLocalImagePath(const QString &target);
