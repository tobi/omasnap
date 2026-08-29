# Clip-out shape masks

Date: 2026-08-28  
Branch: `feat/select-clip-out`  
Status: design locked; Fable + Sol P0s applied (2026-08-28)

## Problem

Clip-out today only locks a **rectangle**. Lifting DHH’s circular X avatar keeps the teal corners and punches a square hole. The job is to clip a **path** — rectangle, ellipse, or freehand lasso — see the hole before committing, then drag the masked pixels out as a layer with a real transparent (or solid) hole left behind.

Click-to-snap is the fast path for that circular avatar: click the object, get an adjustable ellipse, then lift.

## What this is not

- Not a general drawing program, magic-wand editor, or SAM-style subject cutout.
- Not a new linked library or model. Qt6 painter paths, a bounded contour fit, and the existing palette/eyedropper.
- Not a collapse-the-gap tool. That remains Cut (`X`).
- Not an opacity slider. A hole is transparent or a solid colour.

## User-visible behaviour

Stay in **Select** (`V`). Empty-canvas interaction locks a **pixel mask**, not a vector annotation.

1. Pick a clip shape (strip, keys, or cycle).
2. Draw it (or click to snap). A **dotted outline** and **handles** appear. Outside the path is dimmed; inside previews the hole fill (checkerboard if transparent, the swatch if solid).
3. Adjust handles. Change fill from the keyboard. The hole updates live.
4. Grab **inside** the path and **drag to lift**. Release near the hole to snap back (no log entry). Release elsewhere to commit one Clip op: punch/fill the hole, add a `Annotation::Kind::Clip` layer whose bitmap has alpha outside the path.
5. Repeat. `Ctrl+Z` undoes hole and layer together. `Esc` cancels an uncommitted mask.

Shift while drawing an ellipse still forces 1:1 (a circle), matching existing shape tools. Alt still draws from the centre.

### Shape strip and keys (Fable/Sol P0)

`R`, `E`, and `F` **keep arming the drawing tools on first press**, including the selected-shape fill toggle. Clip shape is **not** stolen from those keys. That is the Spotlight/Highlighter pattern: first press arms the tool; a second press on an already-armed tool cycles a variant. Select is already armed, so **`V` while in Select cycles the clip-shape variant.**

While Select is the current tool, a strip sits **above the toolbar**:

| Chip | How you pick it | Tooltip |
|---|---|---|
| Rect | `V` cycle / chip | Clip rectangle · V cycles · empty drag with no layers |
| Ellipse | `V` cycle / chip | Clip ellipse · V cycles · empty drag with no layers |
| Lasso | `V` cycle / chip | Clip lasso · V cycles · empty drag with no layers |
| Snap | chip toggle | Snap on/off · **on by default** · click an object (or refine a locked mask) |

`V` from any other tool enters Select on **Rect** (today’s default). `V` (and a second click of the Select toolbar button) **while already in Select** cycles Rect → Ellipse → Lasso → Rect. Snap is **not** in that cycle: it is an independent toggle, **on when Select starts**, so you can still draw Rect / Ellipse / Lasso with snap armed. Status pill names the shape and whether snap is on.

The **bottom-left hotkey legend** gains the clip rows while Select is on. Hover tooltips on the chips match those rows. No settings UI.

### Gestures (keep today’s Select arbitration)

Empty-canvas interaction is still **layer marquee first**:

1. A drag whose normalised rect is at least 2×2 and **encloses one or more layers** multi-selects those layers, **regardless of clip shape**. No pixel mask. For lasso, that rect is the **path bounding box**, not the start–end fidget.
2. The same drag with **no layers inside** locks a pixel mask in the current geometric shape (Rect / Ellipse / Lasso). Lasso keeps the traced vertices; a closed stroke that returns near the start still locks.
3. A press-and-release that never reaches a large-enough path/box is a **click**: it deselects, dismisses an uncommitted mask if the click is outside it, or **runs Snap when the Snap toggle is on** (and the click hits no layer).
4. **Snap while tracing:** the drag is the search window. Paper is sampled just **outside** that box; inside it, the connected object under the pointer grows 8-connected (so a checkerboard wheel is one blob, not one square) and a 1 px close fills dark cells. **Ellipse + Snap** then fits a circle that **covers that blob** (a wheel with a pin is enclosed, not cropped). Without a drag, click-to-snap still votes for a consistent radius so a round portrait on a card stays a circle, but keeps a small protrusion if the blob is only a bit larger than that circle. **Rect + Snap** uses the blob's AABB and its corner radius (a Messenger card locks a rounded rect on screen, not a sharp box). **Lasso + Snap** uses the blob silhouette. A blue scan-dot traces the outline, then the mask locks. Crop handles hide while that mask is locked.
5. `Alt+click` on empty canvas also snaps (modifier explicit), even if the toggle is off. Turning Snap on while a mask is locked refines from the mask centre.

Arming any drawing tool **clears** an uncommitted mask. Esc clears hex-entry first if it is open, then the mask, then Select, then the editor — same layered Esc as today.

### Fill (hole colour)

The clip fill fly-out is transparent + **match surroundings** (median colour just outside the mask) + palette + custom + **Sample from image** (same eyedropper as the toolbar). Default fill is **match surroundings** when a ring of page pixels exists, else transparent.

Fill keys apply **only while a dotted mask is locked** (Select or the clip eyedropper). Idle Select does not steal them: `T` still starts Text, `1`–`8` still set annotation colour. Arming a drawing tool clears the uncommitted mask. `I` while locked samples a fill colour **without** leaving Select (the lock stays visible). `#` opens a hex-entry field: digits go to the field, Enter commits, first Esc cancels typing only.

| Key | While a dotted mask is locked | Otherwise |
|---|---|---|
| `T` | Cycle hole fill: surroundings → Sample from image → transparent → surroundings | Text tool, unchanged |
| `1`–`8` | Palette fill (already wired on the rect PR) | Annotation colour, unchanged |
| `I` | Eyedropper, sample the screenshot | Eyedropper for annotation colour, unchanged |
| `#` | Type `#RRGGBB` (optional `#RGB`); Enter commits, Esc cancels typing | Ignored |

No `#AARRGGBB` and no alpha slider. No settings UI for these: Snap on and surroundings fill are the product defaults. `Esc` (or a snap-back lift) clears the mask; `T` is Text again.

Live preview: the interior of the path shows the fill immediately (checkerboard for transparent). Status names it (`Hole fill transparent · drag inside to clip out` / `Hole fill #E03131 · drag inside to clip out`).

## Engine

Live drag remains editor-only. The log is touched only on release that does not snap back. One `Operation::Type::Clip`. Undo/redo and recents-shelf replay stay exact because the op still reconstructs both hole and layer.

### `ClipOp`

Replace “always a rectangle” with a shape. Native pixels of the composed image at apply-time, same contract as Cut/clip today:

```
enum class ClipShape { Rect, Ellipse, Lasso };

struct ClipOp {
  ClipShape shape = ClipShape::Rect;
  QRect sourceRect;          // bbox; for Rect/Ellipse this is the shape
  QVector<QPointF> points;   // Lasso vertices in native space; empty otherwise
                             // Integer bbox is derived; quantize only when rasterizing
  QColor fill;               // invalid or alpha 0 = transparent punch
};
```

Snap is **not** a fourth shape. It produces an Ellipse `ClipOp` (and an ellipse mask the user can still resize) before lift.

JSON (`type: "clip"`):

- `sourceRect` as today `[x,y,w,h]`
- `shape`: `"rect"` | `"ellipse"` | `"lasso"` — omitted means `rect`
- `points`: array of `[x,y]` floats for lasso only
- `fill`: HexArgb when opaque, omitted when transparent
- `annotation`: dest rect of the lifted layer, as today

No migration shim. Omitted `shape` reads as rect so a working snapshot from the rect-only clip PR still reopens.

### Copy, punch, lift

New helpers in `src/clip.cpp` (keep `copyRect` / `punchRect` / `fillHole` as the rect special case, or thin wrappers):

- `clipPath(const ClipOp &) → QPainterPath` in native space.
- `copyMasked(source, op) → QImage` of `sourceRect` intersected with the image, Format_ARGB32_Premultiplied, pixels **outside** the path alpha 0.
- `fillHole(image, op)` fills the path with `fill`, or punches transparent when `!clipFillOpaque(fill)`.

The lifted `Annotation::Kind::Clip` **image** is that masked tile. `start`/`end` are the dest bbox in annotation space (same as today). Replay copies with `copyMasked` from the composed source **at that op**, then `fillHole` on the composed image, then attaches the tile. Later cuts cannot rewrite an already-torn piece.

Lasso close: if the pointer is near the start point on release, close; otherwise close with a straight segment. Degenerate paths (empty, < 3 points, zero area) are a no-op, no log entry.

Lasso **adjust** after lock is the eight bbox handles (scale/translate the polygon). No vertex editing.

### Click-to-snap

Naive flood-fill from a face click selects cheek, not the circular avatar. Snap must find the **object boundary**, not the local colour.

Algorithm, Qt only, **axis-aligned ellipse**, cheap enough for the UI thread (no contour extraction):

1. Map the click to native pixels.
2. Cast **36 rays** from the click, evenly spaced, up to 256 px or the image edge.
3. Along each ray, take the first pixel whose luma differs from the previous sample by more than 28 (of 255), or whose 3×3 neighbour contrast spikes. Record that point.
4. Need at least 12 hit rays. Fit an **axis-aligned ellipse** as the bounding ellipse of those points (min/max x/y). If width/height ratio is within 1.12, use a circle (1:1).
5. Reject if the ellipse covers more than 70% of the screenshot, has a radius under 6 px, or the click is not inside it. Status: `Nothing to snap · drag a shape instead`. No mask, no log entry.
6. Lock an Ellipse mask with handles.

Do not add OpenCV, ONNX, or a model. If a later measurement shows a hitch, move this to `QtConcurrent` with a generation token and discard stale results; do not start there.

`copyMasked` stamps any **redaction annotations already in the replayed list at this op** onto the copied tile before it becomes a layer, so a clip cannot lift pixels that a prior redaction already destroyed. `refreshComposedCapture()` must call the same `fillHole(image, ClipOp)` as `replayLog()` so Cut preview cannot reconstruct an ellipse/lasso as a rectangle.

### Threading

Unchanged: PNG encode, disk, `wl-copy` stay off the UI thread. Mask preview, handle math, and snap stay on the UI thread. Commit still appends one op and `replayLog()` rebuilds.

## Editor integration

- Pixel-clip state today is `pixelClipRect_` (logical QRectF). Generalise to a small editor-only struct: shape, logical rect, optional logical lasso points, fill, lift preview. Not a log entry until release.
- Handle hit-testing for ellipse uses the ellipse bbox handles (same eight as the current rect clip / ellipse annotations). Lasso uses the polygon’s bbox handles.
- Hole-fill fly-out stays next to the locked mask, including lasso/ellipse bboxes.
- `nativeClipRect` stays for mapping a logical rect to native. Lasso points map with the same floor/ceil scaling as cut/clip rects.

## Tests

Headless offscreen, in the existing smoke suite. Failures name the first wrong pixel.

- **Rect** — existing `clip-smoke` / `clip-mapping-smoke` still pass.
- **Ellipse** — synthetic circle (opaque disk on a solid field). Clip the disk with an ellipse mask; corners of the bbox are transparent; hole in the source is transparent (or the chosen fill); lifted layer composite matches the disk.
- **Lasso** — triangle or irregular polygon; outside-path pixels in the tile are alpha 0; hole matches the path.
- **Snap** — fixture of a high-contrast circle (the DHH-avatar case, simplified). Click the centre; fitted ellipse covers the disk and not the field. Click empty field; no op, status set.
- **Undo** — ellipse clip then `Ctrl+Z` restores hole and drops the layer.
- **Fill keys** — with a locked mask, `T` sets transparent; `1` sets palette[0]; `#` + hex + Enter sets custom. After Esc (no mask), `T` arms the text tool. Idle Select without a mask: `T` must not set clip fill.
- **V-cycle** — `E` from idle Select still arms `Tool::Ellipse`. `V` then `V` cycles clip shape while `tool_ == Select` (Rect → Ellipse → Lasso → Rect). `R` with a rectangle layer selected still toggles fill.
- **Snap toggle** — Snap chip does not steal Rect/Ellipse/Lasso. Rect + Snap around a square locks a rectangle; around a rounded rect the tile corners are transparent. Ellipse + Snap around a disk locks an ellipse. Lasso + Snap around a disk locks a silhouette path. A closed lasso over stripes still keeps the hand-drawn path.
- **Lasso release** — a freehand that returns near its start keeps the path; lock uses the path bbox, not the start–end marquee.

`make check` after the behavioural change.

## Docs to update in the same work

- `README.md` — clip-out bullet and the Select / `V` / `R` / `E` / `F` / `T` / `#` rows.
- `docs/editing-model.md` — Clip paragraph: path, alpha tile, snap → ellipse.
- `AGENTS.md` layout line for `src/clip.cpp` if the file overview changes.
- `docs/dependencies.md` — no new rows. If snap stays Qt-only it does not belong there.

## Out of scope

- Neural / SAM / OpenCV subject cutout.
- Vertex-level lasso editing.
- Animated marching ants (a dashed stroke is enough).
- Fill alpha other than 0 or 255.
- Compositor-specific snap (Wayland window outlines). Hyprland-only remains the product; this feature is screenshot pixels, not `hyprctl`.
- Changing Cut, redaction, or pin.

## Files (implementation, not this spec)

| File | Role |
|---|---|
| `src/clip.hpp` / `src/clip.cpp` | Shape, path, copyMasked, fillHole, snap-fit |
| `src/capture.hpp` / `src/capture.cpp` | JSON for shape + points |
| `src/editor.cpp` / `src/editor.hpp` | Strip, keys, legend, preview, lift |
| `tests/clip-smoke.cpp` | Engine pixels |
| `tests/clip-mapping-smoke.cpp` | Editor mapping, snap fixture, undo, keys |
| `README.md`, `docs/editing-model.md` | User-facing + log contract |
