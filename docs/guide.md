# Sprawl guide

An infinite canvas for the Omarchy desktop, in the spirit of Freeform: boards
that never end, for sticky notes, text, shapes joined by arrows, pen strokes,
pictures, and frames to gather them in. It is driven from the keyboard, looks
like the rest of Omarchy whatever theme is on, saves itself, and can be read
and written from the command line.

## What it does

- **Boards without edges.** Scroll to pan, pinch or Ctrl+scroll to zoom from
  10% to 800%, `1` to see the whole board, `0` for actual size. Each board
  opens where it was last left.
- **Notes** in the theme's seven colors, whose first line is a title, and that
  grow to hold what is written on them. **Text** on its own, from caption to
  display size. **Shapes**, nineteen of them: rectangles square, rounded and
  pill-ended, ellipse, diamond, triangles, parallelogram, trapezoid,
  pentagon, hexagon, octagon, star, cross, block arrows, speech bubble, and
  cylinder. They come out in a grid beside the shape tool to pick from, and
  a shape on the board can be made another from the bar by the selection.
- **Connectors** that stay attached: a line drawn from one thing to another
  follows both when they move, leaves each at its edge, and can carry a
  label. It goes **straight**, in a **curve**, or as an **elbow** with square
  corners; it is **solid, dashed, or dotted**; and each end is picked on its
  own: nothing, an arrow, an open arrow, a triangle, a dot, a circle, a
  diamond, an open diamond, or a bar. A selected connector's four choices
  are on the bar beside it, and the connector tool's are on a bar of its own.
- **Pen** strokes, smoothed as they are drawn, in the theme's colors and four
  widths. **Pictures** from a file dialog, a drop, or the clipboard, kept with
  the board. **Frames**: a titled border that carries what sits inside it.
- **Select, move, resize, group, restack, duplicate, copy and paste**, with
  undo and redo for every step. Anything new lands under the pointer.
- **Boards** switched by typing a few letters of a name (`Ctrl+O`), made,
  renamed, and moved to the trash from the same place.
- **Export** to PNG, SVG, or PDF: the whole board, or what is selected.
- **Saves as you go.** There is no save command and nothing to lose: a board
  is written a moment after every change, off to one side, so the window
  never stutters for it.

## Looks like Omarchy

Sprawl has no colors or fonts of its own. It reads the current theme and
follows it live, the way the Omarchy shell does:

- **The whole palette** from the theme's `colors.toml`. Notes, shapes, and
  strokes are colored by name (yellow, blue, ...), so a board changes shade
  with the theme, and note paper is toned so that dark writing always reads
  on it. Light themes work.
- **The shell's controls**, from `shell.toml`: panels, rows, and key caps are
  drawn with the same fills, hairline borders, and selected states as the
  shell's own menus.
- **A rail and a mode line** rather than toolbars: the tools down the left
  edge with their keys, and along the bottom the mode in its color, the
  board, what is selected, whether it is saved, where the pointer is, and
  the zoom.
- **The Omarchy font**, at the size the theme gives the shell.
- **Hyprland's corners, border, and motion**: square where windows are
  square, and nothing glides when animations are off.

## Install

```sh
bin/install
```

This builds Sprawl and installs it as an Arch package (`makepkg -fsi`), so it
shows up in the Omarchy app launcher (`Super + Space`). It needs `qmake6`,
`make`, a C++ compiler, and Qt's base, declarative, SVG, and Wayland modules,
all of which are part of Omarchy.

## Shortcuts

`?` shows these in the window.

| Key | Does |
| --- | --- |
| `V` `H` | Select, pan |
| `N` `T` `S` `P` `C` `F` | Note, text, shape, pen, connector, frame |
| `S` again, `Shift+S`, arrows | With the shapes out: the next shape, the one before, and around the grid. `Return` puts them away. |
| `C` again | With the connector tool: the next route, straight, curved, elbow |
| `I` | Add pictures |
| `Space` | New note under the pointer |
| `Return` | Edit the text of what is selected. `Esc` when done; in a note, `Ctrl+Return` too. |
| `Ctrl+Z` / `Ctrl+Shift+Z` | Undo / redo |
| `Ctrl+C` `Ctrl+X` `Ctrl+V` | Copy, cut, paste. Paste also takes a picture or text from anywhere. |
| `Ctrl+D` | Duplicate |
| `Delete` or `Backspace` | Delete |
| `Ctrl+A` | Select all |
| `←` `↑` `↓` `→` | Move what is selected, or the view when nothing is. With `Shift`, further. |
| `Ctrl+G` / `Ctrl+Shift+G` | Group / ungroup |
| `]` / `[` | Bring to front / send to back. With the pen: thicker / thinner. |
| `Ctrl+B` | Bold text |
| `Ctrl++` / `Ctrl+-` | Bigger / smaller text |
| `+` / `-` | Zoom in / out, toward the pointer |
| `0` / `1` | Actual size / fit the board |
| `Ctrl+O` | Switch board. In it: `Ctrl+N` new, `F2` rename, `Ctrl+Delete` to the trash. |
| `Ctrl+N` | New board |
| `Ctrl+E` | Export |
| `Esc` | Back out: of a drag, of typing, of a tool, of the selection |
| `Q` | Quit |

With the mouse: drag to move or to select what a box touches, `Shift`+click
to add to or take from the selection, double-click to edit or, on the empty
board, to write. Handles resize; `Shift` keeps the proportions. The middle
button drags the view. Scroll pans, `Shift`+scroll pans sideways.

## From the command line

Every command works without a display, takes `--json`, exits 0 or 1, and
writes errors to stderr. The window shows a change at once if the board is
open in it.

```sh
sprawl new weekend-trip
sprawl add weekend-trip note --text "Pack\nboots, map" --color green
sprawl add weekend-trip shape --shape diamond --text "Rain?" --x 240 --y 40
sprawl add weekend-trip connector --from 1 --to 2 --text "check" --route elbow --dash dashed
sprawl show weekend-trip
sprawl export weekend-trip -o trip.pdf
sprawl open weekend-trip
```

`sprawl help` lists the commands and `sprawl help add` a command's options.
`sprawl skill install` installs a short skill so coding agents can lay out
boards for you.

## Where things are

A board is a folder in `~/.local/share/sprawl/boards`: a `board.json`, which
is plain enough to write by hand, and an `assets` folder with its pictures.

```json
{"sprawl": 1, "items": [
  {"id": 1, "type": "note", "x": 0, "y": 0, "color": "yellow", "text": "Idea\nwhat it is"},
  {"id": 2, "type": "shape", "x": 240, "y": 48, "shape": "diamond", "text": "Good?"},
  {"id": 3, "type": "connector", "from": 1, "to": 2, "text": "then"}
]}
```

Items are listed back to front; a unit is a pixel at 100%. `sprawl check`
says what is wrong with a file that will not open, and a board that cannot be
read is never written over. Which board was open, and where each was left, is
in `~/.local/state/sprawl/state.json`. Setting `SPRAWL_HOME` keeps boards and
state somewhere else.

A board from somewhere else is only ever read as a board. What it says, and
what it and its pictures are called, is shown as written and never as markup;
only a picture is ever kept as one, wherever a board or the clipboard says to
look; an item holds 10,000 characters at most; and what the commands print
of it has nothing in it that a terminal obeys.

## How it draws

Sprawl is C++ and Qt Quick, and the canvas is a single item that builds its
own scene graph: shaders for the dot grid, for shapes (one quad each, their
edges worked out per pixel), and for pen strokes (widened on the GPU, so they
are never thinner than a pixel). Panning and zooming change one matrix.

A board is drawn in layers of things that do not overlap, cut into squares of
the board, so that a screenful is a few dozen draw calls whatever is on it.
Only the parts of the board near the window are handed to the GPU. What is
too small to make out is left out: type under five pixels tall, and the fine
curves of pen strokes, which are kept at three levels of detail.

And a frame has a budget: about 65,000 triangles. That is not a matter of
speed. A tile-based GPU keeps every triangle of a frame in a buffer of fixed
size until the frame is drawn, and on Asahi Linux the Apple GPU driver does
not survive that buffer filling up: the desktop goes with it. Frames within
the budget were measured on an M3 Max not to make that buffer grow at all.
When more is in view than fits (hundreds of long pen strokes at once, or
notes piled thousands deep), type goes first, then the curves of strokes,
then the strokes, and the line at the foot of the window says
`zoom in for full detail`. The one thing the canvas does not draw is the
text being written: the editor does, every letter of it whether in view or
not, which is why an item holds only so much, and what the editor is holding
comes out of the budget for as long as it is open.

On that machine, on a made-up board of 48,000 items, panning, zooming all the
way out and back, and dragging an item each keep up with its 120 Hz display
(118 to 119 frames a second, measured with `--bench`).

```sh
bin/build                      # build/sprawl
bin/test                       # the tests, with no display
sprawl open --bench 10000      # time frames on a made-up board of that size
sprawl open --probe 8          # Asahi: what a frame of 8 MB costs the GPU's buffer
sprawl open --probe 2 --probe-with writing   # the same, of a note in the editor
sprawl open --theme gruvbox    # look at it in another Omarchy theme
bin/shaders                    # after editing a shader (needs qt6-shadertools)
```

## What it does not do

Boards live on this machine: there is no sync, sharing, or working on one
board together, and Sprawl never uses the network (its interface is not
given one to use). There are no tables, links between boards, rotation, or
alignment guides; text has one weight and size per item; the pen has no
pressure and there is no eraser other than deleting a stroke. The window is
in English until someone translates it: every string is ready to be, and
Sprawl follows the Omarchy language as it follows the theme.
