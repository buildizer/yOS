# Script apps

Extra apps for yOS are small Lua 5.4 programs. Each one is a folder with two required files:

```
/apps/<id>/app.json   manifest
/apps/<id>/main.lua   the program, run each time the app is opened
```

The folder may also hold other files, and the app's own `store.json` (see `store` below).

## Where apps live

Apps are stored on the internal flash (LittleFS on the `spiffs` partition, mounted at boot and
formatted the first time if it can't be mounted) or on the SD card, both under `/apps`. A new app
goes to the internal flash as long as that leaves 256 KB free, otherwise to the SD card. Updating
an app (installing one with the same id) replaces it, wherever the rule then puts it.

Install apps from the web page at `http://<device>/apps`: choose the files (or the folder) and
press Install. The app appears on the Home Screen right away. The same page lists the installed
apps, with their location and size, and removes them. On the device, Settings › Apps › <app>
shows Location, Size and Version, and has **Remove App**.

Limits: 256 KB per file, 1 MB per app, file names of letters, digits, `.`, `-` and `_`.

## app.json

```json
{ "id": "counter", "name": "Counter", "icon": "add", "color": "#30D158", "version": "1.0" }
```

| Field | |
|---|---|
| `id` | 1-32 characters of `a-z`, `0-9`, `-`, `_`. Must match the folder name. Can't be a built-in app's id. |
| `name` | Home Screen label, up to 24 characters. |
| `icon` | One of `extension` (default), `globe`, `clock`, `sun`, `moon`, `cloud`, `folder`, `file`, `settings`, `wifi`, `router`, `dns`, `info`, `storage`, `sd`, `shield`, `lock`, `location`, `thermometer`, `bluetooth`, `refresh`, `keyboard`, `power`, `play`, `apps`, `add`, `check`, `delete`. |
| `color` | Icon tile colour, `#RRGGBB`. |
| `version` | Shown in Settings. |

Folders with a missing or broken manifest, a folder name that differs from `id`, no `main.lua`,
or an id that is already taken are skipped at boot; the serial log says why.

## The Lua API

Scripts get the standard `string`, `table`, `math`, `utf8` and `coroutine` libraries and the base
functions, without `dofile`, `loadfile`, `load`, `string.dump`, `io`, `os`, `debug` or `require`.
`print` goes to the serial log, cut to 200 bytes per call. On top of that:

### ui

UI built with these calls looks like the built-in apps (pages with large titles, inset grouped lists).

- `ui.page(title, function(page) ... end)` pushes a page and calls the function to fill it. The first
  page is the app's main page; later ones get a back button.
- `ui.group(page, header?, footer?)` an inset group; returns a group.
- `ui.row(group, {icon=, title=, value=, on_tap=function() end})` a row; `icon` takes the names
  above. Returns a row with `row:set_value(text)`.
- `ui.button(page, text, function() end)` a full-width button. Returns a handle with `:set_text(text)`.
- `ui.label(page, text)` a paragraph of wrapping text. Returns a label with `label:set_text(text)`.
- `ui.alert(title, message?, buttons?, function(index) end)` an alert with 1-3 buttons (default
  `{"OK"}`; a button called `"Cancel"` is styled as the cancel button). The function gets the
  1-based index of the tapped button.
- `ui.input(title, placeholder?, function(text) end)` a text entry page with the keyboard; the
  function runs when the person taps Done.

Limits: an app session can create up to 300 pages, groups, rows, buttons and labels, and any
text passed to `ui.*`, `set_text` or `set_value` may be up to 2048 bytes; beyond that the call
raises an error (which stops the app unless the script catches it with `pcall`).

Handles to things on a page that has been closed are harmless: `set_value`/`set_text` do nothing,
and passing a closed page or group to `ui.*` raises an error.

### net, json

- `net.get(url, function(status, body) ... end)` fetches an `http://` URL (plain HTTP only) in the
  background and returns at once; the function runs later, from the tick, with `status, body`, or
  `nil, message` when the request failed (bad URL, no Wi-Fi, timeout, body over 64 KB). 4 s
  timeouts. Only one request can run at a time (in total, across apps): calling `net.get` while
  one is pending raises an error, so start the next one from the callback. The screen never waits
  for the network. (The old blocking form `status, body = net.get(url)` is gone.)
- `json.decode(text)` -> value, or `nil, message`. Objects and arrays become tables; `null` becomes nil.

### store

- `store.get(key)` -> the saved string, number or boolean, or nil.
- `store.set(key, value)` saves a string, number or boolean (nil deletes the key). Saved in
  `store.json` in the app's folder (up to 16 KB), so it survives restarts and goes away with the app.
  The file is written at most every 5 s (from the tick) and when the app closes, not on every call.

### sys

- `sys.time()` Unix time in seconds (0-ish until the clock is set by NTP).
- `sys.millis()` milliseconds since boot.

### tick

If the script defines a global `function tick() ... end`, yOS calls it about 4 times a second
while the app is open.

## Safety

Each open app gets a fresh Lua state, closed when the app closes. Its memory comes from PSRAM and
is capped at 512 KB; each call into the script (a tap, an alert or input callback, a tick) may run
for 300 ms of wall-clock time, and main.lua for 1 s. Long-running string patterns count too:
`string.find`/`match`/`gmatch`/`gsub` check the clock while matching. Every call is protected: an
error, running out of memory or running too long stops the app and shows "This app stopped:
<message>" on its page instead of affecting the rest of the system.

Scripts run on the UI task's small stack, so nesting is limited: 40 levels of C calls (pcall,
metamethods, gsub callbacks, parser nesting) and 40 of pattern recursion, and a call fails with
"C stack overflow" as soon as the task has less than 3 KB of free stack.

## Code

- `lib/lua` Lua 5.4.7 from lua.org , built as one C++ unit (`lua_unity.cpp`) so Lua
  errors unwind C++ code properly. Three small `#ifdef YOS_HOOKS` patches (ldo.c, lstate.c,
  lstrlib.c) and the limits in `lua_port.h` add the stack and pattern-time checks.
- `sandbox.{h,cpp}` the capped allocator, time budget, safe libraries and `json.decode`; no
  Arduino dependencies. Desktop tests: `lib/lua/test/run.sh`.
- `script_app.{h,cpp}` the `ScriptApp` (an `App`) and the `ui`/`net`/`store`/`sys` bindings.
- `loader.{h,cpp}` mounting, scanning, installing and removing; used by `services/web.cpp`.

Install an app by uploading its folder (app.json + main.lua) on the web dashboard at /apps.
