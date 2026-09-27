-- hyprgrid's default policy: what the gesture and the keys do with the grid.
-- Embedded in the plugin and run by hl.plugin.hyprgrid.setup(options), which
-- returns this module's actions for custom bindings. Everything here goes
-- through the public Lua API (hl.plugin.hyprgrid), so it can be copied and
-- changed wholesale; see the README for the options.
--
--   * Keys: SUPER+direction moves focus, and past the last window that way,
--     to the screen next door on the grid. SUPER+CTRL+direction moves the
--     window the same way, carrying it to the next screen at the edge.
--     SUPER+ALT+hjkl moves the workspace itself one cell.
--   * Gesture: one continuous swipe, any direction. Focus walks across the
--     windows, and past the last window the camera takes over and slides to
--     the screen next door -- the same rule on both axes, following the
--     content: fingers up walks focus DOWN and pulls the screen below in.
--     Lifting the fingers lands iOS-style on the cell the flick would coast
--   * Mouse: SUPER + middle-drag moves the view, the content following the
--     mouse; letting go lands on the cell the flick would coast to.
--   * Overview: zoomed out, the wheel moves the view a workspace per notch.
--   * Layout: "compact" keeps the grid in one piece: when a workspace is cut
--     off (created alone, or the one linking it to the others went), the
--     smaller part slides until it touches the rest.

local M = {}

local DEFAULTS = {
	gesture = {
		fingers = 3,
		step = 50, -- touchpad travel per window walked; lower = more sensitive
		jitter = 15, -- travel before the camera starts when no focus change needs protecting
		resist = 105, -- damped travel after a focus change before the camera goes 1:1
		damp = 0.25, -- camera speed (fraction of finger speed) while resisting
		hysteresis = 6, -- past the cell boundary before switching focus; stops flicker
		-- After a focus change, travel past the focused window's centre before
		-- the camera starts. A switch lands ~step/2 - hysteresis before the
		-- centre, so this is ~150 of drag after the switch before the screen moves.
		hold = 130,
	},
	keys = {
		focus = { mod = "SUPER", left = { "left", "h" }, right = { "right", "l" }, up = { "up", "k" }, down = { "down", "j" } },
		move_window = { mod = "SUPER + CTRL", left = { "left", "h" }, right = { "right", "l" }, up = { "up", "k" }, down = { "down", "j" } },
		move_workspace = { mod = "SUPER + ALT", left = { "h" }, right = { "l" }, up = { "k" }, down = { "j" } },
	},
	-- Reject tucking or landing a window once it would be squashed past this
	-- aspect (w/h or h/w).
	max_aspect = 3,
	-- A rule (hl.plugin.hyprgrid.rule()) for dragging the view with the mouse:
	-- `button` held, `start_mod` to start (`mod` instead: held throughout).
	-- How the grid keeps its shape (M.layouts, or your own function of the
	-- workspaces, see hl.plugin.hyprgrid.layout()); false: leave it alone.
	layout = "compact",
	mouse = {
		button = "middle",
		start_mod = "SUPER",
		cursor = "hold", -- "hold": hidden and kept in place; "free": moves, stops at the edge
		gain = "follow", -- pixels per cell; "follow": the content stays under the cursor
		invert = false,
	},
	-- Zoomed out (the overview), the wheel moves the view a workspace per
	-- notch, in its row or column (tilting it: sideways). `step`: cells a
	-- notch moves before the view glides on; `idle`: ms after the last notch
	-- before it does. The overview's other options (scale, workspace_gap,
	-- input, wallpaper, blur, shadow, cross_monitor_drag: see its README) go
	-- here too.
	overview = {
		wheel = true,
		step = 0.1,
		idle = 80,
	},
	hooks = {},
}

local STEP = { left = { -1, 0 }, right = { 1, 0 }, up = { 0, -1 }, down = { 0, 1 } }
local OPPOSITE = { left = "right", right = "left", up = "down", down = "up" }

local opts -- DEFAULTS merged with setup()'s options

-- `options` over `defaults`, recursively; a table replaces a list, and false
-- disables a whole section.
local function merge(defaults, options)
	if options == nil then
		return defaults
	end
	if type(defaults) ~= "table" or type(options) ~= "table" or defaults[1] ~= nil then
		return options
	end
	local out = {}
	for k, v in pairs(defaults) do
		out[k] = v
	end
	for k, v in pairs(options) do
		out[k] = merge(defaults[k], v)
	end
	return out
end

local function grid()
	return hl.plugin and hl.plugin.hyprgrid
end

---------------------------------------------------------------------------
-- Windows

-- Is there a window in `dir` from the focused one? Measured geometrically
-- rather than via movefocus: with no_focus_fallback off (the default),
-- movefocus warps to the nearest window in ANY direction when none exists in
-- the requested one, so a focus change alone can't tell we moved that way.
local function default_has_neighbor(dir)
	local aw = hl.get_active_window()
	local ws = hl.get_active_workspace()
	if not aw or not ws then
		return false
	end
	local function box(w)
		return w.at.x, w.at.y, w.at.x + w.size.x, w.at.y + w.size.y
	end
	local ax1, ay1, ax2, ay2 = box(aw)
	local acx, acy = (ax1 + ax2) / 2, (ay1 + ay2) / 2
	for _, w in ipairs(hl.get_workspace_windows(ws.id)) do
		if w.address ~= aw.address and w.mapped and (aw.floating or not w.floating) then
			local x1, y1, x2, y2 = box(w)
			local cx, cy = (x1 + x2) / 2, (y1 + y2) / 2
			local h_overlap = x1 < ax2 and x2 > ax1
			local v_overlap = y1 < ay2 and y2 > ay1
			if dir == "down" and cy > acy and h_overlap then
				return true
			end
			if dir == "up" and cy < acy and h_overlap then
				return true
			end
			if dir == "left" and cx < acx and v_overlap then
				return true
			end
			if dir == "right" and cx > acx and v_overlap then
				return true
			end
		end
	end
	return false
end

local function has_neighbor(dir)
	return (opts.hooks.has_neighbor or default_has_neighbor)(dir)
end

-- Seat a just-carried window toward `to` (the edge it entered from), but only
-- within an existing stack along that axis: step toward `to` while there is a
-- window that way and the window stays within max_aspect. A side-by-side
-- landing is left as-is; only a top/bottom one gets the window pulled to its
-- entry edge (top for a downward carry, bottom for an upward one).
local function default_settle(to)
	local max = opts.max_aspect
	for _ = 1, 20 do
		if not has_neighbor(to) then
			return
		end -- on the entry edge, or a vertical split: leave it
		local w = hl.get_active_window()
		if not w then
			return
		end
		local x, y = w.at.x, w.at.y
		hl.dispatch(hl.dsp.window.move({ direction = to }))
		local n = hl.get_active_window()
		if not n then
			return
		end
		local aspect = (n.size and n.size.y > 0) and (n.size.x / n.size.y) or 1
		if aspect > max or aspect < 1 / max then
			hl.dispatch(hl.dsp.window.move({ direction = OPPOSITE[to] })) -- undo squashing step
			return
		end
		if n.at.x == x and n.at.y == y then
			return
		end -- no further movement
	end
end

local function settle(to)
	return (opts.hooks.settle or default_settle)(to)
end

-- Focus the window on the edge a screen was entered from (the top one when
-- moving down, the left one when moving right), nearest `prev`: the previously
-- focused window's centre across the direction of travel.
local function default_on_enter(dx, dy, prev)
	local horizontal = dx ~= 0 and dy == 0
	local s = horizontal and dx or dy
	local ws = hl.get_active_workspace()
	local aw = hl.get_active_window()
	if not ws or (aw and aw.fullscreen ~= 0) then
		return
	end
	local best, best_edge, best_d
	for _, w in ipairs(hl.get_workspace_windows(ws.id)) do
		if w.mapped and not w.floating then
			local a, b = w.at.y, w.size.y -- along the entry axis
			local c, d = w.at.x, w.size.x -- across it
			if horizontal then
				a, b, c, d = w.at.x, w.size.x, w.at.y, w.size.y
			end
			local edge = s > 0 and a or -(a + b) -- smaller = nearer the entry edge
			local dist = prev and math.abs(c + d / 2 - prev) or 0
			if not best or edge < best_edge - 1 or (math.abs(edge - best_edge) <= 1 and dist < best_d) then
				best, best_edge, best_d = w, edge, dist
			end
		end
	end
	if best and not (aw and aw.address == best.address) then
		hl.dispatch(hl.dsp.focus({ window = best }))
	end
end

-- Switch to workspace `id`, entered from the side a (dx, dy) step comes from.
-- With `monitor` (another monitor it is on), go there instead of bringing it
-- over; with only `monitor`, to that monitor's current workspace.
function M.enter(id, dx, dy, monitor)
	local horizontal = dx ~= 0 and dy == 0
	local aw = hl.get_active_window()
	local prev = aw and (horizontal and aw.at.y + aw.size.y / 2 or aw.at.x + aw.size.x / 2)
	local before = hl.get_active_workspace()
	local here = hl.get_active_monitor and hl.get_active_monitor()
	if monitor and not (here and here.name == monitor) then
		hl.dispatch(hl.dsp.focus({ monitor = monitor }))
		if id then
			hl.dispatch(hl.dsp.focus({ workspace = id }))
		end
	elseif id then
		hl.dispatch(hl.dsp.focus({ workspace = id, on_current_monitor = true }))
	end
	local after = hl.get_active_workspace()
	if after and before and after.id ~= before.id then
		local on_enter = opts.hooks.on_enter or default_on_enter
		on_enter(dx, dy, prev)
	end
end

-- Where a step in `dir` leads (hl.plugin.hyprgrid.step()): a workspace, a new
-- one, or a linked monitor; nil for nothing (or no hyprgrid).
local function step(dir)
	local g = grid()
	local s = g and g.step and g.step(STEP[dir][1], STEP[dir][2])
	return s and s.kind ~= "none" and s or nil
end
M.step = step

---------------------------------------------------------------------------
-- Key actions

-- Move focus in `dir`; past the last window that way, to the screen next door
-- -- a workspace on the grid, or a linked monitor -- focusing the window on
-- the edge it is entered by.
function M.focus(dir)
	return function()
		if has_neighbor(dir) then
			hl.dispatch(hl.dsp.focus({ direction = dir }))
			return
		end
		local s = step(dir)
		if s then
			M.enter(s.id, s.dx, s.dy, s.monitor)
		end
	end
end

-- Move the focused window in `dir`. Attempt the in-layout move (the layout
-- decides whether that's a swap or a tuck) and keep it only while it stays
-- within max_aspect. If nothing moved (the layout's edge) or the move would
-- squash past it, carry the window to the screen next door instead, seated
-- against the edge it enters from.
function M.move_window(dir)
	return function()
		local max = opts.max_aspect
		-- Capture address/position as scalars BEFORE the move: the active window
		-- is a live object, so reading it afterwards would see the new value.
		local b = hl.get_active_window()
		local baddr = b and b.address
		local bx, by = b and b.at.x, b and b.at.y
		hl.dispatch(hl.dsp.window.move({ direction = dir, group_aware = true }))
		local a = hl.get_active_window()
		local moved = a and baddr and a.address == baddr and (bx ~= a.at.x or by ~= a.at.y)
		local aspect = (a and a.size and a.size.y > 0) and (a.size.x / a.size.y) or 1
		if moved and aspect <= max and aspect >= 1 / max then
			return -- in-layout move kept (swap or tuck), nothing over-squashed
		end
		local s = step(dir)
		if not s or not s.id then
			return
		end
		hl.dispatch(hl.dsp.window.move({ workspace = s.id }))
		settle(OPPOSITE[dir])
	end
end

-- Move the current workspace one cell in `dir`, swapping with the one there;
-- you stay on it, only the grid changes. Toward a linked monitor, it moves to
-- that monitor (onto its board).
function M.move_workspace(dir)
	return function()
		local g = grid()
		if not g then
			return
		end
		local s = step(dir)
		if s and s.kind == "monitor" then
			hl.dispatch(hl.dsp.workspace.move({ monitor = s.monitor }))
			return
		end
		g.move(STEP[dir][1], STEP[dir][2])
	end
end

---------------------------------------------------------------------------
-- Gesture
--
-- Travel is spent on focus steps (which don't animate) or on the camera
-- (which follows the fingers 1:1, diagonals included). Focus steps need no
-- dead zone beyond reaching the next window: sw.h/sw.v are measured from the
-- focused window's centre and focus switches just past half a step. The
-- camera only resists where that protects a focus change made during this
-- swipe on this workspace: it then holds off for `hold` past the window's
-- centre and starts damped (`damp` of finger speed) for `resist` of travel --
-- the screen visibly gives instead of freezing -- then follows 1:1.
-- Otherwise it starts after a small jitter guard. Swiping back past where the
-- camera started hands back to walking focus; carrying it through a
-- neighbour's centre makes that workspace active, walking focus again if it
-- has windows the way you're going. One screen is
-- gestures:workspace_swipe_distance of travel.

local sw -- per-gesture state
local function sw_reset()
	sw = {
		mode = "focus", -- "focus" | "camera"
		h = 0, -- horizontal offset from the focused window's centre (> 0: toward "right")
		v = 0, -- vertical offset from the focused window's centre (> 0: toward "down")
		focused_here = false, -- focus changed during this swipe on this workspace
		blocked = nil, -- direction with no screen to go to
		-- camera, while it's held:
		axis = "y", -- the edge it was pulled from: "x" | "y"
		s = 1, -- ...toward +1 / -1 on that axis
		start = 0, -- offset where it started
		resist = 0, -- damped travel at its start
		raw = 0, -- finger travel along the axis since it started
		shown = 0, -- how much of that the camera shows (after damping)
		landed = false, -- it has crossed into another workspace (or was grabbed)
		t = 0, -- time_ms of the event being handled
		dist = 300, -- travel per screen
	}
end

local function vdir(v)
	return v > 0 and "down" or "up"
end

local function hdir(h)
	return h > 0 and "right" or "left"
end

-- has_neighbor, except a fullscreen/maximized window hides the windows behind
-- it, so the swipe goes straight to the next screen instead of walking them.
local function swipe_neighbor(dir)
	local aw = hl.get_active_window()
	if aw and aw.fullscreen ~= 0 then
		return false
	end
	return has_neighbor(dir)
end

-- Finger travel since the camera started -> camera travel (always >= 0).
local function slide_pos(raw)
	local g = opts.gesture
	if raw <= sw.resist then
		return raw * g.damp
	end
	return sw.resist * g.damp + (raw - sw.resist)
end

-- Move the held camera by finger travel (dx, dy).
local function camera_move(dx, dy)
	local g = grid()
	local cx, cy = dx, dy
	if not sw.landed then
		-- Along the axis it was pulled on: damped while resisting, and swiping
		-- back past where it started hands back to walking focus.
		sw.raw = sw.raw + sw.s * (sw.axis == "x" and dx or dy)
		if sw.raw <= 0 then
			g.drag_end(sw.t, true)
			local left = sw.s * (sw.start + sw.raw)
			sw.mode, sw.blocked = "focus", nil
			if sw.axis == "x" then
				sw.h = left
			else
				sw.v = left
			end
			return
		end
		local shown = slide_pos(sw.raw)
		local along = sw.s * (shown - sw.shown)
		sw.shown = shown
		if sw.axis == "x" then
			cx = along
		else
			cy = along
		end
	end
	local rx, ry, land = g.drag(cx / sw.dist, cy / sw.dist, sw.t)
	if not land then
		return
	end
	-- Through a neighbour's centre: it becomes the active workspace.
	local horizontal = math.abs(rx) >= 1 and math.abs(ry) < 1
	local step = horizontal and (rx > 0 and 1 or -1) or (ry > 0 and 1 or -1)
	M.enter(land, horizontal and step or 0, horizontal and 0 or step)
	sw.landed, sw.focused_here, sw.blocked, sw.h, sw.v = true, false, nil, 0, 0
	-- Heading straight into its windows: walk them.
	local across = horizontal and ry or rx
	if math.abs(across) < 0.1 and swipe_neighbor(horizontal and hdir(step) or vdir(step)) then
		g.drag_end(sw.t, true)
		sw.mode = "focus"
	end
end

-- Called when there is no window in sw.h's / sw.v's direction.
local function edge_enter(axis)
	local offset = axis == "x" and sw.h or sw.v
	-- After a focus change the camera holds off for `hold` past the window's
	-- centre, protecting the focus just reached.
	local start = sw.focused_here and opts.gesture.hold or opts.gesture.jitter
	local raw = math.abs(offset) - start
	if raw <= 0 then
		return
	end
	local s = offset > 0 and 1 or -1
	local dir = axis == "x" and hdir(s) or vdir(s)
	local g = grid()
	if sw.blocked == dir or not g.drag_begin(axis == "x" and s or 0, axis == "y" and s or 0) then
		sw.blocked = dir
		-- hold at the start so reversing responds at once
		if axis == "x" then
			sw.h = s * start
		else
			sw.v = s * start
		end
		return
	end
	sw.mode, sw.axis, sw.s, sw.start, sw.raw, sw.shown, sw.landed = "camera", axis, s, start, 0, 0, false
	sw.resist = sw.focused_here and opts.gesture.resist or 0
	sw.h, sw.v = 0, 0
	camera_move(axis == "x" and s * raw or 0, axis == "y" and s * raw or 0)
end

local function horizontal_focus(dx)
	local step = opts.gesture.step
	sw.h = sw.h + dx
	if sw.blocked and hdir(sw.h) ~= sw.blocked then
		sw.blocked = nil
	end
	local switch_at = step / 2 + opts.gesture.hysteresis
	while math.abs(sw.h) > switch_at and swipe_neighbor(hdir(sw.h)) do
		hl.dispatch(hl.dsp.focus({ direction = hdir(sw.h) }))
		sw.h = sw.h - (sw.h > 0 and step or -step)
		sw.v, sw.focused_here = 0, true -- a horizontal step resets vertical travel
	end
	if sw.h ~= 0 and not swipe_neighbor(hdir(sw.h)) then
		edge_enter("x")
	end
end

local function vertical_focus(v)
	local step = opts.gesture.step
	sw.v = sw.v + v
	if sw.blocked and vdir(sw.v) ~= sw.blocked then
		sw.blocked = nil
	end
	local switch_at = step / 2 + opts.gesture.hysteresis
	while math.abs(sw.v) > switch_at and swipe_neighbor(vdir(sw.v)) do
		hl.dispatch(hl.dsp.focus({ direction = vdir(sw.v) }))
		sw.v = sw.v - (sw.v > 0 and step or -step)
		sw.h, sw.focused_here = 0, true -- a vertical step resets horizontal travel
	end
	if sw.v ~= 0 and not swipe_neighbor(vdir(sw.v)) then
		edge_enter("y")
	end
end

local function register_gesture()
	sw_reset()
	hl.gesture({
		fingers = opts.gesture.fingers,
		direction = "swipe",
		action = {
			-- start and the first update receive the same event; count it once.
			start = function(e)
				sw_reset()
				local dist = hl.get_config("gestures:workspace_swipe_distance")
				sw.dist = (type(dist) == "number" and dist > 0) and dist or 300
				sw.t = e and e.time_ms or 0
				local g = grid()
				if g and g.grab() then
					sw.mode, sw.landed = "camera", true -- a camera still moving from the last release: catch it
				end
			end,
			update = function(e)
				-- Content-following on both axes: (+x, +y) = toward the screen on the
				-- right / below, which the fingers pull in by moving left / up.
				local dx, dy = -e.delta.x, -e.delta.y
				sw.t = e.time_ms or sw.t
				if sw.mode == "camera" then
					camera_move(dx, dy) -- every movement, both axes
					return
				end
				if math.abs(dx) > math.abs(dy) then
					horizontal_focus(dx)
				else
					vertical_focus(dy)
				end
			end,
			finish = function(e)
				local g = grid()
				if sw.mode == "camera" and g then
					e = e or {}
					local id, dx, dy = g.drag_end(e.time_ms or sw.t, e.cancelled)
					if id then
						M.enter(id, dx, dy)
					end
				end
				sw_reset()
			end,
		},
	})
end

---------------------------------------------------------------------------

local function register_mouse()
	local m = opts.mouse
	local g = grid()
	if not (g and g.rule) then
		return
	end
	g.rule({
		when = { button = m.button, mod = m.mod or nil },
		start = m.start_mod and { mod = m.start_mod } or nil,
		drive = { motion = { target = "position", gain = m.gain, invert = m.invert } },
		cursor = m.cursor,
		-- Arriving on a workspace focuses the window on the edge it came in by.
		on_enter = function(_, id, dx, dy)
			M.enter(id, dx, dy)
		end,
	})
end

-- The overview's wheel: a rule while zoomed out, with no modifier (SUPER +
-- wheel stays the zoom). Its other options are config values
-- (plugin:hyprgrid:overview:*): set through hl.config.
local POLICY = { wheel = true, step = true, idle = true }

local function register_overview()
	local o = opts.overview
	local values = {}
	for k, v in pairs(o) do
		if not POLICY[k] then
			values[k] = v
		end
	end
	if next(values) then
		hl.config({ plugin = { hyprgrid = { overview = values } } })
	end

	local g = grid()
	if not (g and g.rule) or not o.wheel then
		return
	end
	local scroll = { target = "position", step = o.step }
	g.rule({
		when = { zoomed = true, mod = "" },
		drive = { wheel = scroll, wheel_x = scroll },
		release = { position = "direction" },
		idle = o.idle,
	})
end

---------------------------------------------------------------------------
-- Layouts: called with the grid's workspaces after it changes.

M.layouts = {}

local SIDES = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } }

local function cell_key(x, y)
	return x .. "," .. y
end

-- The workspaces in pieces: each piece's workspaces share edges.
local function pieces(workspaces)
	local at, seen, out = {}, {}, {}
	for _, w in ipairs(workspaces) do
		at[cell_key(w.x, w.y)] = w
	end
	for _, w in ipairs(workspaces) do
		if not seen[w.id] then
			local piece, todo = {}, { w }
			seen[w.id] = true
			while #todo > 0 do
				local c = table.remove(todo)
				piece[#piece + 1] = c
				for _, d in ipairs(SIDES) do
					local n = at[cell_key(c.x + d[1], c.y + d[2])]
					if n and not seen[n.id] then
						seen[n.id] = true
						todo[#todo + 1] = n
					end
				end
			end
			out[#out + 1] = piece
		end
	end
	return out
end

-- The shortest slide (dx, dy) that makes `piece` touch `fixed` without
-- landing on any cell in `taken`.
local function slide(piece, fixed, taken)
	for r = 1, 64 do
		for dx = -r, r do
			local rest = r - math.abs(dx)
			for _, dy in ipairs(rest == 0 and { 0 } or { -rest, rest }) do
				local free, touches = true, false
				for _, w in ipairs(piece) do
					local x, y = w.x + dx, w.y + dy
					if taken[cell_key(x, y)] then
						free = false
						break
					end
					for _, d in ipairs(SIDES) do
						touches = touches or fixed[cell_key(x + d[1], y + d[2])] == true
					end
				end
				if free and touches then
					return dx, dy
				end
			end
		end
	end
end

-- Keep the grid in one piece: the biggest piece stays (on a tie, the one with
-- more windows, then the one showing the active workspace), every other one
-- slides, smallest first, just far enough to touch it.
function M.layouts.compact(workspaces)
	-- Each board -- each region of it, with regions -- keeps its own shape.
	local boards, order = {}, {}
	for _, w in ipairs(workspaces) do
		local b = (w.board or "") .. "/" .. (w.region or "")
		if not boards[b] then
			boards[b] = {}
			order[#order + 1] = b
		end
		table.insert(boards[b], w)
	end
	if #order > 1 then
		for _, b in ipairs(order) do
			M.layouts.compact(boards[b])
		end
		return
	end

	local ps = pieces(workspaces)
	if #ps < 2 then
		return
	end
	local function rank(p)
		local windows, active = 0, false
		for _, w in ipairs(p) do
			windows = windows + w.windows
			active = active or w.active
		end
		return #p, windows, active and 1 or 0
	end
	table.sort(ps, function(a, b)
		local sa, wa, aa = rank(a)
		local sb, wb, ab = rank(b)
		if sa ~= sb then
			return sa > sb
		end
		if wa ~= wb then
			return wa > wb
		end
		return aa > ab
	end)

	local fixed, taken = {}, {}
	for _, w in ipairs(workspaces) do
		taken[cell_key(w.x, w.y)] = true
	end
	for _, w in ipairs(ps[1]) do
		fixed[cell_key(w.x, w.y)] = true
	end

	local moves, moved = {}, false
	for i = #ps, 2, -1 do -- smallest first
		local piece = ps[i]
		for _, w in ipairs(piece) do
			taken[cell_key(w.x, w.y)] = nil
		end
		local dx, dy = slide(piece, fixed, taken)
		dx, dy = dx or 0, dy or 0
		for _, w in ipairs(piece) do
			local x, y = w.x + dx, w.y + dy
			moves[w.id] = { x = x, y = y }
			taken[cell_key(x, y)] = true
			fixed[cell_key(x, y)] = true
		end
		moved = moved or dx ~= 0 or dy ~= 0
	end
	if moved then
		grid().place(moves)
	end
end

local function register_layout()
	local g = grid()
	local layout = opts.layout
	local arrange = type(layout) == "function" and layout or M.layouts[layout]
	if not (g and g.layout) then
		return
	end
	if not arrange then
		error("hyprgrid: unknown layout " .. tostring(layout))
	end
	g.layout(arrange)
end

local function bind_keys(group, action, flags)
	if not group then
		return
	end
	for dir in pairs(STEP) do
		for _, key in ipairs(group[dir] or {}) do
			hl.bind(group.mod .. " + " .. key, action(dir), flags)
		end
	end
end

function M.setup(options)
	opts = merge(DEFAULTS, options or {})
	M.options = opts
	if opts.gesture then
		register_gesture()
	end
	if opts.mouse then
		register_mouse()
	end
	if opts.overview then
		register_overview()
	end
	if opts.layout then
		register_layout()
	end
	if opts.keys then
		-- Where a move leaves the workspace is step()'s to say (the topology),
		-- for focus and moved windows alike: Hyprland's own fallback would
		-- carry a window moved past the layout's edge straight to the monitor
		-- that way, wherever it is on its board.
		hl.config({ binds = { window_direction_monitor_fallback = false } })
		bind_keys(opts.keys.focus, M.focus, { repeating = true })
		bind_keys(opts.keys.move_window, M.move_window, { repeating = true })
		bind_keys(opts.keys.move_workspace, M.move_workspace)
	end
	return M
end

return M
