#include "code-image-smoke.hpp"

#include "capture.hpp"
#include "code-image.hpp"
#include "editor.hpp"

#include <utility>

#include <QApplication>
#include <QByteArray>
#include <QImage>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <Qt>
#include <QtGlobal>

bool runCodeImageChecks(QApplication &application, QString &error) {
  QString normalized = QStringLiteral("\tconst value = 42;\r\n\r\n");
  if (!normalizeCodeInput(normalized, error) ||
      normalized != QStringLiteral("    const value = 42;")) {
    error =
        QStringLiteral("Code normalization did not expand tabs and trim lines");
    return false;
  }

  QString tooLong;
  for (int line = 0; line < 121; ++line)
    tooLong += QStringLiteral("line\n");
  if (normalizeCodeInput(tooLong, error) ||
      !error.contains(QStringLiteral("120"))) {
    error = QStringLiteral("Code normalization accepted more than 120 lines");
    return false;
  }
  QTemporaryFile oversized;
  if (!oversized.open() ||
      oversized.write(QByteArray(32 * 1024 + 1, 'x')) != 32 * 1024 + 1) {
    error = QStringLiteral("Could not prepare oversized code input");
    return false;
  }
  oversized.flush();
  QString oversizedCode;
  if (loadCodeInput(oversized.fileName(), oversizedCode, error) ||
      !error.contains(QStringLiteral("32 KiB"))) {
    error = QStringLiteral("Code input accepted more than 32 KiB");
    return false;
  }

  static const QByteArray fixture = R"json({
    "filetype":"cpp",
    "background":"#101318",
    "foreground":"#f3f4f6",
    "line_number":"#667085",
    "styles":".LineNr { color: #667085 } .Statement { color: #ff5a4a; font-weight: bold }",
    "pre":"<span class=\"LineNr\">1  </span><span class=\"Statement\">return</span> 42;\n<span class=\"LineNr\">2  </span>你好 · こんにちは · 안녕하세요 · 🚀"
  })json";
  HighlightedCode highlighted;
  if (!parseNeovimHighlights(fixture, highlighted, error) ||
      highlighted.filetype != QStringLiteral("cpp") ||
      !highlighted.preformattedHtml.contains(QStringLiteral("你好")) ||
      !highlighted.preformattedHtml.contains(QStringLiteral("こんにちは")) ||
      !highlighted.preformattedHtml.contains(QStringLiteral("안녕하세요")) ||
      !highlighted.preformattedHtml.contains(QStringLiteral("🚀"))) {
    error = QStringLiteral("Could not parse valid Neovim highlighting output");
    return false;
  }
  const QImage fixtureImage =
      renderCodePanel(highlighted, QStringLiteral("example.cpp"));
  if (fixtureImage.isNull() || fixtureImage.width() < 700 ||
      fixtureImage.height() < 180) {
    error = QStringLiteral("Code panel renderer returned an invalid card");
    return false;
  }
  const QImage untitledImage = renderCodePanel(highlighted, QString());
  if (untitledImage.isNull() ||
      untitledImage.height() >= fixtureImage.height()) {
    error = QStringLiteral("An empty title still reserved a header row");
    return false;
  }
  CaptureData capture;
  capture.source = untitledImage;
  capture.previewSize = untitledImage.size();
  capture.monitor.geometry = QRect(QPoint(), untitledImage.size());
  capture.monitor.pixelSize = untitledImage.size();
  capture.monitor.scale = 1.0;
  Operation background;
  background.type = Operation::Type::Background;
  background.background = BackgroundStyle::Aurora;
  OperationLog log;
  log.ops = {background};
  log.index = 1;
  CaptureEditor editor(std::move(capture), CaptureEditor::CaptureMode::Code,
                       QuickOutputMode::None, log);
  editor.resize(900, 700);
  editor.show();
  application.processEvents();
  if (!editor.statusForTest().contains(QStringLiteral("Code image")) ||
      editor.renderCurrentOutput().isNull()) {
    error = QStringLiteral("Code mode did not enter the existing editor flow");
    return false;
  }
  editor.close();
  if (parseNeovimHighlights(QByteArrayLiteral("not json"), highlighted,
                            error)) {
    error = QStringLiteral("Invalid Neovim highlighting output was accepted");
    return false;
  }

  const QByteArray originalPath = qgetenv("PATH");
  qputenv("PATH", "/omasnap-missing-tools");
  CodeImageRequest missingRequest;
  missingRequest.code = QStringLiteral("const answer = 42;");
  if (highlightCodeWithNeovim(missingRequest, highlighted, error) ||
      !error.contains(QStringLiteral("Neovim"))) {
    qputenv("PATH", originalPath);
    error =
        QStringLiteral("Missing Neovim did not produce an actionable error");
    return false;
  }
  qputenv("PATH", originalPath);

  if (!QStandardPaths::findExecutable(QStringLiteral("nvim")).isEmpty()) {
    CodeImageRequest request;
    request.code = QStringLiteral(
        "#include <iostream>\n\nint main() {\n    std::cout << 42;\n}");
    if (!highlightCodeWithNeovim(request, highlighted, error) ||
        highlighted.filetype != QStringLiteral("cpp") ||
        !highlighted.preformattedHtml.contains(QStringLiteral("iostream"))) {
      error =
          QStringLiteral("Configured Neovim did not highlight inferred C++");
      return false;
    }
    request.code = QStringLiteral("package main\n\nimport \"fmt\"\n\nfunc "
                                  "main() {\n    fmt.Println(\"你好\")\n}");
    request.sourceName.clear();
    if (!highlightCodeWithNeovim(request, highlighted, error) ||
        highlighted.filetype != QStringLiteral("go") ||
        highlighted.preformattedHtml.contains(QStringLiteral("golangci-lint"),
                                              Qt::CaseInsensitive)) {
      error = QStringLiteral(
          "Neovim code mode leaked Go diagnostics into syntax output");
      return false;
    }
    request.code =
        QStringLiteral("alpha beta gamma\n你好 こんにちは 안녕하세요");
    request.sourceName = QStringLiteral("snippet.unknown");
    if (!highlightCodeWithNeovim(request, highlighted, error) ||
        !highlighted.filetype.isEmpty() ||
        !highlighted.preformattedHtml.contains(QStringLiteral("你好"))) {
      error = QStringLiteral("Unknown code did not fall back to plain text");
      return false;
    }
  }
  return true;
}
