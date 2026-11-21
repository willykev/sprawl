# Changelog

## Unreleased

Fixes from a security and data-integrity review.

### Safety

- A board shows and exports only pictures that are plain files in its own
  `assets` folder. A link there, or an `assets` folder that is itself a link,
  is shown as a missing picture.
- An SVG that draws in pictures or style sheets from other files is not
  shown, exported or taken in, because Qt would read those files from
  wherever the SVG points.
- "Export what is selected" exports what was selected when the formats came
  up. If all of it is gone by the time a format is chosen, nothing is
  exported. Before, the whole board was.
- `sprawl copy <board> <folder>` copies a board with only the pictures that
  are on it. A board's own folder keeps pictures taken off it, for undo.
- `sprawl export` refuses to write over the board it exports, even through
  a link to the board file.
- Sprawl reads a picture before copying it in. `sprawl check` reports
  pictures on a board that it cannot read.

### Saving

- A board renamed in another window is followed there, even when a
  different board has taken its old name.
- Moving a board to the trash saves its latest changes first.
- A picture pasted from another board is always the one copied, even when
  the board it is pasted on has a different picture with the same name.
- Groups made in two windows at once stay two groups.
- Pictures no longer on the board are let go of from memory past 256 MiB.
- A board still opens when a shape grows or a connector reaches past the
  file's coordinate limit, or a group is made at the group ID limit.
- Each board keeps the pan and zoom it was left at when several windows use
  the same library.
- New Board saves the open board before making another, so a failed save
  leaves the library as it was.
- Board names keep letters such as `𠮷` in `𠮷野`.

### The canvas

- Undo and redo keep the order of the remaining items when one was taken
  off the board elsewhere.
- An emoji pasted at the text limit is left out whole when it will not fit.
- Box selection picks up a pen stroke when the box touches its ink.

## 0.1.2 — 2026-11-16

A new README and a fuller example. Sprawl itself is unchanged.

- The README shows Sprawl at work: a whole board, lines following a shape
  as it moves, the pen, the board switcher, and one board in four themes.
  It adds a short tour, measured performance, and how to update and remove
  Sprawl.
- The weekend-trip example gains a dashed connector and a pen sketch.
- `bin/screenshots` redraws the README's pictures from the example board,
  off screen and in Omarchy's default theme.

## 0.1.1 — 2026-11-14

Fixes from a full code and security review.

### Safety

- What a board file says is only ever shown as text: nothing in it is read
  as markup, the window fetches nothing from the network, and what the
  commands print cannot drive the terminal.
- Only pictures are taken in as pictures, from files or the clipboard. A
  board's name is never a path, and exports only ever write to files.
- An item holds 10,000 characters at most, and the text being written
  counts toward what a frame may ask of the GPU.

### Saving

- Work in progress is saved: text being typed and things being dragged are
  written within moments, and closing the window mid-edit keeps the edit.
- Changes from outside (a `sprawl` command, another window, an agent) are
  merged with what the window holds instead of being written over, and a
  board's file is locked while anything writes it.
- A board that could not be read opens read only, says why, and is never
  written over.
- No box is saved too small to be read back, and boards that have one open
  again.

### The canvas

- Nothing changes mid-drag, and nothing changes on a read-only board.
- Copying, deleting and moving a big selection no longer slows down with
  its size.
- Paste takes what it can, says what it left out, and joins copied
  connectors as they go down.
- A board's first view fits the window it opens in, and writing survives
  the window losing focus.
- The board switcher lists boards only to pick one, and typing a board's
  whole name puts it first.

### Drawing and performance

- The stroke being drawn counts toward each frame's GPU budget, so a long
  stroke on a dense board can no longer overload the GPU.
- Text hidden to keep a frame in budget comes back as soon as there is
  room, and hiding pen strokes hides only pen strokes.
- Pen strokes come to a point at the corners the hand turns, even drawn
  fast, and stay smooth along curves; letting go of a stroke no longer
  changes how it looks. Strokes saved by 0.1.0 keep the rounded corners
  they were saved with: draw them again to sharpen them.
- Text is placed and culled by its measured size, notes and shapes read
  from a file grow to hold their text, big items are culled one by one, a
  picture is handed to the GPU once, the first frame does not wait for text
  out of view, and the dot grid sits on the board.
- `sprawl open --probe` and `--bench` keep to what is known to be safe for
  the GPU. No frame is ever allowed more than 16 MB, twice the budget,
  whoever asks, and a made-up board has 100,000 items at most. What makes
  no sense is refused, saying what they take.

### Exports, commands and starting up

- Exports are the right size and crop nothing: PDF pages are exact, SVG
  view boxes whole, and labels whole.
- `sprawl add` puts things where it is told and refuses what no board
  could hold, a missing board is named as missing, and an export asks
  before replacing a file.
- A board's name means that board, whatever the folder a command runs in
  holds, and options such as `--theme` open a window of their own.
- The package depends on `qt6-imageformats`, so WebP pictures can be
  added, and lists the optional packages that read AVIF, HEIC and JPEG XL.

### Theme

- Everything follows a theme switch, color chips, badges and type sizes
  included, and your own Omarchy shell settings are laid over the theme's,
  as the shell does.

## 0.1.0 — 2026-11-13

First public release.

- Infinite boards with sticky notes, text, pictures, frames, and pen strokes.
- Shapes and attached connectors for diagrams.
- Selection, grouping, undo/redo, and automatic local saves.
- PNG, SVG, and PDF export; CLI commands and a sample weekend-trip board.
- Live Omarchy theming and a GPU-rendered Qt Quick canvas.
