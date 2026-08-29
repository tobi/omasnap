# Editing model: everything is undoable, nothing is baked in until output

## The operation log is the source of truth

The image on screen while editing is not the working state — the operation
log (`ops_` in `CaptureEditor`, an ordered `QVector<Operation>` with an
`opIndex_` cursor) is. Every action that changes the picture — a crop, a
background choice, adding/patching/deleting an annotation, a cut, a clip — appends
one `Operation`. Undo moves `opIndex_` back; redo moves it forward.
`CaptureEditor::replayLog()` rebuilds the entire visible state (selection,
background, annotation list, cuts) by replaying the log from empty up to
`opIndex_`, every time. There is no separate "current annotations" model
that the log merely mirrors — the log **is** the model.

This is why undo is exact and unlimited within a session, why the crash
recovery snapshot (`snapshotWatcher_`) can save just the source image plus
the JSON-serialized log and reconstruct everything on reopen (see
`recent-snaps.hpp`), and why new annotation kinds or edit actions must be
expressed as a new `Operation::Type` (or a field on `Annotation`) rather
than a side channel of editor state — anything that bypasses the log breaks
undo, crash recovery, and the recents shelf simultaneously.

## Rendering is a pure function of the log, until you export

`renderCapture(capture, selection, annotations, backgroundStyle)` in
`src/capture.cpp` takes the pristine source image and the current vector
state and produces a flattened `QImage`. It runs constantly during editing
(every paint, effectively) and is cheap enough to do so because it never
mutates its inputs. **Nothing is baked into the working image as you draw.**
Add a rectangle, change your mind, delete it — the source pixels underneath
were never touched.

Output happens at exactly three moments, all user-initiated: **Copy**,
**Save**, or both together (`CaptureEditor::finish()`), plus pinning a
snapshot. Each of those calls `renderCapture` once, off the UI thread (see
[threading.md](threading.md)), and writes the result. Until one of those
happens, everything remains a log entry you can undo.

## The two exceptions, and why they're still safe

Two operations *do* need to touch real pixels before export, and both are
still fully undoable because of how they're kept in the log:

- **Cut** (`Operation::Type::Cut`) removes a band of the image and shifts
  everything after it. The working image (`capture_.source`) after a cut is
  `composeCuts(pristineSource_, cuts_)` — recomputed from the untouched
  original plus the list of cut ops every time the list changes
  (`CaptureEditor::refreshComposedCapture()`). Undo a cut and the composed
  image is rebuilt without it; `pristineSource_` was never modified.
- **Clip** (`Operation::Type::Clip`) copies a **path** of native pixels
  (rectangle, ellipse, or lasso polygon) and fills that hole — transparent by
  default, or a solid colour from the selection fly-out — then adds those
  pixels as a `Annotation::Kind::Clip` layer at the drop location. Pixels
  outside the path on the lifted tile are alpha 0. The hole does **not**
  collapse the image (that is Cut). The torn-off tile is kept on the Clip
  annotation (and in JSON). Replay applies cuts and clip fills in order and
  recopies from the composed image only when that tile is missing or the
  cut/clip prefix before the op changed, so later cuts cannot rewrite an
  already-torn piece. `refreshComposedCapture()` uses the same path fill.
  Undo a clip and both the hole and the layer disappear together. Live drag is
  editor-only preview; the log is touched only on release. Shape is stored as
  `shape` (`rect` omitted, `ellipse`, `lasso` plus `points`); fill as HexArgb
  when opaque. Click-to-snap / Snap-on drag fits the current clip shape
  (rounded rect, ellipse, or silhouette) with a one-lap scan-dot, then the
  mask locks. Rect stores optional `radius`.
- **Redaction** exists to permanently destroy sensitive content, so it is
  the one place where "non-destructive until export" would be a bug, not a
  feature: `renderCapture` applies redactions to the cropped pixels
  **before** painting ordinary vector annotations, and does so on every
  render, not just the final export. A redaction annotation is still a
  normal log entry — delete it before you export and the pixels underneath
  reappear from the pristine source, exactly like any other undo. But at
  the moment you do export, the destroyed pixels must actually be gone from
  the output. The code comment in `src/capture.cpp` is the rule: redactions
  "must never be approximated by a translucent overlay ... because that
  could leave recoverable source data in exports." Never soften a
  redaction's pixel destruction into a paint-time visual effect to make it
  cheaper or prettier — that reintroduces the leak this feature exists to
  prevent.

## What this means for new features

- New annotation kinds: add a field/kind, extend `renderCapture` and
  `replayLog`; don't add editor-only state that isn't in the log.
- New destructive operations (if ever needed): must recompute from
  `pristineSource_` the way cuts do, unless the entire point is irreversible
  pixel destruction like redaction — and if so, say so explicitly in a
  comment the way redaction's is written, because the next person touching
  that code needs the same warning.
- Anything that renders for output must go through `renderCapture`, not a
  parallel drawing path, or the two will drift.
