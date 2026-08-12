/** @fileoverview Resolves local image targets accepted by the command line. */
#include "cli-path.hpp"

#include <QFileInfo>
#include <QUrl>

QString resolveLocalImagePath(const QString &target) {
  if (target.isEmpty())
    return {};

  const QUrl url(target);
  QString path;
  if (url.isLocalFile())
    path = url.toLocalFile();
  else if (!url.scheme().isEmpty() &&
           target.startsWith(url.scheme() + QStringLiteral("://"),
                             Qt::CaseInsensitive))
    return {};
  else
    path = target;

  const QFileInfo file(path);
  return file.isFile() ? file.absoluteFilePath() : QString{};
}
