# The overview

hyprgrid's camera zoomed out: the whole grid on screen, every workspace on
its cell with its windows live. Pick a window, drag it to another workspace
or into an empty cell, resize it. It is part of hyprgrid: nothing to load
apart.

It derives from [hyprland-scroll-overview](https://github.com/yayuuu/hyprland-scroll-overview)
by Vaxry, yayuuu and its contributors, whose rendering is the base of this
one; it no longer follows upstream (see [Where it came from](#where-it-came-from)).

## Contents

- [Opening it](#opening-it)
- [Using it](#using-it)
- [Options](#options)
- [Actions](#actions)
- [Your own keys inside the overview](#your-own-keys-inside-the-overview)
- [How it is built](#how-it-is-built)
- [Where it came from](#where-it-came-from)

## Opening it

The overview is the view's zoom (0 normal, 1 zoomed out), per monitor: any
rule driving the zoom opens it. hyprgrid's default policy has the touchpad
ones: four fingers up zoom out, following the fingers, and down zoom back in
(`overview.touchpad` in `setup()`). Zoomed out, three fingers pan the grid.
For the mouse, these are the ones in its design notes:

```lua
local g = hl.plugin.hyprgrid
-- SUPER + wheel zooms out (down) and in (up); one notch is enough.
g.rule({ when = { mod = "SUPER" }, drive = { wheel = "zoom" } })
-- SUPER + middle-drag moves the grid; scrolling meanwhile zooms.
g.rule({ when = { button = "middle" }, start = { mod = "SUPER" }, drive = { motion = "position", wheel = "zoom" }, cursor = "hold" })
```

Or a key, through its action (see [Actions](#actions)):

```lua
hl.bind("SUPER + g", hl.plugin.hyprgrid.overview("toggle"))
```

## Using it

| Input | Does |
| --- | --- |
| Wheel (tilted: sideways) | The next workspace that way, per notch (hyprgrid's `overview.wheel`). |
| Arrows | Move the selection between windows; past the last one, to the next workspace on the grid. |
| `Enter` | Close the overview on the selection. |
| Click | Go to the window (or workspace) under the pointer and close. |
| Drag with the main button | Move a window: onto another, next to it (the thirds of a window say which side); onto another workspace; into an empty cell next to the grid, which gets a workspace. |
| Drag with the right button | Resize a window by its nearest corner. |
| `Escape` while dragging | Put the window back. |
| A tap (touchscreen) | Go to the window under it and close. |

Each monitor zoomed out is a screen of the overview, showing its board from
that monitor's view: with two monitors on one grid, both see the whole grid,
a seam between the regions, and a frame where each monitor looks. A window
dragged on one can be dropped on the other.

Over a layer (a bar), the pointer goes to it, unless a button is busy with a
window.

## Options

In hyprgrid's `setup()`, next to the wheel's:

```lua
hl.plugin.hyprgrid.setup({
    overview = {
        scale = 0.5,
        workspace_gap = 100,
        wallpaper = 2,
        blur = true,
        shadow = { enabled = true, range = 50 },
    },
})
```

They are config values (`plugin:hyprgrid:overview:*`); `setup()` passes
them to `hl.config`, which works directly too.

| Option | Default | Does |
| --- | --- | --- |
| `scale` | `0.5` | A workspace's size zoomed out, 0.1–0.9. |
| `workspace_gap` | `0` | Gap between workspaces, in pixels. |
| `wallpaper` | `0` | `0` one wallpaper behind everything, `1` a wallpaper per workspace, `2` both. |
| `blur` | `false` | Blur the wallpaper behind everything (not the per-workspace ones). |
| `cross_monitor_drag` | `false` | Open the overview on a monitor a dragged window goes over; and carry a SUPER + drag Hyprland started into the overview. |

`input`:

| Option | Default | Does |
| --- | --- | --- |
| `drag_mode` | `0` | `0` the main button drags windows, `1` the middle button does. |
| `drag_threshold` | `10` | Pixels a press must move before it's a drag rather than a click; `0` turns it off. |
| `left_handed` | Hyprland's | `1` swaps the mouse buttons in the overview only. |

`shadow` (around each workspace):

| Option | Default | Does |
| --- | --- | --- |
| `enabled` | `false` | Draw it. |
| `range`, `render_power`, `color` | Hyprland's `decoration.shadow.*` | Its look. |

## Actions

Each returns an action for `hl.bind`; inside a function you bound, calling
one runs it right away:

| Action | Arguments |
| --- | --- |
| `overview(arg)` | `"toggle"`, `"open"` or `"close"`, optionally followed by a monitor name or `all` (`"toggle all"`, `"open DP-1"`). A bare `toggle` or `open` acts on the focused monitor; a bare `close` closes every screen. `"select"` selects the workspace under the pointer. |
| `overview_navigate(dir)` | `"left"`, `"right"`, `"up"`, `"down"`: move the selection, and past the last window, to the next workspace on the grid. |
| `overview_window(arg)` | `"select"` focuses the window under the pointer, `"close"` closes it. |

All three are in `hl.plugin.hyprgrid`, and dispatchers too
(`hyprgrid:overview`…). From a script or a bar:

```bash
hyprctl dispatch 'hl.plugin.hyprgrid.overview("toggle")'
```

## Your own keys inside the overview

Define a submap named `hyprgrid_overview`, and it replaces the overview's
keys and clicks: it is on while a screen is open. This one reproduces the
defaults, plus closing the window under the pointer with a middle click:

```lua
hl.define_submap("hyprgrid_overview", function()
    local g = hl.plugin.hyprgrid
    hl.bind("left", g.overview_navigate("left"))
    hl.bind("right", g.overview_navigate("right"))
    hl.bind("up", g.overview_navigate("up"))
    hl.bind("down", g.overview_navigate("down"))
    hl.bind("return", g.overview("select"))
    hl.bind("escape", g.overview("close"))
    hl.bind("mouse:272", function() -- the default click
        g.overview("select")
        g.overview_window("select")
        g.overview("close")
    end, { mouse = true })
    hl.bind("mouse:274", g.overview_window("close"), { mouse = true })
end)
```

Your other binds are off while the submap is on; add
`{ submap_universal = true }` to the ones that should keep working. Drags
stay the overview's. The wheel is a rule of hyprgrid's: for wheel binds in
the submap, set `overview = { wheel = false }` in `setup()`.

## How it is built

The view (`View.cpp`) owns each monitor's position and zoom; the overview
only draws it and handles input while zoomed out. Two parts need no
Hyprland and are tested on their own:

- `Scene.hpp`: where things are drawn: cells, windows (another monitor's
  fitted in), the seam, the frames, drop slots (`tests/scene.cpp`).
- `Interaction.hpp`: what the pointer and the keys pick: the window under a
  point, where a drop lands, how a resize moves a corner, the window next
  door; and the buttons as a state machine: click, drag, resize
  (`tests/interaction.cpp`).

The rest is the overview on Hyprland:

| File | Holds |
| --- | --- |
| `Session.hpp/.cpp` | The overview: one for all monitors, with a screen on each one zoomed out. What screens share: Hyprland's state while open (no workspace slide, no warps or focus following the mouse, the submap), the entry points `View.cpp` calls. |
| `Input.cpp` | The pointer, touch and the keys, routed to the screen under the pointer (or the one a button went down on). |
| `Drag.cpp` | A window dragged, dropped or resized, across screens. |
| `Screen.hpp/.cpp` | A screen: the workspaces its monitor's board has, their cells, hit tests, the selection, closing. |
| `ScreenRender.cpp` | Drawing a screen: wallpaper, workspaces with their windows live, grid, seam, frames, the dragged window. |
| `Frames.cpp` | Frames and damage zoomed out: the selected workspace live, the others framed at a preview's pace. |
| `Visibility.cpp` | Drawing what Hyprland hides (culled surfaces, windows behind a fullscreen one), restored after; and the bar shown over a fullscreen window. |
| `Window.cpp` | One window drawn at a box: the geometry swapped in for the draw, its popups, subsurfaces, border, shadow and group tabs. |
| `Hooks.cpp` | The hooks into Hyprland's renderer, frames and damage, and the dispatchers. |
| `Config.cpp` | The options and the Lua actions. |

## Where it came from

The overview began as a fork of
[yayuuu/hyprland-scroll-overview](https://github.com/yayuuu/hyprland-scroll-overview),
itself from Vaxry's hyprland-plugins, forked at `d1fc8d4` (2026-08-05), and
was merged into hyprgrid, then rebuilt around hyprgrid's view and grid.
Upstream's features that don't apply here (its list and scrolling layouts,
its own zoom, pan and gestures) are gone; the rest is kept, restructured.

It no longer merges upstream. What is worth watching there, and in forks
fixing it for new Hyprland versions (silicalet's, which the fork took its
Hyprland API fixes from), are fixes to Hyprland API changes: small, and
ported by hand. Last reviewed: upstream `10eeefa` (2026-09-21).

Licensed BSD 3-Clause, see [LICENSE](LICENSE).
