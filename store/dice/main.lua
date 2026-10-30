-- Dice: roll one to four six-sided dice. Remembers how many you like to roll.
local count = store.get("count") or 2
local result, total, countRow

local function roll()
  local shown, sum = {}, 0
  for i = 1, count do
    local n = math.random(6)
    sum = sum + n
    shown[i] = tostring(n)
  end
  result:set_text(table.concat(shown, "   "))
  total:set_value(tostring(sum))
end

ui.page("Dice", function(page)
  result = ui.label(page, "Tap Roll to throw the dice.")
  local g = ui.group(page)
  total = ui.row(g, {icon = "add", title = "Total", value = ""})
  countRow = ui.row(g, {icon = "apps", title = "Dice", value = tostring(count), on_tap = function()
    count = count % 4 + 1  -- 1, 2, 3, 4, then back to 1
    store.set("count", count)
    countRow:set_value(tostring(count))
  end})
  ui.button(page, "Roll", roll)
end)
