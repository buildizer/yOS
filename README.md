<p align="center">
  <img src="design/branding/yos-wordmark-light.svg" alt="yOS" width="200">
</p>

# yOS

Firmware for the Guition ESP32-4848S040 (ESP32-S3, 4" 480x480 touch display): an iOS-style home
screen, built-in apps, Lua script apps and an App Store. Build with PlatformIO (`pio run -e yos`).

## Phone remote (Bluetooth LE)

A paired phone or computer can control the display and move files to and from internal flash and
the SD card, using the web page in `tools/remote/index.html`.

**Use it.** Open the page from GitHub Pages, `https://<user>.github.io/<repo>/tools/remote/`
(for this repository: `https://buildizer.github.io/yos/tools/remote/`, with Pages serving the
default branch), or from any HTTPS host or `http://localhost` (Web Bluetooth needs a secure page).
On the display, turn on Settings › Bluetooth and *Discoverable as yOS*, then press Connect: the
first time, the display shows a 6-digit code to type on the phone. Web Bluetooth works in Chrome
and Edge on Android, Windows, macOS and ChromeOS; not in iOS Safari (on iPhone use the Bluefy
browser).

**Protocol.** Service `7e5a0001-4f53-4c59-9a1d-79a05b1e0001` (advertised while discoverable):

| UUID (`7e5a00xx-…`) | Properties | Purpose |
|---|---|---|
| `…02` | read | `"yOS (<build date>)"` |
| `…03` | read, encrypted + authenticated | `"paired"`; reading it starts passkey pairing |
| `…04` command | write, read, notify; encrypted + authenticated | JSON commands in, JSON replies out |
| `…05` data | write, write without response, notify; encrypted + authenticated | raw file bytes |

Every command is one JSON object written to the command characteristic (at most 512 bytes), e.g.
`{"cmd":"files","root":"sd","path":"/","tag":7}`. Each reply is a JSON line ending in `\n`, sent as
notifications of up to MTU−3 bytes (the board asks for MTU 247), with `"cmd"`, `"ok"`, `"error"`
when not ok, and the request's `"tag"` copied back. Reading the command characteristic returns
the last reply (first 512 bytes).

| Command | Arguments | Reply |
|---|---|---|
| `info` | | `name`, `version`, `uptime`, `heap` {free, internal, psram}, `wifi` {connected, ssid, ip, rssi}, `brightness` (0–100), `screen`, `sd`, `mtu`, `battery` (null: mains powered) |
| `apps` | | `apps`: [{id, name, running, open}] |
| `open` | `id` | ok once queued for the UI |
| `home` | | |
| `brightness` | `level` 0–100, `save` (default true; false while dragging) | |
| `screen` | `on` true/false | off = backlight off until a touch or `on` |
| `files` | `root` `"flash"`/`"sd"`, `path` | `entries` [{name, dir, size}] (200 max, then `more`), `total`, `used` |
| `get` | `root`, `path` | `size`; then the bytes as data notifications; then `{"cmd":"get_done","ok":…}` |
| `put` | `root`, `path`, `size` (32 MB max) | `window`, `chunk`; then write the bytes to the data characteristic |
| `delete` | `root`, `path` | file or empty folder |
| `mkdir` | `root`, `path` | |
| `cancel` | | ends the running transfer (`get_done`/`put_done` with `ok:false`) |

Uploads: write chunks of at most `chunk` bytes, never more than `window` bytes past the last
`{"ev":"ack","done":n}` (sent every 4 KB); the file is written as `<path>.part` and renamed when
complete, then `{"cmd":"put_done","ok":true,"size":n}` arrives. Paths are absolute within the
root, at most 120 characters, without `..`. One transfer runs at a time; one that makes no
progress for 15 s, or whose phone disconnects, is dropped. The display shows a small progress
pill while a file moves.

## iPhone Remote (Bluetooth keyboard, media keys, mouse)

The iPhone Remote app makes the display a Bluetooth LE HID device for a paired iPhone (or iPad,
Mac, Android phone): a keyboard, media keys and a mouse, with no extra hardware. Its HID, Battery
and Device Information services share the GATT server, passkey pairing and bonds with the phone
remote above.

**Pair.** On the display, turn on Settings › Bluetooth and *Discoverable as yOS* (the app has the
same switches while nothing is connected). On the iPhone open Settings › Bluetooth, tap **yOS**
under Other Devices and type the 6-digit code the display shows. A paired iPhone reconnects on
its own whenever yOS is discoverable. A phone that paired before this firmware cached the old
service list: forget yOS on the phone (and under Settings › Bluetooth › Paired Phones on the
display) and pair again.

**Use it.** The app sends previous / play-pause / next and volume down / mute / up, a *Camera
Shutter* button (volume up, which takes a photo in the Camera app), *Type Text…* (US layout,
printable ASCII; other characters are skipped) and eight shortcut buttons. Touch and hold a
shortcut to rename it, pick its key and modifiers, or restore its default; the shortcuts are
kept in the `yosremote` NVS namespace. Defaults: Home (⌘H), Search (⌘Space), Switch App (⌘Tab),
Escape, ←, →, Tab, Return. Other iPhone shortcuts that work with a hardware keyboard: in-app ones
such as ⌘C / ⌘V / ⌘Z / ⌘A, ⌘N and ⌘F where the app supports them, and arrows / Tab inside text
fields. With Settings › Accessibility › Keyboards › Full Keyboard Access turned on, Tab and the
arrows move the focus anywhere, Space activates, and Escape goes back. While a Bluetooth keyboard
is connected, iOS hides its on-screen keyboard (tap the keyboard icon at the bottom right of the
screen to show it).

**Pointer.** The touchpad sends relative mouse motion; a tap is a click. iOS shows the pointer
only with Settings › Accessibility › Touch › AssistiveTouch turned on (Pointer Devices there sets
the tracking speed and what the buttons do).
