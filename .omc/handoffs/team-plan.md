## Handoff: team-plan → team-exec
- **Decided**: Lead laid groundwork on ~/development/yos (verified on device): Settings split into src/apps/settings/{display,wifi,bluetooth,about}.cpp + settings.h; App::buildSettings(content, appRoot) hook rendered on Settings › Apps › <app>; prefs::use24h(); ui::textInput() keyboard page; os::unregisterApp/rebuildHome; NimBLE-Arduino dep; stubs bleBegin()/loadScriptApps() called from main.cpp.
- **Ownership**: w1 system-ui (os.cpp, hal.*, settings/display.cpp); w2 app-settings (weather.*, weather_app.cpp, clock_app.cpp, new countries.h); w3 wifi (services/wifi_manager.*, settings/wifi.cpp, main.cpp); w4 bluetooth (services/ble.*, settings/bluetooth.cpp); w5 script apps (lib/lua/**, src/script/**, services/web.cpp).
- **Rejected**: workers sharing one tree (single .pio build dir, file clobbering); ELF/firmware-bundle SD apps (user chose Lua).
- **Risks**: one physical board, so only the lead flashes; RAM (NimBLE + Lua + LVGL); PSRAM bandwidth tearing (keep pclk 12 MHz, CPU copy).
- **Remaining**: implement in isolated copies ~/development/yos-work/wN, lead merges owned files, flashes, verifies.
