# Design: one view, composable gestures

Status: implemented, but for `on_step` (plan step 4). It replaced a
split where hyprgrid owned a camera, a separate hyprgrid-overview plugin its
own zoom and pan, and each input (trackpad swipe, wheel, keys) reached them
by its own path.

## Goals

- Mouse and keyboard are first-class inputs, not only the trackpad.
- One gesture can change meaning while it runs, without a jump:
  - 4 fingers up zooms into the overview; lifting one finger switches to
    panning; a flick then zooms back in on the workspace the flick lands on.
  - SUPER + middle-drag pans; scrolling while still holding zooms out, and
    the pan keeps following the mouse.
- Lua describes gestures. C++ runs them on every event and frame. Lua runs
  only at transitions.
- One kinetics implementation (velocity tracking, flick projection, springs)
  for everything.

## The view

Each monitor has one view, owned by hyprgrid:

- `position`: where you look on the grid, in cells (x, y). Animated.
- `zoom`: 0 is the normal view, 1 is the overview. Animated.

The active workspace follows the position (it switches when the view
settles on, or crosses into, another cell), as both plugins already do.

Two renderers read the view:

- `zoom == 0`: hyprgrid's current path. Hyprland renders the workspaces and
  hyprgrid offsets them (`m_renderOffset`).
- `zoom > 0`: the overview (`overview/`) draws. Its scale and offset are
  derived from the view; it keeps no pan or zoom state of its own.

Cells are the shared unit. Each renderer turns cells into pixels with its own
spacing. Pointer motion is converted to cells with the spacing at the
current zoom, so content stays under the cursor while zooming.

The overview is part of hyprgrid; how it is built is in
[overview/README.md](../overview/README.md).

## Gestures

A gesture is not chosen by how it starts. What you hold decides, at every
moment, what the moving inputs do:

- **Held state**: fingers on the touchpad, mouse buttons pressed, modifiers.
- **Moving inputs**: pointer or finger motion, the wheel.

The config declares **rules**. A rule says: while this is held, these moving
inputs drive these targets, and this is how things settle if you let go
here. The engine re-matches the rules whenever the held state changes. The
targets (the view's `position` and `zoom`) keep their value and speed
across a change of rule, so nothing jumps.

So the config describes states, not paths: with a 3-finger rule (pan) and
a 4-finger rule (zoom), starting with 4 then lifting one pans, starting
with 3 then adding one zooms, and 3 fingers always means pan, however you
got there.

A **session** runs from the first time a rule matches until none does; the
rule that matched last decides how the targets settle. Only one session
runs at a time; while it runs, it gets the input its rules use.

Hooks (Lua functions) run only for decisions that depend on the desktop
rather than the hands -- which window to focus on entering a workspace,
whether there is a window to walk to -- never per motion event.

The screen-edge behaviour of pointer motion is a per-rule setting: keep the
cursor visible (it stops at the edge), or hide it and hold it in place.

The engine is plain C++ with no Hyprland dependency, so it can be unit
tested. Small adapters feed it from Hyprland's event bus
(`gesture.swipe.*` with finger counts, `input.mouse.move/button/axis`) and
cancel the events a session consumes.

## Lua gesture API

Status: implemented; the part marked (step 4), `on_step`, is specified so
the shape holds, and comes with the default swipe rebuilt as rules.

### Rules

```lua
hl.plugin.hyprgrid.rule({
    when    = { button = "middle" },
    start   = { mod = "SUPER" },
    drive   = { motion = "position", wheel = "zoom" },
    cursor  = "hold",
    release = { position = "flick", zoom = "direction" },
})
```

`rule()` is called at config time, like `hl.bind`. A config reload rebuilds
Hyprland's Lua state, so hyprgrid drops every rule on `config.preReload`
and the config declares them again.

| Field | Default | Does |
| --- | --- | --- |
| `when` | required | what must stay held while the rule is active |
| `start` | none | what must also be held to enter the rule (not to stay in it) |
| `drive` | required | moving input -> target, see below |
| `cursor` | `"free"` | pointer motion at the screen edge: `"free"` (the cursor moves and stops at the edge) or `"hold"` (hidden and kept in place, so motion never ends) |
| `release` | see below | how each target settles if the session ends in this rule |
| `idle` | `150` | ms without wheel events that count as letting go, for rules nothing is held for but modifiers |
| `on_enter`, `on_step` | none | hooks, see below |

### Held state: `when` and `start`

| Key | Value |
| --- | --- |
| `mod` | modifiers, as in binds: `"SUPER"`, `"SUPER + SHIFT"` |
| `button` | `"left"`, `"right"`, `"middle"` or a Linux button code |
| `fingers` | touchpad fingers, 3 to 5 (fewer are scrolling) |
| `zoomed` | `true`: only while the view under the cursor is zoomed out (the overview); `false`: only zoomed in |

Keys in `when` must match exactly for as long as the rule is active; keys
in `start` only when the rule is entered. For modifiers that is the choice
between "keep SUPER down for the whole drag"
(`when = { button = "middle", mod = "SUPER" }`) and "SUPER only to start"
(`when = { button = "middle" }, start = { mod = "SUPER" }`).

When several rules match, the one with the most keys wins; on a tie, the
one declared first. A rule with only `mod` keys (e.g. SUPER + wheel) is
active while those are held and a moving input it drives keeps coming; it
ends after `idle` ms without one.

The input a session uses is swallowed: the app under the cursor and
Hyprland's own binds never see it (a SUPER + wheel rule shadows a SUPER +
wheel bind). Input the matching rules don't drive passes through.

### `drive`: which moving input drives which target

`drive = { motion = "position" }` is shorthand for
`drive = { motion = { target = "position" } }`. The inputs: `motion`
(the pointer, or for a rule on `fingers` the fingers: each drives only its
own kind of rule), `motion_x` / `motion_y` (one axis),
`wheel`, `wheel_x` (the wheel tilted sideways). The targets: `position` (where the view is on the grid,
in cells) and `zoom` (0 normal, 1 overview).

| Option | Default | Does |
| --- | --- | --- |
| `gain` | `"follow"` | motion: pixels per cell. `"follow"` keeps the content under the cursor (one cell per monitor width / height at the current zoom) |
| `invert` | `false` | motion: by default the view follows the content (drag right, the grid moves right, like a map); `true` moves the view the way you move |
| `step` | `0.25` | wheel: target units per notch (position: cells, down or right) |

### `release`: how each target settles

| Value | Settles |
| --- | --- |
| `"flick"` | on the stop nearest where the velocity would coast (position: cells; zoom: 0 or 1). Default for position. |
| `"direction"` | on the next stop in the direction of the last movement, at its speed; one wheel notch is enough (position: the next workspace in its row or column). Default for zoom. |
| `"stay"` | back where the session started |
| `"keep"` | zoom: where it is, if between stops the nearest one |
| a number | zoom: that value (`0`: back to the normal view) |

### Hooks

| Hook | Called | Without it |
| --- | --- | --- |
| `on_enter(s, id, dx, dy)` | the view reached a neighbour's centre, or a release lands on another cell: workspace `id`, a step of `(dx, dy)` | hyprgrid switches to `id` on the session's monitor |
| `on_step(s, dx, dy)` (step 4) | every `step_px` of motion along an axis (return `true` to spend it, e.g. on walking focus, instead of moving the target) | the target moves |

With `on_enter` set, the hook does the switch (the default policy uses it
to focus the window on the edge it came in by, as `grid.enter()` does now).
`s.monitor` is the session's monitor; `s:cancel()` ends the session with
everything back where it started.

### Examples

Trackpad: four fingers zoom, three pan while zoomed out; letting go from
panning lands on the flick's workspace, and zooms back in if the session
zoomed (`setup()`'s `overview.touchpad`, which also keeps three fingers
zoomed in for the default swipe).

```lua
local g = hl.plugin.hyprgrid
g.rule({ when = { fingers = 4 }, drive = { motion_y = { target = "zoom", gain = 300 } }, release = { zoom = "flick" } })
g.rule({ when = { fingers = 3, zoomed = true }, drive = { motion = { target = "position", gain = 300 } }, release = { position = "flick", zoom = 0 } })
```

A swipe a rule takes at its begin is the session's whole: Hyprland's
`hl.gesture`s and the app see none of it.

Mouse: SUPER + middle-drag pans, scrolling meanwhile zooms, and
SUPER + wheel alone zooms in and out.

```lua
g.rule({ when = { button = "middle" }, start = { mod = "SUPER" }, drive = { motion = "position", wheel = "zoom" }, cursor = "hold" })
g.rule({ when = { mod = "SUPER" }, drive = { wheel = "zoom" } })
```

Overview: the wheel alone, zoomed out, steps a workspace per notch
(`setup()`'s `overview.wheel`).

```lua
local scroll = { target = "position", step = 0.1 }
g.rule({ when = { zoomed = true, mod = "" }, drive = { wheel = scroll, wheel_x = scroll }, release = { position = "direction" }, idle = 80 })
```

### In `setup()`

The default policy declares its rules from options, like its keys:

```lua
hl.plugin.hyprgrid.setup({
    mouse = { button = "middle", start_mod = "SUPER", cursor = "hold" }, -- default; false: none
})
```

### Step 2 implements

`rule()` with `when` / `start` for `mod` and `button`, rule matching,
`drive` for `motion` / `motion_x` / `motion_y` -> `position` with `gain`
and `invert`, both `cursor` modes, `release` `"flick"` / `"stay"` for
position, `on_enter`, `s:cancel()`, and the `mouse` option in `setup()`.
The engine (matching, driving, which hooks fire) is Hyprland-free and unit
tested; a thin adapter feeds it Hyprland's mouse events and applies it to
the view.

### Lessons from implementing step 2

- **Input re-enters.** A workspace switch (or a hook) makes Hyprland
  simulate pointer motion, which comes back into the move handler before
  the first input is done. Measured again, it moved the view again, switched
  again, and recursed until Hyprland crashed. The engine now drops motion
  arriving while it applies an input and defers held-state changes and
  cancels until it is done (tested), and the adapter settles its reference
  point before calling the engine.
- **Hide the cursor through Hyprland.** Setting the cursor image directly
  desyncs the renderer (the cursor came back drawn under the windows) and
  its 500 ms ticker would unhide it. A held cursor sets `cursor:invisible`
  for the session, restoring the user's value after, and lets Hyprland hide
  and restore the app's cursor.

## Topology: boards and links

Status: implemented.

Several monitors can share one grid, have one each, or anything between.
The engine owns that as data, the topology, and answers every "what is that
way?" from it, so keys, the overview, layouts and gestures agree.

- **Boards**: which monitors share a grid. A workspace sits on the board of
  the monitor it is on; moved to a monitor on another board, it is placed
  on that board, next to its workspaces. By default (`"regions"`) two
  monitors share one grid, split into a region each (below).
- **Links**: what the edge of a board leads to: another monitor, or
  nothing.

```lua
hl.plugin.hyprgrid.topology({
    boards = "regions",    -- one grid, a region per monitor (default); "monitor": one each; "shared": one, no seams; or groups
    links  = "physical",   -- an edge facing another monitor leads to it (default); "none"
    land   = "active",     -- crossing lands on that monitor's current workspace (default); "row"
    grow   = "open_edges", -- empty cells can be entered except toward a link (default); "always", "never"
})
```

Explicit forms, for other setups:

- `boards = { work = { "DP-3" }, side = { "DP-2", "HDMI-A-1" } }`: named
  groups; a monitor in none gets its own board.
- `links = { { from = "DP-3", side = "left", to = "DP-2" }, ... }`: links
  made by hand (one-way unless listed both ways).
- `links = function(monitor, side) return "DP-2" end`: anything else; called
  on a move at a board's edge only, never per frame.

`topology()` is called at config time and reset to the defaults on a
config reload, like rules.

### Regions

With `boards = "regions"` there is one grid, and each monitor owns a region
of it. The seam between two regions comes from where the monitors are:
side by side, a vertical seam (cells left of it belong to the left
monitor); stacked, a horizontal one. A workspace's monitor is its cell's
region.

- A region grows everywhere but across its seam. Numbered workspaces start
  on the cell next to the seam, and a workspace moved over from the other
  monitor lands there too (Hyprland resizes it and its windows to the
  monitor it is now on).
- Crossing happens only from the cell next to the seam: a step across it is
  the link to the other monitor (none with `links = "none"`). With
  `land = "active"` it lands on that monitor's current workspace, jumping
  over the ones in between; `"row"` picks the one in the same row.
- The overview draws the one grid from each monitor's point of view: the
  other monitor's workspaces sit in their cells, fitted at their own shape,
  with faint outlines for empty cells, a line along the seam, and a frame
  over each monitor's current workspace.
- More than two monitors fall back to a grid each, for now; `monitor.
  layoutChanged` recomputes the seam.

### step()

Every move asks `hl.plugin.hyprgrid.step(dx, dy)`, from the focused
monitor's active workspace. The answer, in this order:

| `kind` | When | Also |
| --- | --- | --- |
| `"workspace"` | a workspace on this board is that way | `id`, `monitor` (on a shared board, possibly another monitor) |
| `"monitor"` | this board's edge that way is linked (`grow = "always"`: only if the empty cell can't be entered) | `monitor`, `id`: the workspace to land on |
| `"new"` | the empty cell that way can be entered | `id`: reserved for it; switching or moving a window there creates it |
| `"none"` | nothing | |

The default policy carries it out: focus goes to the window on the edge it
came in by, on the workspace or monitor reached; a carried window moves
there; a workspace moved toward a link goes to that monitor.
`neighbor(dx, dy)` stays, as `step()` limited to the board (`"workspace"`
or `"new"`).

With a single monitor the defaults behave as before: its board is the
whole grid, and it has no links.

## Plan

0. Done: both plugins built from local checkouts; the overview since merged
   into hyprgrid.
1. Done: the view replaces the camera; one `Kinetics.hpp`.
2. Done: the gesture engine and mouse adapter (rules on buttons and
   modifiers, both cursor modes).
3. Done: the zoom target; the overview renders the view. SUPER + wheel
   zooms; zoomed out, the wheel is a rule too (`zoomed`, `wheel_x`,
   `release.position = "direction"`).
4. Done: the trackpad adapter, `fingers` rules, finger count changes
   within one session, the overview's touchpad rules in `setup()`. Still to
   do: the default swipe rebuilt as rules with `on_step` (it is an
   `hl.gesture` for now, on the fingers no rule takes).
5. Done: the old paths removed (`pan`/`pan_end`, the overview's own
   gestures, pan and wheel code), the READMEs rewritten.

### Lessons from implementing step 4

- **A finger count change is a cancelled swipe.** libinput
  (`evdev-mt-touchpad-gestures.c`) debounces a new finger count for 100 ms,
  then ends the swipe cancelled and begins another only once the new
  fingers move; lifting every finger ends it normally. So a cancelled end
  holds the session: the next begin carries it on with its count, and it
  ends as a release if none comes within 300 ms or the pointer moves (one
  finger left).

## Open questions

- Seams for three or more monitors (regions handle two).
- `Kinetics.hpp` opens a private part of hyprutils (`#define private public`);
  a hyprutils change can break it.
