-- Tests for the layouts in hyprgrid.lua: `make test`. hyprgrid.lua only
-- touches `hl` when called, so a stub records what the layouts place.

local placed
hl = { plugin = { hyprgrid = {
	place = function(moves)
		placed = moves
		return true
	end,
} } }

local M = dofile("hyprgrid.lua")
local failures = 0

local function check(cond, what)
	if not cond then
		failures = failures + 1
		print("layout.lua: failed: " .. what)
	end
end

local function ws(id, x, y, windows, active, board)
	return { id = id, x = x, y = y, windows = windows or 0, monitor = board or "M", board = board, active = active or false }
end

local function at(id)
	return placed and placed[id] and (placed[id].x .. "," .. placed[id].y)
end

-- In one piece: nothing moves.
placed = nil
M.layouts.compact({ ws(1, 0, 0, 1), ws(2, 0, 1), ws(3, 1, 1) })
check(placed == nil, "one piece stays")

-- A numbered workspace created far down its column slides up next to the others.
placed = nil
M.layouts.compact({ ws(1, 0, 0, 2, true), ws(5, 0, 4) })
check(at(5) == "0,1", "ws 5 slides to 0,1, got " .. tostring(at(5)))
check(at(1) == nil, "ws 1 stays (not moved)")

-- The link between two parts went: the smaller part slides back against the bigger one.
placed = nil
M.layouts.compact({ ws(1, 0, 0, 1), ws(2, 0, 1, 1), ws(4, 0, 3, 1) })
check(at(4) == "0,2", "the lone part slides to touch, got " .. tostring(at(4)))

-- A tie in size: the part with more windows stays.
placed = nil
M.layouts.compact({ ws(1, 0, 0, 1), ws(3, 2, 0, 3) })
check(at(3) == nil and at(1) == "1,0", "fewer windows slide, got " .. tostring(at(1)) .. " " .. tostring(at(3)))

-- A piece slides whole, keeping its shape, and never onto a taken cell.
placed = nil
M.layouts.compact({ ws(1, 0, 0, 1), ws(2, 1, 0, 1), ws(3, 2, 0, 1), ws(7, 0, 3), ws(8, 1, 3) })
check(at(7) == "0,1" and at(8) == "1,1", "a two-cell piece slides whole, got " .. tostring(at(7)) .. " " .. tostring(at(8)))

-- Three pieces: all end up touching.
placed = nil
M.layouts.compact({ ws(1, 0, 0, 5), ws(2, 5, 0), ws(3, 0, -6) })
check(at(2) == "1,0", "right piece slides left, got " .. tostring(at(2)))
check(at(3) == "0,-1", "top piece slides down, got " .. tostring(at(3)))

-- Boards keep their shapes apart: the same cells on two boards don't touch,
-- and each board is tidied on its own.
placed = nil
M.layouts.compact({ ws(1, 0, 0, 1, true, "DP-2"), ws(2, 0, 0, 1, true, "DP-3"), ws(5, 0, 3, 0, false, "DP-3") })
check(at(1) == nil and at(2) == nil, "each board's workspace stays")
check(at(5) == "0,1", "a workspace cut off on its own board slides back there, got " .. tostring(at(5)))

placed = nil
M.layouts.compact({ ws(1, 0, 0, 1, true, "DP-2"), ws(2, 5, 5, 1, true, "DP-3") })
check(placed == nil, "one workspace per board: nothing to tidy")

if failures > 0 then
	print(failures .. " layout check(s) failed")
	os.exit(1)
end
print("all layout checks passed")
