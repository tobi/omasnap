/** @fileoverview Editor keybindings from the user's INI config ([keys]). */
#pragma once

#include <QKeySequence>
#include <QString>
#include <cstdint>
#include <map>
#include <vector>

/** Every editor action bindable under [keys] in omasnap.conf. */
enum class KeyAction : std::uint8_t {
  Select,
  Arrow,
  Line,
  Freehand,
  Highlighter,
  Marker,
  Rectangle,
  Ellipse,
  Spotlight,
  Redact,
  Cut,
  Text,
  Eyedropper,
  Ocr,
  Pin,
  Backdrop,
  DuplicateLayer,
  RestoreLastRegion,
  ToggleScrollMode,
  CycleSelectTab,
  Color1,
  Color2,
  Color3,
  Color4,
  Color5,
  Color6,
  Color7,
  Color8,
  Undo,
  Redo,
  Copy,
  Save,
  SelectAll,
  ZoomIn,
  ZoomOut,
  ZoomFit,
};

/** Which handler group an action is matched in. A key may repeat across
 *  scopes — R restores the last region while selecting and draws rectangles
 *  while editing — but never twice within one scope, which loadKeybindConfig
 *  treats as a rejected config. */
enum class KeyScope : std::uint8_t { Edit, Select };

struct KeybindConfig {
  /** Ordered bindings per action; the first entry drives hints. */
  std::map<KeyAction, std::vector<QKeySequence>> bindings;
};

/** The bindings previously hardcoded in editor.cpp, aliases included. */
[[nodiscard]] KeybindConfig defaultKeybindConfig();

/** Reads [keys]: `name=key[,key...]` with portable QKeySequence names
 *  (`arrow=A`, `redo=Ctrl+Shift+Z,Ctrl+Y`). A missing file or section leaves
 *  defaults untouched. A listed action with no keys ("marker =") unbinds it.
 *  Any unparsable key, or one bound to two actions of the same scope, rejects
 *  the whole section: defaults come back and a warning names the offender.
 *  Unknown action names are ignored. */
[[nodiscard]] KeybindConfig loadKeybindConfig(const QString &filePath);

/** INI key for `action`, e.g. "restore-region". */
[[nodiscard]] QString keyActionName(KeyAction action);

/** Which scope `action` is matched in (see KeyScope). */
[[nodiscard]] KeyScope keyActionScope(KeyAction action);

/** Display form of `action`'s first binding ("Alt+D"); empty when unbound.
 *  Status lines and tooltips use this so hints follow rebinds. */
[[nodiscard]] QString primaryKeyHint(const KeybindConfig &config,
                                     KeyAction action);
