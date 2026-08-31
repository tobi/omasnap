/** @fileoverview Tests INI keybind loading: defaults, lists, rejection. */
#include "keybind-config-smoke.hpp"

#include "keybind-config.hpp"

#include <QFile>
#include <QTemporaryDir>

namespace {
bool writeFile(const QString &path, const QByteArray &contents) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) &&
         file.write(contents) == contents.size();
}
} // namespace

bool runKeybindConfigSmoke(QString &error) {
  QTemporaryDir dir;
  if (!dir.isValid()) {
    error = QStringLiteral("could not create temporary directory");
    return false;
  }
  const KeybindConfig defaults = defaultKeybindConfig();
  if (defaults.bindings.at(KeyAction::Marker).size() != 2 ||
      defaults.bindings.at(KeyAction::Redo).size() != 2 ||
      primaryKeyHint(defaults, KeyAction::DuplicateLayer) !=
          QStringLiteral("Alt+D")) {
    error = QStringLiteral("default keybinds lost an alias or hint");
    return false;
  }
  // Every default binding must actually parse; an empty sequence would
  // silently unbind the action.
  for (const auto &[action, sequences] : defaults.bindings) {
    if (sequences.empty()) {
      error = QStringLiteral("action %1 has no default binding")
                  .arg(keyActionName(action));
      return false;
    }
    for (const QKeySequence &sequence : sequences) {
      if (sequence.isEmpty()) {
        error = QStringLiteral("a default binding for %1 did not parse")
                    .arg(keyActionName(action));
        return false;
      }
    }
  }

  // Missing file -> defaults.
  const KeybindConfig missing =
      loadKeybindConfig(dir.filePath(QStringLiteral("absent.conf")));
  if (missing.bindings != defaults.bindings) {
    error = QStringLiteral("missing file did not fall back to defaults");
    return false;
  }

  // A full override replaces the bound list; hints follow the first entry.
  const QString full = dir.filePath(QStringLiteral("full.conf"));
  if (!writeFile(full, "[keys]\n"
                       "arrow=W\n"
                       "marker=N,M\n"
                       "redo=Ctrl+Shift+Z,Ctrl+Y\n")) {
    error = QStringLiteral("could not write full config");
    return false;
  }
  const KeybindConfig loaded = loadKeybindConfig(full);
  const std::vector<QKeySequence> expectedArrow = {
      QKeySequence(QStringLiteral("W"))};
  const std::vector<QKeySequence> expectedMarker = {
      QKeySequence(QStringLiteral("N")), QKeySequence(QStringLiteral("M"))};
  if (loaded.bindings.at(KeyAction::Arrow) != expectedArrow ||
      loaded.bindings.at(KeyAction::Marker) != expectedMarker ||
      loaded.bindings.at(KeyAction::Redo) !=
          defaults.bindings.at(KeyAction::Redo) ||
      primaryKeyHint(loaded, KeyAction::Arrow) != QStringLiteral("W")) {
    error = QStringLiteral("full keybind config not applied");
    return false;
  }

  // An unparsable key rejects the whole section, valid lines included.
  const QString invalid = dir.filePath(QStringLiteral("invalid.conf"));
  if (!writeFile(invalid, "[keys]\n"
                          "arrow=W\n"
                          "line=not+a+real+key\n")) {
    error = QStringLiteral("could not write invalid config");
    return false;
  }
  qInstallMessageHandler(
      [](QtMsgType, const QMessageLogContext &, const QString &) {});
  const KeybindConfig rejected = loadKeybindConfig(invalid);
  qInstallMessageHandler(nullptr);
  if (rejected.bindings != defaults.bindings) {
    error = QStringLiteral("unparsable entry did not reject the section");
    return false;
  }

  // Two actions of one scope sharing a key is rejected too.
  const QString conflict = dir.filePath(QStringLiteral("conflict.conf"));
  if (!writeFile(conflict, "[keys]\n"
                           "select=C\n"
                           "marker=C\n")) {
    error = QStringLiteral("could not write conflicting config");
    return false;
  }
  qInstallMessageHandler(
      [](QtMsgType, const QMessageLogContext &, const QString &) {});
  const KeybindConfig conflicted = loadKeybindConfig(conflict);
  qInstallMessageHandler(nullptr);
  if (conflicted.bindings != defaults.bindings) {
    error = QStringLiteral("cross-action conflict did not reject the section");
    return false;
  }

  // The same key twice within one action's own list is merely deduped.
  const QString repeat = dir.filePath(QStringLiteral("repeat.conf"));
  if (!writeFile(repeat, "[keys]\nmarker=C,C,M\n")) {
    error = QStringLiteral("could not write repeated config");
    return false;
  }
  const KeybindConfig deduped = loadKeybindConfig(repeat);
  const std::vector<QKeySequence> expectedDeduped = {
      QKeySequence(QStringLiteral("C")), QKeySequence(QStringLiteral("M"))};
  if (deduped.bindings.at(KeyAction::Marker) != expectedDeduped) {
    error = QStringLiteral("repeated entries were not deduped");
    return false;
  }

  // A listed action with no keys unbinds it; absent ones keep defaults.
  const QString unbind = dir.filePath(QStringLiteral("unbind.conf"));
  if (!writeFile(unbind, "[keys]\n"
                         "marker=\n"
                         "redo = \n")) {
    error = QStringLiteral("could not write unbind config");
    return false;
  }
  const KeybindConfig unbound = loadKeybindConfig(unbind);
  if (!unbound.bindings.at(KeyAction::Marker).empty() ||
      !unbound.bindings.at(KeyAction::Redo).empty()) {
    error = QStringLiteral("empty values did not unbind the action");
    return false;
  }
  if (unbound.bindings.at(KeyAction::Arrow) !=
      defaults.bindings.at(KeyAction::Arrow)) {
    error = QStringLiteral("unbinding one action disturbed another");
    return false;
  }

  // Cross-scope reuse stays legal: R and S mean different things per phase.
  const QString scopes = dir.filePath(QStringLiteral("scopes.conf"));
  if (!writeFile(scopes, "[keys]\nrectangle=R\nrestore-region=R\n")) {
    error = QStringLiteral("could not write scoped config");
    return false;
  }
  const KeybindConfig scoped = loadKeybindConfig(scopes);
  if (keyActionScope(KeyAction::Rectangle) != KeyScope::Edit ||
      keyActionScope(KeyAction::RestoreLastRegion) != KeyScope::Select ||
      scoped.bindings.at(KeyAction::Rectangle) !=
          std::vector<QKeySequence>{QKeySequence(QStringLiteral("R"))} ||
      scoped.bindings.at(KeyAction::RestoreLastRegion) !=
          std::vector<QKeySequence>{QKeySequence(QStringLiteral("R"))}) {
    error = QStringLiteral("cross-scope reuse was rejected or disturbed");
    return false;
  }
  return true;
}
