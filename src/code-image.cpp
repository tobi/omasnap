#include "code-image.hpp"

#include <algorithm>
#include <utility>

#include <QByteArray>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QHash>
#include <QIODevice>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QPainter>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QTextOption>
#include <Qt>
#include <QtMath>
#include <QtNumeric>
#include <QtTypes>

namespace {
constexpr qsizetype kMaximumCodeBytes = qsizetype{32} * 1024;
constexpr qsizetype kMaximumCodeLines = 120;
constexpr qsizetype kMaximumLineCharacters = 240;
constexpr qsizetype kMaximumHighlightBytes = qsizetype{2} * 1024 * 1024;
constexpr int kNeovimTimeoutMs = 10000;

constexpr auto kHighlightScript = R"lua(
local output = vim.env.OMASNAP_CODE_OUTPUT
local requested = vim.env.OMASNAP_CODE_LANGUAGE or ""
local source_name = vim.env.OMASNAP_CODE_SOURCE_NAME or "snippet.txt"
local lines = vim.api.nvim_buf_get_lines(0, 0, -1, false)

local filetype = requested
if filetype == "" then
  local args = { contents = lines }
  if source_name ~= "snippet.txt" then args.filename = source_name end
  filetype = vim.filetype.match(args) or ""
end
if filetype == "" then
  local text = table.concat(lines, "\n")
  local trimmed = vim.trim(text)
  local json_ok, json_value = pcall(vim.json.decode, trimmed)
  if json_ok and type(json_value) == "table" then
    filetype = "json"
  elseif text:match("^%s*package%s+[%w_.]+") and text:match("\n%s*func%s+[%w_]+") then
    filetype = "go"
  elseif text:match("#include%s*[<\"]") or text:match("%f[%w]std::") or text:match("%f[%w]int%s+main%s*%(") then
    filetype = "cpp"
  elseif text:match("%f[%w]fn%s+[%w_]+%s*%(") and (text:match("%f[%w]let%s+mut%f[%W]") or text:match("%f[%w]use%s+[%w_:]+") or text:match("%-%>%s*[%w_]+")) then
    filetype = "rust"
  elseif text:match("%f[%w]def%s+[%w_]+%s*%b()%s*:") or text:match("^%s*from%s+[%w_.]+%s+import%s+") then
    filetype = "python"
  elseif text:match("<!DOCTYPE%s+html") or text:match("<html[%s>]") or text:match("<[a-z][%w-]*[%s>]") then
    filetype = "html"
  elseif text:match("%f[%w]interface%s+[%w_]+%s*{") or text:match("%f[%w]type%s+[%w_]+%s*=") then
    filetype = "typescript"
  elseif text:match("%f[%w]const%s+[%w_$]+%s*=") or text:match("%f[%w]let%s+[%w_$]+%s*=") or text:match("=>") or text:match("%f[%w]function%s+[%w_$]*%s*%(") then
    filetype = "javascript"
  elseif text:match("^%s*[Ss][Ee][Ll][Ee][Cc][Tt]%s+") or text:match("^%s*[Ii][Nn][Ss][Ee][Rr][Tt]%s+") or text:match("^%s*[Uu][Pp][Dd][Aa][Tt][Ee]%s+") then
    filetype = "sql"
  elseif text:match("^%s*local%s+[%w_]+") and (text:match("%f[%w]function%f[%W]") or text:match("%f[%w]end%f[%W]")) then
    filetype = "lua"
  elseif text:match("[%w.#][%w%s.#,:>%[%]()-]*%s*{%s*[%w-]+%s*:") then
    filetype = "css"
  elseif text:match("^%s*#%s+[^#]") or text:match("\n%s*[-*]%s+%S") then
    filetype = "markdown"
  end
end

vim.opt_local.swapfile = false
vim.opt_local.undofile = false
vim.opt_local.modeline = false
vim.cmd("syntax enable")
if filetype ~= "" then vim.bo.syntax = filetype end

vim.cmd("packadd nvim.tohtml")
local exported = require("tohtml").tohtml(0, { number_lines = true })
local html = table.concat(exported, "\n")
local styles = html:match("<style>\n([%s%S]-)\n</style>") or ""
local pre = html:match("<pre>\n([%s%S]-)\n</pre>") or ""
pre = pre:gsub('class="CursorLineNr"', 'class="LineNr"')

local function color(group, attribute)
  local id = vim.fn.synIDtrans(vim.fn.hlID(group))
  return vim.fn.synIDattr(id, attribute)
end
local payload = vim.json.encode({
  filetype = filetype,
  background = color("Normal", "bg#"),
  foreground = color("Normal", "fg#"),
  line_number = color("LineNr", "fg#"),
  styles = styles,
  pre = pre,
})
vim.fn.writefile({ payload }, output, "b")
)lua";

QHash<QString, QColor> omarchyThemeColors() {
  QProcess process;
  process.start(QStringLiteral("omarchy-theme-color"),
                {QStringLiteral("--all")});
  if (!process.waitForStarted(1000) || !process.waitForFinished(2000) ||
      process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    return {};
  QHash<QString, QColor> colors;
  const QString output = QString::fromUtf8(process.readAllStandardOutput());
  for (const QString &line : output.split(QChar('\n'))) {
    const qsizetype separator = line.indexOf(QChar('\t'));
    if (separator <= 0)
      continue;
    const QColor color(line.mid(separator + 1).trimmed());
    if (color.isValid())
      colors.insert(line.left(separator), color);
  }
  return colors;
}

QColor blend(const QColor &from, const QColor &to, qreal amount) {
  return {qRound(from.red() + (to.red() - from.red()) * amount),
          qRound(from.green() + (to.green() - from.green()) * amount),
          qRound(from.blue() + (to.blue() - from.blue()) * amount)};
}

QString sourceNameFor(const CodeImageRequest &request) {
  if (!request.sourceName.trimmed().isEmpty())
    return QFileInfo(request.sourceName.trimmed()).fileName();
  if (!request.language.trimmed().isEmpty())
    return QStringLiteral("snippet.%1").arg(request.language.trimmed());
  return QStringLiteral("snippet.txt");
}

bool writePrivateFile(const QString &path, const QByteArray &contents,
                      QString &error) {
  QSaveFile file(path);
  file.setDirectWriteFallback(false);
  if (!file.open(QIODevice::WriteOnly) ||
      !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
    error = QStringLiteral("Could not prepare Neovim input");
    return false;
  }
  if (file.write(contents) != contents.size() || !file.commit()) {
    error = QStringLiteral("Could not write Neovim input");
    return false;
  }
  return true;
}

QString plainTextFromHtml(const HighlightedCode &highlighted,
                          const QFont &font) {
  QTextDocument document;
  document.setDefaultFont(font);
  document.setHtml(
      QStringLiteral("<pre>%1</pre>").arg(highlighted.preformattedHtml));
  return document.toPlainText();
}
} // namespace

bool loadCodeInput(const QString &path, QString &code, QString &error) {
  QByteArray contents;
  if (path.isEmpty()) {
    QProcess clipboard;
    clipboard.start(QStringLiteral("wl-paste"),
                    {QStringLiteral("--no-newline"), QStringLiteral("--type"),
                     QStringLiteral("text")});
    if (!clipboard.waitForStarted(1000) || !clipboard.waitForFinished(3000) ||
        clipboard.exitStatus() != QProcess::NormalExit ||
        clipboard.exitCode() != 0) {
      error = QStringLiteral("Could not read text from the clipboard");
      return false;
    }
    contents = clipboard.readAllStandardOutput();
  } else {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
      error = QStringLiteral("Could not read code file: %1").arg(path);
      return false;
    }
    if (file.size() > kMaximumCodeBytes) {
      error = QStringLiteral("Code input is larger than 32 KiB");
      return false;
    }
    contents = file.read(kMaximumCodeBytes + 1);
  }
  if (contents.size() > kMaximumCodeBytes) {
    error = QStringLiteral("Code input is larger than 32 KiB");
    return false;
  }
  code = QString::fromUtf8(contents);
  return normalizeCodeInput(code, error);
}

QString activeCodeFilenameHint() {
  QProcess process;
  process.start(QStringLiteral("hyprctl"),
                {QStringLiteral("activewindow"), QStringLiteral("-j")});
  if (!process.waitForStarted(500) || !process.waitForFinished(1500) ||
      process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    return {};
  const QJsonDocument document =
      QJsonDocument::fromJson(process.readAllStandardOutput());
  const QString title =
      document.object().value(QStringLiteral("title")).toString();
  static const QRegularExpression filename(
      QStringLiteral(
          R"((?:^|[\s/\\|—–-])([\w@+.-]+\.(?:c|cc|cpp|cs|css|dart|ex|exs|fish|go|h|hpp|hs|html|java|jl|js|json|jsx|kt|lua|md|ml|nim|php|pl|proto|py|r|rb|rs|rst|scala|scss|sh|sql|svelte|swift|tf|toml|ts|tsx|vim|vue|xml|yaml|yml|zig|zsh))\b)"),
      QRegularExpression::CaseInsensitiveOption);
  const QRegularExpressionMatch match = filename.match(title);
  return match.hasMatch() ? match.captured(1) : QString();
}

bool normalizeCodeInput(QString &code, QString &error) {
  code.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
  code.replace(QChar('\r'), QChar('\n'));
  code.replace(QChar('\t'), QStringLiteral("    "));
  while (code.endsWith(QChar('\n')))
    code.chop(1);
  if (code.trimmed().isEmpty()) {
    error = QStringLiteral("No code found; copy a snippet or pass a file");
    return false;
  }
  const QStringList lines = code.split(QChar('\n'));
  if (lines.size() > kMaximumCodeLines) {
    error = QStringLiteral("Code images are limited to 120 lines for sharing");
    return false;
  }
  if (std::ranges::any_of(lines, [](const QString &line) {
        return line.size() > kMaximumLineCharacters;
      })) {
    error = QStringLiteral("Code image lines are limited to 240 characters");
    return false;
  }
  return true;
}

bool parseNeovimHighlights(const QByteArray &json, HighlightedCode &highlighted,
                           QString &error) {
  if (json.size() > kMaximumHighlightBytes) {
    error = QStringLiteral("Neovim highlighting output is too large");
    return false;
  }
  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    error = QStringLiteral("Neovim returned invalid highlighting data");
    return false;
  }
  const QJsonObject root = document.object();
  HighlightedCode parsed;
  parsed.preformattedHtml = root.value(QStringLiteral("pre")).toString();
  parsed.styleSheet = root.value(QStringLiteral("styles")).toString();
  parsed.background =
      QColor(root.value(QStringLiteral("background")).toString());
  parsed.foreground =
      QColor(root.value(QStringLiteral("foreground")).toString());
  parsed.lineNumber =
      QColor(root.value(QStringLiteral("line_number")).toString());
  parsed.filetype = root.value(QStringLiteral("filetype")).toString();
  if (parsed.preformattedHtml.isEmpty()) {
    error = QStringLiteral("Neovim returned no highlighted code");
    return false;
  }
  highlighted = std::move(parsed);
  return true;
}

bool highlightCodeWithNeovim(const CodeImageRequest &request,
                             HighlightedCode &highlighted, QString &error) {
  const QString neovim = QStandardPaths::findExecutable(QStringLiteral("nvim"));
  if (neovim.isEmpty()) {
    error = QStringLiteral("Neovim is required for code highlighting");
    return false;
  }
  const QTemporaryDir temporary(QDir::tempPath() +
                                QStringLiteral("/omasnap-code-XXXXXX"));
  if (!temporary.isValid()) {
    error = QStringLiteral("Could not create private code workspace");
    return false;
  }
  const QString sourceName = sourceNameFor(request);
  const QString inputPath = temporary.filePath(QStringLiteral("source.input"));
  const QString scriptPath =
      temporary.filePath(QStringLiteral("highlight.lua"));
  const QString outputPath =
      temporary.filePath(QStringLiteral("highlight.json"));
  if (!writePrivateFile(inputPath, request.code.toUtf8(), error) ||
      !writePrivateFile(scriptPath, QByteArray(kHighlightScript), error))
    return false;

  QProcess process;
  process.setWorkingDirectory(temporary.path());
  QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
  environment.insert(QStringLiteral("OMASNAP_CODE_OUTPUT"), outputPath);
  environment.insert(QStringLiteral("OMASNAP_CODE_LANGUAGE"),
                     request.language.trimmed());
  environment.insert(QStringLiteral("OMASNAP_CODE_SOURCE_NAME"), sourceName);
  environment.insert(QStringLiteral("OMASNAP_CODE_SCRIPT"), scriptPath);
  process.setProcessEnvironment(environment);
  process.start(
      neovim,
      {QStringLiteral("--headless"), QStringLiteral("-n"), QStringLiteral("-i"),
       QStringLiteral("NONE"), QStringLiteral("--cmd"),
       QStringLiteral("set noexrc nomodeline"), inputPath, QStringLiteral("-c"),
       QStringLiteral("lua dofile(vim.env.OMASNAP_CODE_SCRIPT)"),
       QStringLiteral("-c"), QStringLiteral("qa!")});
  if (!process.waitForStarted(1000)) {
    error = QStringLiteral("Could not start Neovim");
    return false;
  }
  if (!process.waitForFinished(kNeovimTimeoutMs)) {
    process.kill();
    process.waitForFinished();
    error = QStringLiteral("Neovim highlighting timed out");
    return false;
  }
  QFile output(outputPath);
  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 ||
      !output.open(QIODevice::ReadOnly)) {
    const QString detail =
        QString::fromUtf8(process.readAllStandardError()).trimmed();
    error = detail.isEmpty() ? QStringLiteral("Neovim highlighting failed")
                             : QStringLiteral("Neovim highlighting failed: %1")
                                   .arg(detail.left(240));
    return false;
  }
  return parseNeovimHighlights(output.read(kMaximumHighlightBytes + 1),
                               highlighted, error);
}

QImage renderCodePanel(const HighlightedCode &highlighted,
                       const QString &title) {
  const QHash<QString, QColor> theme = omarchyThemeColors();
  QColor background = theme.value(QStringLiteral("background"));
  if (!background.isValid())
    background = highlighted.background;
  if (!background.isValid())
    background = QColor(QStringLiteral("#101318"));
  QColor foreground = theme.value(QStringLiteral("foreground"));
  if (!foreground.isValid())
    foreground = highlighted.foreground;
  if (!foreground.isValid())
    foreground = background.lightnessF() > 0.5 ? Qt::black : Qt::white;
  QFont codeFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
  codeFont.setPixelSize(28);
  codeFont.setStyleHint(QFont::Monospace);
  const QString plain = plainTextFromHtml(highlighted, codeFont);
  qreal widest = 0;
  QFontMetricsF metrics(codeFont);
  for (const QString &line : plain.split(QChar('\n')))
    widest = std::max(widest, metrics.horizontalAdvance(line));
  constexpr int maximumPanelWidth = 2000;
  constexpr int sidePadding = 54;
  if (widest + sidePadding * 2 > maximumPanelWidth) {
    const qreal available = maximumPanelWidth - sidePadding * 2;
    codeFont.setPixelSize(
        std::max(18, qFloor(codeFont.pixelSize() * available / widest)));
    metrics = QFontMetricsF(codeFont);
    widest = 0;
    for (const QString &line : plain.split(QChar('\n')))
      widest = std::max(widest, metrics.horizontalAdvance(line));
  }

  const int contentWidth =
      std::clamp(qCeil(widest) + 8, 620, maximumPanelWidth - sidePadding * 2);
  const int width = contentWidth + sidePadding * 2;
  const QString shownTitle = title.trimmed();
  const bool hasTitle = !shownTitle.isEmpty();
  const int headerHeight = hasTitle ? 76 : 0;
  const int topPadding = 38;
  const int bottomPadding = 42;
  QTextDocument document;
  document.setDocumentMargin(0);
  document.setDefaultFont(codeFont);
  QTextOption textOption = document.defaultTextOption();
  textOption.setWrapMode(QTextOption::NoWrap);
  document.setDefaultTextOption(textOption);
  QString fontFamily = codeFont.family();
  fontFamily.replace(QChar('"'), QStringLiteral("\\\""));
  const QString css =
      highlighted.styleSheet +
      QStringLiteral(
          "\nbody, pre { background-color: transparent; color: %1; }"
          "\npre { margin: 0; white-space: pre; font-family: \"%2\"; "
          "font-size: %3px; }"
          "\n.LineNr { color: %4; }")
          .arg(foreground.name(), fontFamily,
               QString::number(codeFont.pixelSize()),
               highlighted.lineNumber.isValid()
                   ? highlighted.lineNumber.name()
                   : blend(background, foreground, 0.42).name());
  document.setDefaultStyleSheet(css);
  document.setHtml(
      QStringLiteral("<pre>%1</pre>").arg(highlighted.preformattedHtml));
  document.setTextWidth(contentWidth);
  const int contentHeight = qCeil(document.size().height());
  const int height = headerHeight + topPadding + contentHeight + bottomPadding;
  QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
  image.fill(background);

  QPainter painter(&image);
  painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
  if (hasTitle) {
    const QColor header = blend(background, foreground,
                                background.lightnessF() > 0.5 ? 0.025 : 0.045);
    painter.fillRect(QRect(0, 0, width, headerHeight), header);
    painter.setPen(blend(background, foreground, 0.12));
    painter.drawLine(0, headerHeight - 1, width, headerHeight - 1);

    QFont headerFont = codeFont;
    headerFont.setPixelSize(std::clamp(codeFont.pixelSize() - 6, 14, 20));
    headerFont.setBold(true);
    painter.setFont(headerFont);
    painter.setPen(blend(background, foreground, 0.78));
    painter.drawText(QRect(120, 0, width - 240, headerHeight), Qt::AlignCenter,
                     QFontMetrics(headerFont)
                         .elidedText(shownTitle, Qt::ElideMiddle, width - 260));
  }
  painter.save();
  painter.translate(sidePadding, headerHeight + topPadding);
  document.drawContents(&painter, QRectF(0, 0, contentWidth, contentHeight));
  painter.restore();
  painter.end();
  return image;
}

bool createCodeImage(CodeImageRequest request, QImage &image,
                     QString &detectedLanguage, QString &error) {
  if (!normalizeCodeInput(request.code, error))
    return false;
  static const QRegularExpression validFiletype(
      QStringLiteral("^[A-Za-z0-9_+.-]{1,40}$"));
  request.language = request.language.trimmed();
  if (!request.language.isEmpty() &&
      !validFiletype.match(request.language).hasMatch()) {
    error = QStringLiteral("Language must be a valid Neovim filetype");
    return false;
  }
  request.title = request.title.trimmed();
  if (request.title.size() > 120 || request.title.contains(QChar('\n'))) {
    error = QStringLiteral(
        "Code image titles must be one line up to 120 characters");
    return false;
  }
  HighlightedCode highlighted;
  if (!highlightCodeWithNeovim(request, highlighted, error))
    return false;
  image = renderCodePanel(highlighted, request.title);
  detectedLanguage = highlighted.filetype;
  if (image.isNull()) {
    error = QStringLiteral("Could not render code image");
    return false;
  }
  return true;
}
