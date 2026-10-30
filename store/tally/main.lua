-- Tally: count anything with a tap. The count and the step survive restarts.
local count = store.get("count") or 0
local step = store.get("step") or 1
local countRow, stepRow, addButton

local function show()
  countRow:set_value(tostring(count))
  stepRow:set_value(tostring(step))
  addButton:set_text("+" .. step)
end

local function add(n)
  count = math.max(0, count + n)
  store.set("count", count)
  show()
end

local function stepPage()
  ui.page("Step", function(page)
    local g = ui.group(page, nil, "How much each tap on the big button adds.")
    local rows = {}
    for _, n in ipairs({1, 2, 5, 10}) do
      rows[n] = ui.row(g, {title = tostring(n), value = n == step and "Selected" or "", on_tap = function()
        step = n
        store.set("step", step)
        for k, r in pairs(rows) do r:set_value(k == n and "Selected" or "") end
        show()
      end})
    end
  end)
end

ui.page("Tally", function(page)
  local g = ui.group(page)
  countRow = ui.row(g, {icon = "add", title = "Count", value = tostring(count)})
  stepRow = ui.row(g, {icon = "settings", title = "Step", value = tostring(step), on_tap = stepPage})
  addButton = ui.button(page, "+" .. step, function() add(step) end)
  ui.button(page, "-1", function() add(-1) end)
  local r = ui.group(page)
  ui.row(r, {icon = "refresh", title = "Reset", on_tap = function()
    ui.alert("Reset the count?", "It goes back to 0.", {"Cancel", "Reset"}, function(i)
      if i == 2 then add(-count) end
    end)
  end})
end)
