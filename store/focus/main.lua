-- Focus: a Pomodoro timer. 25 minutes of work, then a 5 minute break; counts finished sessions.
local WORK, BREAK = 25 * 60, 5 * 60
local phase = "work"          -- "work" or "break"
local left = WORK             -- seconds left in the phase
local runningSince = nil      -- sys.millis() when started, nil while paused
local leftAtStart = WORK
local done = store.get("done") or 0
local clock, phaseRow, doneRow, startButton

local function fmt(s)
  return string.format("%02d:%02d", s // 60, s % 60)
end

local function show()
  clock:set_text(fmt(left))
  phaseRow:set_value(phase == "work" and "Work" or "Break")
  doneRow:set_value(tostring(done))
  startButton:set_text(runningSince and "Pause" or "Start")
end

local function setPhase(p)
  phase = p
  left = p == "work" and WORK or BREAK
  runningSince = nil
  show()
end

local function finish()
  if phase == "work" then
    done = done + 1
    store.set("done", done)
    setPhase("break")
    ui.alert("Time for a Break", "Five minutes. Stand up, stretch, look away from the screen.")
  else
    setPhase("work")
    ui.alert("Break Is Over", "Ready for the next session?")
  end
end

function tick()
  if not runningSince then return end
  local now = leftAtStart - (sys.millis() - runningSince) // 1000
  if now <= 0 then return finish() end
  if now ~= left then
    left = now
    clock:set_text(fmt(left))
  end
end

ui.page("Focus", function(page)
  clock = ui.label(page, fmt(left))
  local g = ui.group(page)
  phaseRow = ui.row(g, {icon = "clock", title = "Now", value = "Work"})
  doneRow = ui.row(g, {icon = "check", title = "Sessions Done", value = tostring(done)})
  startButton = ui.button(page, "Start", function()
    if runningSince then
      runningSince = nil      -- pause: keep what's left
    else
      runningSince = sys.millis()
      leftAtStart = left
    end
    show()
  end)
  local r = ui.group(page)
  ui.row(r, {icon = "refresh", title = "Skip to Next Phase", on_tap = function()
    setPhase(phase == "work" and "break" or "work")
  end})
  ui.row(r, {icon = "delete", title = "Reset Sessions", on_tap = function()
    done = 0
    store.set("done", done)
    show()
  end})
  show()
end)
