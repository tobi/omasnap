/** @fileoverview Neovim-backed source-code image rendering. */
#pragma once

#include <QColor>
#include <QImage>
#include <QString>

struct HighlightedCode {
  QString preformattedHtml;
  QString styleSheet;
  QColor background;
  QColor foreground;
  QColor lineNumber;
  QString filetype;
};

struct CodeImageRequest {
  QString code;
  QString sourceName;
  QString title;
  QString language;
};

/** Reads UTF-8 code from `path`, or text from the Wayland clipboard when empty.
 */
[[nodiscard]] bool loadCodeInput(const QString &path, QString &code,
                                 QString &error);
/** Returns a filename found in the focused window title, when one is present.
 */
[[nodiscard]] QString activeCodeFilenameHint();
/** Validates and normalizes a snippet before handing it to Neovim. */
[[nodiscard]] bool normalizeCodeInput(QString &code, QString &error);
/** Parses the private JSON contract emitted by the bundled Neovim script. */
[[nodiscard]] bool parseNeovimHighlights(const QByteArray &json,
                                         HighlightedCode &highlighted,
                                         QString &error);
/** Runs configured Neovim syntax highlighting without evaluating the snippet.
 */
[[nodiscard]] bool highlightCodeWithNeovim(const CodeImageRequest &request,
                                           HighlightedCode &highlighted,
                                           QString &error);
/** Paints only the themed central code panel; Omasnap adds its own backdrop. */
[[nodiscard]] QImage renderCodePanel(const HighlightedCode &highlighted,
                                     const QString &title);
[[nodiscard]] bool createCodeImage(CodeImageRequest request, QImage &image,
                                   QString &detectedLanguage, QString &error);
