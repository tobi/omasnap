/** @fileoverview Editor keybindings from the user's INI config ([keys]). */
#include "keybind-config.hpp"

#include <QSettings>
#include <algorithm>
#include <array>

namespace {
constexpr std::array kAllKeyActions = {
    KeyAction::Select,
    KeyAction::Arrow,
    KeyAction::Line,
    KeyAction::Freehand,
    KeyAction::Highlighter,
    KeyAction::Marker,
    KeyAction::Rectangle,
    KeyAction::Ellipse,
    KeyAction::Spotlight,
    KeyAction::Redact,
    KeyAction::Cut,
    KeyAction::Text,
    KeyAction::Eyedropper,
    KeyAction::Ocr,
    KeyAction::Pin,
    KeyAction::Backdrop,
    KeyAction::DuplicateLayer,
    KeyAction::RestoreLastRegion,
    KeyAction::ToggleScrollMode,
    KeyAction::CycleSelectTab,
    KeyAction::Color1,
    KeyAction::Color2,
    KeyAction::Color3,
    KeyAction::Color4,
    KeyAction::Color5,
    KeyAction::Color6,
    KeyAction::Color7,
    KeyAction::Color8,
    KeyAction::Undo,
    KeyAction::Redo,
    KeyAction::Copy,
    KeyAction::Save,
    KeyAction::SelectAll,
    KeyAction::ZoomIn,
    KeyAction::ZoomOut,
    KeyAction::ZoomFit,

};
} // namespace

KeybindConfig defaultKeybindConfig() {
  KeybindConfig config;
  const auto put = [&config](KeyAction action,
                             std::initializer_list<const char *> keys) {
    std::vector<QKeySequence> sequences;
    sequences.reserve(keys.size());
    for (const char *key : keys)
      sequences.emplace_back(QLatin1String(key));
    config.bindings.emplace(action, std::move(sequences));
  };
  put(KeyAction::Select, {"V"});
  put(KeyAction::Arrow, {"A"});
  put(KeyAction::Line, {"L"});
  put(KeyAction::Freehand, {"F"});
  put(KeyAction::Highlighter, {"H"});
  put(KeyAction::Marker, {"C", "M"});
  put(KeyAction::Rectangle, {"R"});
  put(KeyAction::Ellipse, {"E"});
  put(KeyAction::Spotlight, {"S"});
  put(KeyAction::Redact, {"D"});
  put(KeyAction::Cut, {"X"});
  put(KeyAction::Text, {"T"});
  put(KeyAction::Eyedropper, {"I"});
  put(KeyAction::Ocr, {"O"});
  put(KeyAction::Pin, {"P"});
  put(KeyAction::Backdrop, {"B"});
  put(KeyAction::DuplicateLayer, {"Alt+D"});
  // Select-phase keys. Sharing R and S with edit-phase tools is fine: the
  // scopes never match in the same pass.
  put(KeyAction::RestoreLastRegion, {"R"});
  put(KeyAction::ToggleScrollMode, {"S"});
  put(KeyAction::CycleSelectTab, {"Space"});
  put(KeyAction::Color1, {"1"});
  put(KeyAction::Color2, {"2"});
  put(KeyAction::Color3, {"3"});
  put(KeyAction::Color4, {"4"});
  put(KeyAction::Color5, {"5"});
  put(KeyAction::Color6, {"6"});
  put(KeyAction::Color7, {"7"});
  put(KeyAction::Color8, {"8"});
  put(KeyAction::Undo, {"Ctrl+Z"});
  put(KeyAction::Redo, {"Ctrl+Shift+Z", "Ctrl+Y"});
  put(KeyAction::Copy, {"Ctrl+C"});
  put(KeyAction::Save, {"Ctrl+S"});
  put(KeyAction::SelectAll, {"Ctrl+A"});
  // Bare zoom keys stay the defaults on purpose: a remote keyboard bridge may
  // inject modifiers in a way the compositor never publishes as xkb state.
  put(KeyAction::ZoomIn, {"+", "=", "Ctrl+="});
  put(KeyAction::ZoomOut, {"-", "_", "Ctrl+-"});
  put(KeyAction::ZoomFit, {"0", "Ctrl+0"});
  return config;
}

QString keyActionName(KeyAction action) {
  switch (action) {
  case KeyAction::Select:
    return QStringLiteral("select");
  case KeyAction::Arrow:
    return QStringLiteral("arrow");
  case KeyAction::Line:
    return QStringLiteral("line");
  case KeyAction::Freehand:
    return QStringLiteral("freehand");
  case KeyAction::Highlighter:
    return QStringLiteral("highlighter");
  case KeyAction::Marker:
    return QStringLiteral("marker");
  case KeyAction::Rectangle:
    return QStringLiteral("rectangle");
  case KeyAction::Ellipse:
    return QStringLiteral("ellipse");
  case KeyAction::Spotlight:
    return QStringLiteral("spotlight");
  case KeyAction::Redact:
    return QStringLiteral("redact");
  case KeyAction::Cut:
    return QStringLiteral("cut");
  case KeyAction::Text:
    return QStringLiteral("text");
  case KeyAction::Eyedropper:
    return QStringLiteral("eyedropper");
  case KeyAction::Ocr:
    return QStringLiteral("ocr");
  case KeyAction::Pin:
    return QStringLiteral("pin");
  case KeyAction::Backdrop:
    return QStringLiteral("backdrop");
  case KeyAction::DuplicateLayer:
    return QStringLiteral("duplicate");
  case KeyAction::RestoreLastRegion:
    return QStringLiteral("restore-region");
  case KeyAction::ToggleScrollMode:
    return QStringLiteral("scroll-mode");
  case KeyAction::CycleSelectTab:
    return QStringLiteral("cycle-tab");
  case KeyAction::Color1:
    return QStringLiteral("color1");
  case KeyAction::Color2:
    return QStringLiteral("color2");
  case KeyAction::Color3:
    return QStringLiteral("color3");
  case KeyAction::Color4:
    return QStringLiteral("color4");
  case KeyAction::Color5:
    return QStringLiteral("color5");
  case KeyAction::Color6:
    return QStringLiteral("color6");
  case KeyAction::Color7:
    return QStringLiteral("color7");
  case KeyAction::Color8:
    return QStringLiteral("color8");
  case KeyAction::Undo:
    return QStringLiteral("undo");
  case KeyAction::Redo:
    return QStringLiteral("redo");
  case KeyAction::Copy:
    return QStringLiteral("copy");
  case KeyAction::Save:
    return QStringLiteral("save");
  case KeyAction::SelectAll:
    return QStringLiteral("select-all");
  case KeyAction::ZoomIn:
    return QStringLiteral("zoom-in");
  case KeyAction::ZoomOut:
    return QStringLiteral("zoom-out");
  case KeyAction::ZoomFit:
    return QStringLiteral("zoom-fit");
  }
  return {};
}

KeyScope keyActionScope(KeyAction action) {
  switch (action) {
  case KeyAction::RestoreLastRegion:
  case KeyAction::ToggleScrollMode:
  case KeyAction::CycleSelectTab:
    return KeyScope::Select;
  default:
    return KeyScope::Edit;
  }
}

KeybindConfig loadKeybindConfig(const QString &filePath) {
  KeybindConfig config = defaultKeybindConfig();
  QSettings settings(filePath, QSettings::IniFormat);
  settings.beginGroup(QStringLiteral("keys"));
  const QStringList childKeys = settings.childKeys();
  if (childKeys.isEmpty())
    return config;

  // Parse everything before touching the defaults so any bad entry rejects
  // the whole section rather than half-applying.
  QString offender;
  bool ok = true;
  std::map<KeyAction, std::vector<QKeySequence>> parsed;
  for (const QString &child : childKeys) {
    const auto action = std::find_if(
        kAllKeyActions.begin(), kAllKeyActions.end(),
        [&](KeyAction candidate) { return keyActionName(candidate) == child; });
    if (action == kAllKeyActions.end())
      continue; // Unknown names are ignored, like unknown keys elsewhere.
    std::vector<QKeySequence> sequences;
    const QStringList values = settings.value(child).toStringList();
    for (const QString &value : values) {
      const QString trimmed = value.trimmed();
      if (trimmed.isEmpty())
        continue;
      const QKeySequence sequence = QKeySequence::fromString(trimmed);
      // Qt parses garbage like "not+a+real+key" into an unknown key without
      // complaint; only a renderable sequence actually names a key.
      if (sequence.isEmpty() ||
          sequence.toString(QKeySequence::PortableText).isEmpty()) {
        offender = trimmed;
        ok = false;
        break;
      }
      if (std::find(sequences.begin(), sequences.end(), sequence) ==
          sequences.end())
        sequences.push_back(sequence);
    }
    if (!ok)
      break;
    // A listed action with no keys (e.g. "marker =") unbinds it; an absent
    // one keeps its defaults.
    parsed.emplace(*action, std::move(sequences));
  }

  if (ok) {
    // A key bound to two actions of one scope would make the match order
    // decide, which is exactly what a config should not depend on.
    for (auto it = parsed.begin(); it != parsed.end() && ok; ++it) {
      for (auto other = std::next(it); other != parsed.end(); ++other) {
        if (keyActionScope(it->first) != keyActionScope(other->first))
          continue;
        for (const QKeySequence &sequence : it->second) {
          if (std::find(other->second.begin(), other->second.end(), sequence) !=
              other->second.end()) {
            offender = sequence.toString();
            ok = false;
            break;
          }
        }
        if (!ok)
          break;
      }
    }
  }

  if (!ok) {
    qWarning("omasnap: rejecting [keys] section of %s: %s",
             qUtf8Printable(filePath), qUtf8Printable(offender));
    return config;
  }
  for (auto &[action, sequences] : parsed)
    config.bindings[action] = std::move(sequences);
  return config;
}

QString primaryKeyHint(const KeybindConfig &config, KeyAction action) {
  const auto found = config.bindings.find(action);
  if (found == config.bindings.end() || found->second.empty())
    return {};
  return found->second.front().toString(QKeySequence::PortableText);
}
