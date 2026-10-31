-- Port Scanner: a TCP connect-scanner. It only does what any client does -- open a normal
-- connection and see if the port answers. Scan only devices and networks you own.
-- Open ports are listed with their common service name; the last result survives restarts.

local TIMEOUT = 1000     -- ms per connect
local RANGE_CAP = 1024   -- most ports a single range scan may cover

-- A small, well-known port list with service names (shown for open ports in either mode).
local COMMON = {
  {21, "ftp"}, {22, "ssh"}, {23, "telnet"}, {25, "smtp"}, {53, "dns"},
  {80, "http"}, {110, "pop3"}, {143, "imap"}, {443, "https"}, {445, "smb"},
  {554, "rtsp"}, {1883, "mqtt"}, {3000, "dev"}, {3306, "mysql"}, {3389, "rdp"},
  {5000, "upnp"}, {5432, "postgres"}, {6379, "redis"}, {7547, "tr-069"}, {8000, "http-alt"},
  {8080, "http-alt"}, {8443, "https-alt"}, {8883, "mqtt-tls"}, {9000, "http-alt"}, {32400, "plex"},
}
local SERVICE = {}
for _, p in ipairs(COMMON) do SERVICE[p[1]] = p[2] end

local target = store.get("target") or ""
local mode = "common"          -- "common" or "range"
local rangeFrom = store.get("from") or 1
local rangeTo = store.get("to") or 1024

local scanning = false
local stopReq = false
local queue, index, found   -- current scan state
local statusLabel, resultsLabel, targetRow, commonRow, rangeRow, startButton

local function serviceName(port)
  return SERVICE[port] or ""
end

-- Builds the queue of ports for the chosen mode, or nil + message when the input is bad.
local function buildQueue()
  if target == "" then return nil, "Enter a host or tap Scan my gateway." end
  local q = {}
  if mode == "common" then
    for _, p in ipairs(COMMON) do q[#q + 1] = p[1] end
  else
    local from, to = math.floor(rangeFrom), math.floor(rangeTo)
    if from < 1 or from > 65535 or to < 1 or to > 65535 then return nil, "Ports must be 1-65535." end
    if to < from then return nil, "The 'to' port must not be below 'from'." end
    if to - from + 1 > RANGE_CAP then return nil, "That range is over " .. RANGE_CAP .. " ports." end
    for p = from, to do q[#q + 1] = p end
  end
  return q
end

local function showResults()
  if #found == 0 then
    resultsLabel:set_text(scanning and "No open ports yet." or "No open ports found.")
    return
  end
  local lines = {}
  for _, f in ipairs(found) do
    local name = serviceName(f)
    lines[#lines + 1] = name == "" and tostring(f) or (f .. "  " .. name)
  end
  resultsLabel:set_text("Open ports:\n" .. table.concat(lines, "\n"))
end

local function finish(msg)
  scanning = false
  startButton:set_text("Start Scan")
  statusLabel:set_text(msg)
  showResults()
  -- Save the last result as a compact "host|p1,p2,..." string.
  store.set("target", target)
  store.set("last", target .. "|" .. table.concat(found, ","))
end

local function scanNext()
  if stopReq then return finish("Stopped at " .. (index - 1) .. "/" .. #queue .. ".") end
  if index > #queue then return finish("Done. " .. #found .. " open of " .. #queue .. ".") end
  local port = queue[index]
  statusLabel:set_text("Scanning " .. index .. "/" .. #queue .. " (port " .. port .. ")")
  local ok, err = pcall(net.tcp_open, target, port, TIMEOUT, function(result)
    if not scanning then return end        -- app may have moved on
    if result == "open" then
      found[#found + 1] = port
      showResults()
    end
    index = index + 1
    scanNext()
  end)
  if not ok then finish("Scan error: " .. tostring(err)) end
end

local function startScan()
  local q, err = buildQueue()
  if not q then return ui.alert("Can't scan", err) end
  queue, index, found, stopReq, scanning = q, 1, {}, false, true
  startButton:set_text("Stop")
  showResults()
  scanNext()
end

local function selectMode(m)
  mode = m
  commonRow:set_value(m == "common" and "Selected" or "")
  rangeRow:set_value(m == "range" and (rangeFrom .. "-" .. rangeTo) or "")
end

local function rangePage()
  ui.page("Port Range", function(page)
    local g = ui.group(page, nil, "Up to " .. RANGE_CAP .. " ports per scan.")
    local fromRow = ui.row(g, {icon = "add", title = "From", value = tostring(rangeFrom), on_tap = function()
      ui.input("From Port", tostring(rangeFrom), function(text)
        local n = tonumber(text)
        if n then rangeFrom = math.floor(n); store.set("from", rangeFrom) end
        fromRow:set_value(tostring(rangeFrom)); selectMode("range")
      end)
    end})
    local toRow = ui.row(g, {icon = "check", title = "To", value = tostring(rangeTo), on_tap = function()
      ui.input("To Port", tostring(rangeTo), function(text)
        local n = tonumber(text)
        if n then rangeTo = math.floor(n); store.set("to", rangeTo) end
        toRow:set_value(tostring(rangeTo)); selectMode("range")
      end)
    end})
  end)
end

ui.page("Port Scanner", function(page)
  statusLabel = ui.label(page, "TCP connect-scan. Only scan networks you own.")

  local tg = ui.group(page, "Target")
  targetRow = ui.row(tg, {icon = "router", title = "Host", value = target == "" and "Not set" or target, on_tap = function()
    ui.input("Host or IP", target, function(text)
      target = text or ""
      targetRow:set_value(target == "" and "Not set" or target)
    end)
  end})
  ui.row(tg, {icon = "wifi", title = "Scan my gateway", on_tap = function()
    local gw = net.gateway()
    if gw == "" or gw == "0.0.0.0" then return ui.alert("No gateway", "Connect to Wi-Fi first.") end
    target = gw
    targetRow:set_value(target)
  end})

  local mg = ui.group(page, "Mode")
  commonRow = ui.row(mg, {icon = "apps", title = "Common ports", value = "Selected", on_tap = function()
    selectMode("common")
  end})
  rangeRow = ui.row(mg, {icon = "dns", title = "Range", value = "", on_tap = function()
    selectMode("range")
    rangePage()
  end})

  resultsLabel = ui.label(page, "No open ports yet.")
  startButton = ui.button(page, "Start Scan", function()
    if scanning then
      stopReq = true
      statusLabel:set_text("Stopping...")
    else
      startScan()
    end
  end)

  -- Show the previous result, if any.
  local last = store.get("last")
  if last then
    local host, ports = last:match("^(.-)|(.*)$")
    if host and host ~= "" then
      statusLabel:set_text("Last: " .. host .. (ports == "" and " (none open)" or " -> " .. ports))
    end
  end
end)
