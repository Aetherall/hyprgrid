# hyprgrid

> **AI disclaimer.** Most of this code, its tests and its documentation were
> written by an AI assistant (Anthropic's Claude), directed, reviewed and
> tested in daily use by the author. It carries the mistakes such code can:
> read it before trusting it, and report what you find.

A 2D workspace grid for Hyprland. Your workspaces sit on a board instead of a
numbered line, and your screen is a camera that glides between them.

```
        x = -1      x = 0       x = 1
      ┌─────────┬───────────┬─────────┐
y = 0 │         │  ws 1     │         │
      ├─────────┼───────────┼─────────┤
y = 1 │  ws 103 │ [ws 2]  ──┼─> ws 101│   SUPER + l, or a 3-finger swipe,
      ├─────────┼───────────┼─────────┤   slides the camera one cell right
y = 2 │         │  ws 3     │         │
      └─────────┴───────────┴─────────┘
```

- **A grid.** Workspaces 1–99 start stacked in column 0. The board grows as
  you use it: you can step into any empty cell next to a workspace that has
  windows, and a new workspace appears there. Workspaces can be moved around.
- **A camera.** Every workspace switch, whatever caused it (a swipe, a key, an
  app), is animated as a camera move across the grid, diagonals included. A
  swipe hands its speed to the camera, so a flick coasts on.
- **An overview.** Zoom the camera out and the whole grid shows, windows
  live: pick one, drag it to another workspace or an empty cell, resize it.
  See [overview/README.md](overview/README.md).
- **Behaviour in Lua.** The plugin provides the grid, the camera and the
  overview. What your keys and gestures do with them is a Lua policy that
  ships with the plugin, and that you can tune, partly replace, or rewrite
  from your config.

**Status:** early. Needs Hyprland's Lua config (0.55+); built and used on
Hyprland 0.56.

## Contents

- [Quick start](#quick-start)
- [What you get](#what-you-get)
- [Customizing](#customizing)
  1. [Tune the options](#1-tune-the-options)
  2. [Change the keys](#2-change-the-keys)
  3. [Bind the actions yourself](#3-bind-the-actions-yourself)
  4. [Replace one piece with a hook](#4-replace-one-piece-with-a-hook)
  5. [Write your own policy](#5-write-your-own-policy)
- [How the grid works](#how-the-grid-works)
- [Installation](#installation)
- [For plugin authors](#for-plugin-authors)

## Quick start

1. Install the plugin (see [Installation](#installation)) and load it:

   ```lua
   hl.plugin.load("/path/to/libhyprgrid.so")
   ```

2. Turn on the default behaviour:

   ```lua
   if hl.plugin and hl.plugin.hyprgrid then
       hl.plugin.hyprgrid.setup()
   end
   ```

   Hyprland reads your config once before loading plugins, then again after:
   the `if` skips the first pass, when the plugin isn't there yet.

## What you get

`setup()` with no options gives you:

| Input | Does |
| --- | --- |
| `SUPER` + arrows / `hjkl` | Move focus. Past the last window that way, move to the screen next door. |
| `SUPER + CTRL` + arrows / `hjkl` | Move the window. At the edge of the screen, carry it to the screen next door. |
| `SUPER + ALT` + `hjkl` | Move the whole workspace one cell, swapping with the one there. You stay on it. |
| 3-finger swipe, any direction | Walk focus across the windows; past the last one, drag the next screen in, on both axes. Let go and the camera lands where the flick would coast to. |
| `SUPER` + middle-drag | Drag the grid, the content following the mouse; the cursor hides and the drag never hits the screen edge. Let go and it lands where the flick would coast to. `SUPER` is only needed to start. |
| Wheel, in the overview | The next workspace that way, per notch; tilted: sideways. |

The swipe follows the content, like a phone: fingers up walks focus down and
pulls in the screen below.

## Customizing

Start at the top and go only as far down as you need.

### 1. Tune the options

Pass a table to `setup()`. Every field is optional, and anything you leave
out keeps its default:

```lua
hl.plugin.hyprgrid.setup({
    gesture = { fingers = 4 },
})
```

The full set, with the defaults:

```lua
hl.plugin.hyprgrid.setup({
    gesture = {
        fingers = 3,
        step = 50,       -- swipe travel per window walked; lower = more sensitive
        jitter = 15,     -- travel before the camera starts moving
        resist = 105,    -- after walking focus, travel during which the camera moves slowly
        damp = 0.25,     -- camera speed during `resist`, as a fraction of finger speed
        hysteresis = 6,  -- travel past the switch point before focus changes (stops flicker)
        hold = 130,      -- after walking focus, travel past the new window before the camera moves
    },
    keys = { ... },      -- see below
    mouse = {
        button = "middle",
        start_mod = "SUPER", -- needed to start the drag; `mod` instead: held throughout
        cursor = "hold",     -- "hold": hidden and kept in place; "free": moves, stops at the edge
        gain = "follow",     -- pixels per cell; "follow": the content stays under the cursor
        invert = false,      -- true: the view moves the way the mouse does
    },
    overview = {
        wheel = true,    -- zoomed out, the wheel steps a workspace per notch
        step = 0.1,      -- cells a notch moves before the view glides on
        idle = 80,       -- ms after the last notch before it does
        -- and the overview's own options (scale, wallpaper, blur, ...):
        -- see overview/README.md
    },
    max_aspect = 3,      -- a moved window is never squashed past 3:1
    hooks = {},          -- see "Replace one piece with a hook"
})
```

`false` turns a whole section off: `gesture = false` leaves you with the keys
only, `mouse = false` without the mouse drag, `overview = false` leaves
the wheel alone in the overview.

One screen of camera travel is Hyprland's
`gestures:workspace_swipe_distance`. The camera's landing spring is a curve
named `kinetic`, if your config defines one; keep it critically damped (the
default is equivalent to):

```lua
local w = 2 * math.pi / 0.3 -- 0.3 s period; lower is snappier
hl.curve("kinetic", { type = "spring", stiffness = w ^ 2, damping = 2 * w, mass = 1 })
```

### 2. Change the keys

There are three groups of keys. Each has a modifier and a list of keys per
direction:

```lua
keys = {
    focus          = { mod = "SUPER",        left = { "left", "h" }, right = { "right", "l" }, up = { "up", "k" }, down = { "down", "j" } },
    move_window    = { mod = "SUPER + CTRL", left = { "left", "h" }, right = { "right", "l" }, up = { "up", "k" }, down = { "down", "j" } },
    move_workspace = { mod = "SUPER + ALT",  left = { "h" },         right = { "l" },          up = { "k" },         down = { "j" } },
}
```

Override only what you want. A list replaces the default list, it isn't added
to it:

```lua
hl.plugin.hyprgrid.setup({
    keys = {
        focus = { left = { "left" }, right = { "right" }, up = { "up" }, down = { "down" } }, -- arrows only
        move_window = { mod = "SUPER + SHIFT" },                                             -- another modifier
        move_workspace = false,                                                              -- none at all
    },
})
```

`keys = false` binds nothing.

### 3. Bind the actions yourself

`setup()` returns the actions it binds. `focus`, `move_window` and
`move_workspace` take a direction (`"left"`, `"right"`, `"up"`, `"down"`) and
return a function you can hand to `hl.bind`:

```lua
local grid = hl.plugin.hyprgrid.setup({ keys = false })

hl.bind("SUPER + a", grid.focus("left"))
hl.bind("SUPER + d", grid.focus("right"))
hl.bind("SUPER + SHIFT + a", grid.move_window("left"))
hl.bind("SUPER + ALT + a", grid.move_workspace("left"))
```

| Action | Does |
| --- | --- |
| `grid.focus(dir)` | Focus the next window that way, or move to the screen next door. |
| `grid.move_window(dir)` | Move the window within the layout, or carry it to the screen next door at the edge. |
| `grid.move_workspace(dir)` | Move the current workspace one cell. |
| `grid.enter(id, dx, dy)` | Switch to workspace `id` as if arriving from a step of `(dx, dy)`, then focus the window on the edge you came in by. Runs immediately; call it from your own functions. |

`grid.options` holds the options in effect, defaults included.

### 4. Replace one piece with a hook

Hooks swap out one decision of the default policy and keep everything else.
Each one replaces the built-in version entirely.

| Hook | Called | Should |
| --- | --- | --- |
| `has_neighbor(dir)` | Before moving focus or walking the swipe in `dir` | Return `true` if there's a window that way (focus moves to it), `false` to go to the screen next door. |
| `on_enter(dx, dy, prev)` | After arriving on a screen from a step of `(dx, dy)` | Focus the window you want. `prev` is the previous window's centre, across the direction of travel (nil if there was none). Default: the window on the edge you came in by, nearest `prev`. |
| `settle(to)` | After a window is carried to another screen | Place the window. `to` is the edge it came in by (`"left"`, …). Default: in a stack along that axis, move it to that edge. |

Examples:

```lua
hl.plugin.hyprgrid.setup({
    hooks = {
        -- Never walk windows: keys and swipes always go screen to screen.
        has_neighbor = function() return false end,

        -- Arriving on a screen, keep whatever focus Hyprland picks.
        on_enter = function() end,

        -- Leave carried windows wherever the layout drops them.
        settle = function() end,
    },
})
```

### 5. Write your own policy

For behaviour the options can't express, skip `setup()` and use the grid
directly. Everything the default policy does goes through these functions,
all acting on the focused monitor:

| Function | Returns |
| --- | --- |
| `cell(id)` | `x, y` of workspace `id` on the grid, or nil if it isn't on it (named and special workspaces). |
| `neighbor(dx, dy)` | The id of the workspace `(dx, dy)` cells away. For an empty cell you're allowed to enter, a new id: switch to it (or send a window) to create the workspace there. nil if you can't go there. |
| `move(dx, dy)` | Moves the active workspace `(dx, dy)` cells, swapping with the one there. Returns whether it moved. |

A switch to a grid workspace animates the camera on its own, so a policy made
of `neighbor` and Hyprland's dispatchers already glides:

```lua
local g = hl.plugin.hyprgrid

-- SUPER + TAB: the next screen to the right, or the one below at the end of the row.
hl.bind("SUPER + TAB", function()
    local id = g.neighbor(1, 0) or g.neighbor(0, 1)
    if id then
        hl.dispatch(hl.dsp.focus({ workspace = id, on_current_monitor = true }))
    end
end)
```

To drive the camera with your fingers, from a `hl.gesture` action:

| Function | Returns |
| --- | --- |
| `drag_begin(dx, dy)` | Starts dragging the camera from the active cell toward `(dx, dy)`. `false` if that cell can't be entered. |
| `grab()` | Catches the camera if it's still moving from a release, and drags it from there. `x, y` where it is (relative to the active cell), or nil if it was still. |
| `drag(dx, dy, time_ms)` | Moves the camera by `(dx, dy)` cells. Returns `x, y`, and the id of a workspace when the camera reaches its centre: switch to it, and the drag carries on from there. |
| `drag_end(time_ms, stay)` | Lets go. The camera lands on the cell its speed coasts to (or back on the active cell if `stay`). Returns `id, dx, dy` of the workspace to switch to, or nil if it stayed. |

To drive the view from the mouse, declare rules. A rule says: while this is
held, these moving inputs drive the view. Rules are matched again whenever
what you hold changes, so you describe states, not sequences:

```lua
hl.plugin.hyprgrid.rule({
    when   = { button = "middle" },  -- held while the rule is active
    start  = { mod = "SUPER" },      -- also held to enter it
    drive  = { motion = "position" },
    cursor = "hold",
})
```

The input a rule takes goes no further (not to the app, not to your binds).
The fields, and the part still to come (touchpad fingers), are in
[docs/design.md](docs/design.md). A rule can drive the zoom too
(`drive = { wheel = "zoom" }`: zoomed all the way out, that is the overview).

With two monitors, they share one grid by default, split into a region
each along a seam that follows their physical layout: each region grows
everywhere but across the seam, and from the cell next to the seam, focus
(or a carried window, or a moved workspace) crosses to the other monitor's
current workspace. The overview shows the whole grid from each monitor,
with the seam and a frame over each monitor's workspace.
`topology()` changes that, and `step(dx, dy)` answers where a move leads
(`"workspace"`, `"new"`, `"monitor"` or `"none"`), for your own actions:

```lua
hl.plugin.hyprgrid.topology({
    boards = "regions",    -- "monitor": a grid each; "shared": one grid, no seams; or groups { name = { monitors } }
    links  = "physical",   -- "none": no crossing; a list of links; or function(monitor, side) -> monitor
    land   = "active",     -- "row": the workspace in the same row on the other monitor
    grow   = "open_edges", -- "always": grow toward a link before crossing it; "never"
})
```

The details are in [docs/design.md](docs/design.md) ("Topology").

The default policy is the best example of all of this. It's
[`hyprgrid.lua`](hyprgrid.lua), also installed in `share/hyprgrid/`: copy
it into your config, change it, and call your copy's `setup()` instead.

## How the grid works

- **Placement.** A workspace gets a cell the first time hyprgrid sees it:
  workspace `n` from 1 to 99 goes to column 0, row `n - 1`, or the nearest
  free cell. Workspaces created by stepping into an empty cell get that cell
  (their ids start at 100). Named and special workspaces aren't on the grid.
- **Where you can go.** A cell can be entered if it holds a workspace, or if
  it's empty and next to (not diagonal from) a workspace with windows.
- **Memory.** The layout is saved in `$XDG_RUNTIME_DIR/hyprgrid`, so reloading
  the plugin keeps it. It's forgotten when you log out.
- **The camera.** Hyprland still performs every switch. hyprgrid only takes
  over the animation, starting from wherever the old workspace is on screen,
  so switches in quick succession chain smoothly. Each monitor has its own
  camera.

## Installation

### Nix

A plugin must be built against the exact Hyprland that loads it, so make this
flake follow your Hyprland input:

```nix
# flake.nix
inputs = {
  hyprland.url = "github:hyprwm/Hyprland";
  hyprgrid = {
    url = "github:aetherall/hyprgrid";
    inputs.hyprland.follows = "hyprland";
  };
};
```

The plugin is `${inputs.hyprgrid.packages.${system}.default}/lib/libhyprgrid.so`.

If you take Hyprland from nixpkgs instead, apply `overlays.default` and use
`pkgs.hyprgrid`, which builds against `pkgs.hyprland`.

### Without Nix

`make`, with Hyprland's headers available through pkg-config. This builds
`hyprgrid.so`.

## For plugin authors

Other plugins (a bar…) can use the grid through the C API in
[`hyprgrid.h`](hyprgrid.h): read a workspace's cell, or move a workspace.
The header explains how to find hyprgrid at runtime.

## License

BSD 3-Clause, see [LICENSE](LICENSE). The overview derives from
hyprland-scroll-overview, under its own BSD 3-Clause
[licence](overview/LICENSE).
