#include "services/web.h"
#include <WebServer.h>
#include <ArduinoJson.h>
#include "services/monitor.h"
#include "services/storage.h"
#include <SD.h>
#include "os/hal.h"
#include <esp32s3/rom/cache.h>
#include "os/os.h"
#include "os/prefs.h"
#include "os/wallpaper.h"
#include "script/loader.h"
#include "os/ota_screen.h"
#include "services/firmware_update.h"
#include "services/github_update.h"

static WebServer server(80);
static Snapshot snap;

static const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Netwatch</title>
<style>
:root{--bg:#0f1115;--card:#181b22;--fg:#e6e8eb;--dim:#8a919c;--ok:#2ecc71;--bad:#e74c3c;--warn:#3498db}
body{margin:0;font:15px/1.4 system-ui,sans-serif;background:var(--bg);color:var(--fg)}
main{max-width:720px;margin:0 auto;padding:16px}
h1{font-size:20px;margin:0 0 12px;display:flex;justify-content:space-between}
h1 small{color:var(--dim);font-weight:400;font-size:13px}
.card{background:var(--card);border-radius:10px;padding:12px 14px;margin-bottom:12px}
.row{display:flex;align-items:center;gap:10px;padding:6px 0;border-bottom:1px solid #222631}
.row:last-child{border:0}
.dot{width:12px;height:12px;border-radius:50%;background:var(--dim);flex:none}
.up .dot{background:var(--ok)}.down .dot{background:var(--bad)}
.name{flex:1}.ip,.since{color:var(--dim);font-size:13px}.rtt{width:70px;text-align:right;font-variant-numeric:tabular-nums}
.stats{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;text-align:center}
.stats b{display:block;font-size:20px}.stats span{color:var(--dim);font-size:12px}
table{width:100%;border-collapse:collapse;font-size:13px}td{padding:4px 0;border-bottom:1px solid #222631}
.u{color:var(--ok)}.d{color:var(--bad)}
</style></head><body><main>
<h1>Netwatch <small><a href="/apps" style="color:#8a919c;margin-right:8px">Apps</a><a href="/wallpapers" style="color:#8a919c;margin-right:8px">Wallpapers</a><a href="/sd" style="color:#8a919c;margin-right:8px">SD card</a><span id="clock"></span></small></h1>
<div class="card" id="targets"></div>
<div class="card"><div class="stats" id="adg"></div></div>
<div class="card"><table id="events"></table></div>
</main><script>
const fmtDur=s=>s<60?s+'s':s<3600?Math.floor(s/60)+'m '+s%60+'s':Math.floor(s/3600)+'h '+Math.floor(s%3600/60)+'m';
const fmtTime=t=>t>1e9?new Date(t*1000).toLocaleString():'-';
async function tick(){
  try{
    const d=await (await fetch('/api/status')).json();
    document.getElementById('clock').textContent=fmtTime(d.now)+' · '+d.ip;
    const rows=[...d.targets,d.dns].map(t=>`<div class="row ${t.known?(t.up?'up':'down'):''}"><div class="dot"></div>
      <div class="name">${t.name}<div class="ip">${t.ip}</div></div>
      <div class="since">${t.known?(t.up?'up ':'DOWN ')+fmtDur(d.now-t.since):'checking'}</div>
      <div class="rtt">${t.up?t.rtt.toFixed(0)+' ms':'—'}</div></div>`);
    document.getElementById('targets').innerHTML=rows.join('');
    const a=d.adguard;
    document.getElementById('adg').innerHTML=!a.known?'<span>AdGuard stats off (log in from Settings › Apps › Netwatch)</span>':!a.ok?'<span class="d">AdGuard API unreachable</span>':
      `<div><b>${a.queries.toLocaleString()}</b><span>queries</span></div><div><b>${a.queries?(100*a.blocked/a.queries).toFixed(1):0}%</b><span>blocked</span></div><div><b>${a.avg_ms.toFixed(1)} ms</b><span>avg processing</span></div>`;
    document.getElementById('events').innerHTML=d.events.length?d.events.map(e=>`<tr><td>${fmtTime(e.at)}</td><td>${e.what}</td>
      <td class="${e.up?'u':'d'}">${e.up?'UP (down '+fmtDur(e.down_s)+')':'DOWN'}</td></tr>`).join(''):'<tr><td>No outages recorded since boot</td></tr>';
  }catch(e){document.getElementById('clock').textContent='connection lost'}
}
tick();setInterval(tick,2000);
</script></body></html>)HTML";

static void handleStatus() {
  monitorSnapshot(snap);
  JsonDocument doc;
  doc["now"] = (uint32_t)time(nullptr);
  doc["ip"] = WiFi.localIP().toString();
  JsonArray targets = doc["targets"].to<JsonArray>();
  for (int i = 0; i < NUM_TARGETS; i++) {
    const TargetState &t = snap.targets[i];
    JsonObject o = targets.add<JsonObject>();
    o["name"] = TARGETS[i].name;
    o["ip"] = TARGETS[i].ip.toString();
    o["known"] = t.known;
    o["up"] = t.up;
    o["rtt"] = t.rttMs;
    o["since"] = (uint32_t)t.since;
  }
  JsonObject dns = doc["dns"].to<JsonObject>();
  dns["name"] = "DNS via AdGuard";
  dns["ip"] = String("resolving ") + snap.dns.lastName;
  dns["known"] = snap.dns.known;
  dns["up"] = snap.dns.up;
  dns["rtt"] = snap.dns.rttMs;
  dns["since"] = (uint32_t)snap.dns.since;
  JsonObject adg = doc["adguard"].to<JsonObject>();
  adg["known"] = snap.adguard.known;
  adg["ok"] = snap.adguard.ok;
  adg["queries"] = snap.adguard.queries;
  adg["blocked"] = snap.adguard.blocked;
  adg["avg_ms"] = snap.adguard.avgMs;
  JsonArray events = doc["events"].to<JsonArray>();
  for (int i = 0; i < snap.numEvents; i++) {
    JsonObject e = events.add<JsonObject>();
    e["at"] = (uint32_t)snap.events[i].at;
    e["what"] = snap.events[i].what;
    e["up"] = snap.events[i].up;
    e["down_s"] = snap.events[i].downSecs;
  }
  String body;
  serializeJson(doc, body);
  server.send(200, "application/json", body);
}

static const char SD_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Netwatch SD</title>
<style>
:root{--bg:#0f1115;--card:#181b22;--fg:#e6e8eb;--dim:#8a919c;--ok:#2ecc71;--bad:#e74c3c;--warn:#f39c12}
body{margin:0;font:15px/1.4 system-ui,sans-serif;background:var(--bg);color:var(--fg)}
main{max-width:720px;margin:0 auto;padding:16px}a{color:#5dade2}
h1{font-size:20px;margin:0 0 12px;display:flex;justify-content:space-between;align-items:baseline}
h1 a{font-size:13px;font-weight:400}
.card{background:var(--card);border-radius:10px;padding:12px 14px;margin-bottom:12px}
.bar{height:10px;background:#222631;border-radius:5px;overflow:hidden;margin:8px 0}.bar div{height:100%;background:var(--ok)}
.dim{color:var(--dim);font-size:13px}
table{width:100%;border-collapse:collapse}td{padding:6px 0;border-bottom:1px solid #222631}
td.n{word-break:break-all}td.s{text-align:right;color:var(--dim);font-size:13px;white-space:nowrap;padding:0 10px}
button{font:inherit;border:0;border-radius:6px;padding:6px 12px;cursor:pointer;background:#2a2f3a;color:var(--fg)}
button.warn{background:var(--warn);color:#000}button.bad{background:var(--bad);color:#fff}
.actions{display:flex;gap:8px;flex-wrap:wrap;margin-top:10px}
#msg{min-height:20px}
</style></head><body><main>
<h1>SD card <a href="/">&larr; Dashboard</a></h1>
<div class="card"><div id="usage">Loading...</div><div class="bar"><div id="bar" style="width:0"></div></div>
<div class="actions"><button class="warn" onclick="act('clear','Delete all Netwatch logs (outage log and daily CSVs)?')">Clear logs</button>
<button class="bad" onclick="act('format','FORMAT the SD card? Everything on it will be erased.')">Format card</button></div>
<div id="msg" class="dim"></div></div>
<div class="card"><div class="dim" id="path"></div><table id="files"></table></div>
</main><script>
let dir='/netwatch';
const fmt=b=>b<1024?b+' B':b<1048576?(b/1024).toFixed(1)+' KB':b<1073741824?(b/1048576).toFixed(1)+' MB':(b/1073741824).toFixed(2)+' GB';
const enc=encodeURIComponent;
async function load(){
  const d=await (await fetch('/api/sd?dir='+enc(dir))).json();
  const jobs={1:'Working...',2:'Done',3:'FAILED'};
  document.getElementById('msg').textContent=d.job_state?(d.job=='format'?'Format: ':'Clear logs: ')+jobs[d.job_state]:'';
  if(!d.ready){document.getElementById('usage').textContent='No SD card mounted. Insert a card, or format it if it is unreadable.';
    document.getElementById('files').innerHTML='';document.getElementById('bar').style.width='0';return}
  document.getElementById('usage').textContent=fmt(d.used)+' used of '+fmt(d.total)+' ('+(100*d.used/d.total).toFixed(1)+'%)';
  document.getElementById('bar').style.width=(100*d.used/d.total)+'%';
  document.getElementById('path').textContent=dir;
  let rows=dir!='/'?`<tr><td class="n"><a href="#" onclick="cd('${dir.replace(/\/[^\/]*$/,'')||'/'}');return false">..</a></td><td></td><td></td></tr>`:'';
  rows+=d.entries.map(e=>{const p=(dir=='/'?'':dir)+'/'+e.name;
    return e.dir?`<tr><td class="n"><a href="#" onclick="cd('${p}');return false">${e.name}/</a></td><td class="s"></td><td><button onclick="del('${p}')">Delete</button></td></tr>`
    :`<tr><td class="n"><a href="/sd/download?path=${enc(p)}">${e.name}</a></td><td class="s">${fmt(e.size)}</td><td><button onclick="del('${p}')">Delete</button></td></tr>`}).join('');
  document.getElementById('files').innerHTML=rows||'<tr><td class="dim">Empty</td></tr>';
}
function cd(p){dir=p;load()}
async function del(p){if(!confirm('Delete '+p+'?'))return;const r=await fetch('/api/sd/delete?path='+enc(p),{method:'POST'});if(!r.ok)alert(await r.text());load()}
async function act(a,q){if(!confirm(q))return;const r=await fetch('/api/sd/'+a,{method:'POST'});if(!r.ok)alert(await r.text());load()}
load();setInterval(load,3000);
</script></body></html>)HTML";

// Only absolute paths without ".." are allowed.
static bool safePath(const String &p) { return p.startsWith("/") && p.indexOf("..") < 0; }

static const char *jobName(StorageJob j) { return j == JOB_FORMAT ? "format" : j == JOB_CLEAR_LOGS ? "clear" : ""; }

static void handleSdList() {
  String dir = server.hasArg("dir") ? server.arg("dir") : "/";
  if (!safePath(dir)) return server.send(400, "text/plain", "bad path");
  JsonDocument doc;
  doc["ready"] = storageReady();
  doc["job"] = jobName(storageCurrentJob());
  doc["job_state"] = (int)storageJobState();
  JsonArray entries = doc["entries"].to<JsonArray>();
  if (storageReady() && storageJobState() != JOB_RUNNING) {
    storageWithLock([&] {
      doc["total"] = SD.totalBytes();
      doc["used"] = SD.usedBytes();
      File d = SD.open(dir);
      if (d && d.isDirectory()) {
        for (File f = d.openNextFile(); f; f = d.openNextFile()) {
          JsonObject e = entries.add<JsonObject>();
          e["name"] = String(f.name());
          e["dir"] = f.isDirectory();
          e["size"] = f.size();
          f.close();
        }
      }
      if (d) d.close();
    });
  }
  String body;
  serializeJson(doc, body);
  server.send(200, "application/json", body);
}

static void handleSdDownload() {
  String path = server.arg("path");
  if (!safePath(path) || !storageReady()) return server.send(400, "text/plain", "bad path");
  // Open under the lock, then stream in 4 KB chunks taking the lock per read so the monitor log and UI stay unblocked.
  File f;
  size_t size = 0;
  bool ok = false;
  storageWithLock([&] {
    f = SD.open(path);
    if (f && !f.isDirectory()) {
      size = f.size();
      ok = true;
    }
  });
  if (!ok) {
    storageWithLock([&] { if (f) f.close(); });
    return server.send(404, "text/plain", "not found");
  }
  String name = path.substring(path.lastIndexOf('/') + 1);
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
  server.setContentLength(size);
  server.send(200, name.endsWith(".csv") ? "text/csv" : "application/octet-stream", "");
  static uint8_t buf[4096];  // handlers run one at a time in loopTask
  size_t left = size;
  while (left) {
    size_t n = 0;
    storageWithLock([&] { n = f.read(buf, std::min(left, sizeof(buf))); });
    if (!n || server.client().write(buf, n) != n) break;
    left -= n;
  }
  storageWithLock([&] { f.close(); });
}

static void handleSdDelete() {
  String path = server.arg("path");
  if (!safePath(path) || path == "/" || !storageReady()) return server.send(400, "text/plain", "bad path");
  bool ok = false;
  storageWithLock([&] {
    File f = SD.open(path);
    bool isDir = f && f.isDirectory();
    if (f) f.close();
    ok = isDir ? SD.rmdir(path) : SD.remove(path);
  });
  if (ok) server.send(200, "text/plain", "ok");
  else server.send(409, "text/plain", "Could not delete (folders must be empty)");
}

static void startJob(StorageJob job) {
  if (storageStartJob(job)) server.send(200, "text/plain", "started");
  else server.send(409, "text/plain", "Another SD job is still running");
}

#ifdef YOS_DEV_TOOLS
// Developer tools: screenshot of the panel as a 24-bit BMP, and simulated touches.
static void handleScreenshot() {
  const int W = 480, H = 480, rowBytes = W * 3, size = 54 + rowBytes * H;
  uint8_t header[54] = {'B', 'M'};
  auto put32 = [&](int off, uint32_t v) { memcpy(header + off, &v, 4); };
  put32(2, size); put32(10, 54); put32(14, 40); put32(18, W); put32(22, H);
  header[26] = 1; header[28] = 24; put32(34, rowBytes * H);
  WiFiClient client = server.client();
  server.setContentLength(size);
  server.send(200, "image/bmp", "");
  client.write(header, sizeof(header));
  uint16_t *fb = hal::framebuffer();
  // No cache invalidation here: the framebuffer is written through the cache by the copy task, and
  // invalidating could discard lines it hasn't written back yet, corrupting the live screen.
  static uint8_t row[480 * 3];
  for (int y = H - 1; y >= 0; y--) {  // BMP rows go bottom-up
    for (int x = 0; x < W; x++) {
      uint16_t c = fb[y * W + x];
      row[x * 3 + 0] = (c & 0x1F) << 3;
      row[x * 3 + 1] = ((c >> 5) & 0x3F) << 2;
      row[x * 3 + 2] = (c >> 11) << 3;
    }
    client.write(row, rowBytes);
  }
}

static void handleTouch() {
  int x1 = server.arg("x").toInt(), y1 = server.arg("y").toInt();
  int x2 = server.hasArg("x2") ? server.arg("x2").toInt() : x1;
  int y2 = server.hasArg("y2") ? server.arg("y2").toInt() : y1;
  hal::injectTouch(x1, y1, x2, y2, server.hasArg("ms") ? server.arg("ms").toInt() : 80);
  server.send(200, "text/plain", "ok");
}
#endif

// ---------- script apps ----------

static const char APPS_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>yOS Apps</title>
<style>
:root{--bg:#0f1115;--card:#181b22;--fg:#e6e8eb;--dim:#8a919c;--ok:#2ecc71;--bad:#e74c3c}
body{margin:0;font:15px/1.4 system-ui,sans-serif;background:var(--bg);color:var(--fg)}
main{max-width:720px;margin:0 auto;padding:16px}a{color:#5dade2}
h1{font-size:20px;margin:0 0 12px;display:flex;justify-content:space-between;align-items:baseline}
h1 a{font-size:13px;font-weight:400}h2{font-size:15px;margin:0 0 8px}
.card{background:var(--card);border-radius:10px;padding:12px 14px;margin-bottom:12px}
.dim{color:var(--dim);font-size:13px}
table{width:100%;border-collapse:collapse}td{padding:6px 0;border-bottom:1px solid #222631}
td.s{text-align:right;color:var(--dim);font-size:13px;white-space:nowrap;padding:0 10px}
button{font:inherit;border:0;border-radius:6px;padding:6px 12px;cursor:pointer;background:#2a2f3a;color:var(--fg)}
button.bad{background:var(--bad);color:#fff}button.go{background:#0a84ff;color:#fff}
label{display:block;margin:6px 0}#msg{min-height:20px;margin-top:8px}.err{color:var(--bad)}.okc{color:var(--ok)}
</style></head><body><main>
<h1>Apps <a href="/">&larr; Dashboard</a></h1>
<div class="card"><table id="apps"></table><div class="dim" id="space"></div></div>
<div class="card"><h2>Install an app</h2>
<div class="dim">Pick the app's files (app.json, main.lua and any others) or its folder. Apps go to this device while it has room, otherwise to the SD card. Installing an app with the same id updates it.</div>
<label>Files: <input type="file" id="files" multiple></label>
<label>or folder: <input type="file" id="folder" webkitdirectory></label>
<button class="go" onclick="install()">Install</button><div id="msg"></div></div>
</main><script>
const fmt=b=>b<1024?b+' B':b<1048576?(b/1024).toFixed(1)+' KB':(b/1048576).toFixed(1)+' MB';
const esc=s=>String(s).replace(/[&<>"']/g,c=>'&#'+c.charCodeAt(0)+';');
const msg=(t,c)=>{const m=document.getElementById('msg');m.textContent=t;m.className=c||''};
async function load(){
  const d=await (await fetch('/api/apps')).json();
  document.getElementById('apps').innerHTML=d.apps.length?d.apps.map(a=>`<tr><td>${esc(a.name)} <span class="dim">${esc(a.id)} v${esc(a.version)}</span></td>
    <td class="s">${a.sd?'SD Card':'On This Device'}</td><td class="s">${fmt(a.size)}</td>
    <td><button class="bad" data-id="${esc(a.id)}" data-name="${esc(a.name)}" onclick="del(this.dataset.id,this.dataset.name)">Remove</button></td></tr>`).join(''):'<tr><td class="dim">No apps installed</td></tr>';
  document.getElementById('space').textContent='Free on this device: '+fmt(d.internal_free)+' · SD card: '+(d.sd_ready?'ready':'not inserted');
}
async function del(id,name){if(!confirm('Remove '+name+' and its data?'))return;
  const r=await fetch('/api/apps/remove?id='+encodeURIComponent(id),{method:'POST'});if(!r.ok)alert(await r.text());load()}
async function install(){
  const files=[...document.getElementById('files').files,...document.getElementById('folder').files];
  if(!files.length)return msg('Choose the app files first','err');
  const fd=new FormData();for(const f of files)fd.append('file',f,f.name);
  msg('Installing...');
  try{const r=await fetch('/api/apps/install',{method:'POST',body:fd});const t=await r.text();
    if(r.ok){const a=JSON.parse(t);msg('Installed '+a.name+(a.sd?' on the SD card':' on this device'),'okc')}else msg(t,'err')}
  catch(e){msg('Upload failed: '+e,'err')}
  load();
}
load();
</script></body></html>)HTML";

static std::vector<ScriptAppFile> uploads;
static size_t uploadBytes = 0;
static String uploadError;

static void handleAppsList() {
  JsonDocument doc;
  JsonArray apps = doc["apps"].to<JsonArray>();
  for (const ScriptAppInfo &a : scriptAppList()) {
    JsonObject o = apps.add<JsonObject>();
    o["id"] = a.id;
    o["name"] = a.name;
    o["version"] = a.version;
    o["sd"] = a.onSd;
    o["size"] = a.bytes;
  }
  doc["internal_free"] = scriptInternalFree();
  doc["sd_ready"] = storageReady();
  String body;
  serializeJson(doc, body);
  server.send(200, "application/json", body);
}

// Called for each chunk of each uploaded file; the files are kept in memory (PSRAM) until the
// request ends, since app.json may arrive after main.lua.
static void handleAppUpload() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    String name = up.filename.substring(up.filename.lastIndexOf('/') + 1);  // folder uploads may send a path
    uploads.push_back({name, std::string()});
  } else if (up.status == UPLOAD_FILE_WRITE && !uploads.empty()) {
    uploadBytes += up.currentSize;
    if (uploadBytes > 1024 * 1024) uploadError = "An app may be at most 1 MB";
    else uploads.back().data.append((const char *)up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    uploads.clear();
    uploadBytes = 0;
    uploadError = "";
  }
}

static void handleAppInstall() {
  String err = uploadError;
  ScriptAppInfo info;
  bool ok = err.isEmpty() && scriptAppInstall(uploads, err, &info);
  uploads.clear();
  uploads.shrink_to_fit();
  uploadBytes = 0;
  uploadError = "";
  if (!ok) return server.send(400, "text/plain", err);
  JsonDocument doc;
  doc["id"] = info.id;
  doc["name"] = info.name;
  doc["sd"] = info.onSd;
  doc["size"] = info.bytes;
  String body;
  serializeJson(doc, body);
  server.send(200, "application/json", body);
}

static void handleAppRemove() {
  String err;
  if (scriptAppRemove(server.arg("id").c_str(), err)) server.send(200, "text/plain", "ok");
  else server.send(404, "text/plain", err);
}

// ---------- wallpapers ----------

static const char WALLPAPERS_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>yOS Wallpapers</title>
<style>
:root{--bg:#0f1115;--card:#181b22;--fg:#e6e8eb;--dim:#8a919c;--ok:#2ecc71;--bad:#e74c3c}
body{margin:0;font:15px/1.4 system-ui,sans-serif;background:var(--bg);color:var(--fg)}
main{max-width:720px;margin:0 auto;padding:16px}a{color:#5dade2}
h1{font-size:20px;margin:0 0 12px;display:flex;justify-content:space-between;align-items:baseline}
h1 a{font-size:13px;font-weight:400}h2{font-size:15px;margin:0 0 8px}
.card{background:var(--card);border-radius:10px;padding:12px 14px;margin-bottom:12px}
.dim{color:var(--dim);font-size:13px}code{background:#222631;padding:1px 5px;border-radius:4px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(110px,1fr));gap:12px}
.w{text-align:center}.w canvas,.w .ph{width:96px;height:96px;border-radius:14px;background:#222631;display:block;margin:0 auto 4px}
.w .n{word-break:break-all;font-size:13px}.cur{outline:3px solid #0a84ff;outline-offset:2px}
button{font:inherit;border:0;border-radius:6px;padding:4px 10px;cursor:pointer;background:#2a2f3a;color:var(--fg);margin-top:4px}
button.bad{background:var(--bad);color:#fff}button.go{background:#0a84ff;color:#fff;padding:6px 12px}
label{display:block;margin:6px 0}#msg{min-height:20px;margin-top:8px}.err{color:var(--bad)}.okc{color:var(--ok)}
</style></head><body><main>
<h1>Wallpapers <a href="/">&larr; Dashboard</a></h1>
<div class="card"><div class="grid" id="list"></div><div class="dim" id="info"></div></div>
<div class="card"><h2>Add wallpapers</h2>
<div class="dim">Convert a picture with <code>python3 tools/make_wallpaper.py photo.png</code>, which writes <code>photo.rgb565</code> (480x480, 460800 bytes) and <code>photo.thumb</code> (96x96, 18432 bytes). Upload both here, then choose it on the device in Settings &rsaquo; Display &amp; Brightness &rsaquo; Wallpaper. Names may use letters, digits, <code>_</code> and <code>-</code>.</div>
<label>Files: <input type="file" id="files" multiple accept=".rgb565,.thumb"></label>
<button class="go" onclick="up()">Upload</button><div id="msg"></div></div>
</main><script>
const esc=s=>String(s).replace(/[&<>"']/g,c=>'&#'+c.charCodeAt(0)+';');
const msg=(t,c)=>{const m=document.getElementById('msg');m.textContent=t;m.className=c||''};
async function thumb(cv,name){
  try{const b=new DataView(await (await fetch('/sd/download?path='+encodeURIComponent('/wallpapers/'+name+'.thumb'))).arrayBuffer());
    if(b.byteLength!=18432)return;const x=cv.getContext('2d'),im=x.createImageData(96,96);
    for(let i=0;i<9216;i++){const c=b.getUint16(i*2,true);im.data[i*4]=(c>>11)<<3;im.data[i*4+1]=((c>>5)&63)<<2;im.data[i*4+2]=(c&31)<<3;im.data[i*4+3]=255}
    x.putImageData(im,0,0)}catch(e){}
}
async function load(){
  const d=await (await fetch('/api/wallpapers')).json();
  document.getElementById('info').textContent=d.ready?(d.items.length?'':'No wallpapers yet'):'No SD card mounted';
  document.getElementById('list').innerHTML=d.items.map(w=>`<div class="w">${w.thumb?`<canvas width="96" height="96" data-n="${esc(w.name)}" class="${w.name==d.current?'cur':''}"></canvas>`:`<div class="ph ${w.name==d.current?'cur':''}"></div>`}
    <div class="n">${esc(w.name)}</div><button class="bad" data-n="${esc(w.name)}" onclick="del(this.dataset.n)">Remove</button></div>`).join('');
  document.querySelectorAll('canvas[data-n]').forEach(c=>thumb(c,c.dataset.n));
}
async function del(n){if(!confirm('Remove '+n+'?'))return;
  const r=await fetch('/api/wallpapers/remove?name='+encodeURIComponent(n),{method:'POST'});if(!r.ok)alert(await r.text());load()}
async function up(){
  const files=[...document.getElementById('files').files];
  if(!files.length)return msg('Choose .rgb565 (and .thumb) files first','err');
  const fd=new FormData();for(const f of files)fd.append('file',f,f.name);
  msg('Uploading...');
  try{const r=await fetch('/api/wallpapers/upload',{method:'POST',body:fd});const t=await r.text();
    if(r.ok)msg('Uploaded '+JSON.parse(t).saved.join(', '),'okc');else msg(t,'err')}
  catch(e){msg('Upload failed: '+e,'err')}
  load();
}
load();
</script></body></html>)HTML";

static const char *WP_DIR = "/wallpapers";

static void handleWallpaperList() {
  JsonDocument doc;
  doc["ready"] = storageReady();
  doc["current"] = wallpaper::current();
  JsonArray items = doc["items"].to<JsonArray>();
  for (const wallpaper::Info &w : wallpaper::list()) {
    JsonObject o = items.add<JsonObject>();
    o["name"] = w.name;
    o["thumb"] = w.thumb;
  }
  String body;
  serializeJson(doc, body);
  server.send(200, "application/json", body);
}

// Upload state for the file currently streaming to the SD card. Each chunk is written under the SD
// lock; the file lands as <name>.<ext>.part and is renamed once its size checks out.
static File wpFile;
static String wpPart, wpFinal, wpError;
static size_t wpGot = 0, wpWant = 0;
static std::vector<String> wpSaved;
static bool wpTouchedCurrent = false;

static void wpReset() {
  wpError = "";
  wpSaved.clear();
  wpTouchedCurrent = false;
}

static void handleWallpaperUpload() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    if (wpFile) storageWithLock([&] { wpFile.close(); });
    wpFinal = "";
    String fn = up.filename.substring(up.filename.lastIndexOf('/') + 1);
    int dot = fn.lastIndexOf('.');
    String base = dot > 0 ? fn.substring(0, dot) : fn, ext = dot > 0 ? fn.substring(dot + 1) : "";
    ext.toLowerCase();
    if (!wpError.isEmpty()) return;
    if (!storageReady()) { wpError = "No SD card mounted"; return; }
    if (!wallpaper::validName(base)) { wpError = "Bad file name (use letters, digits, _ and -): " + fn; return; }
    if (ext == "rgb565") wpWant = wallpaper::FULL_BYTES;
    else if (ext == "thumb") wpWant = wallpaper::THUMB_BYTES;
    else { wpError = "Only .rgb565 and .thumb files are accepted: " + fn; return; }
    wpFinal = String(WP_DIR) + "/" + base + "." + ext;
    wpPart = wpFinal + ".part";
    wpGot = 0;
    storageWithLock([&] {
      if (!SD.exists(WP_DIR)) SD.mkdir(WP_DIR);
      wpFile = SD.open(wpPart, FILE_WRITE);
    });
    if (!wpFile) { wpError = "Could not write to the SD card"; wpFinal = ""; }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!wpFile || wpFinal.isEmpty()) return;
    wpGot += up.currentSize;
    if (wpGot > wpWant) {  // too big: stop writing, the size check on END reports it
      return;
    }
    size_t w = 0;
    storageWithLock([&] { w = wpFile.write(up.buf, up.currentSize); });
    if (w != up.currentSize) wpError = "SD card write failed (full?)";
  } else if (up.status == UPLOAD_FILE_END) {
    if (wpFinal.isEmpty()) return;
    bool ok = wpError.isEmpty() && wpGot == wpWant;
    String name = wpFinal.substring(strlen(WP_DIR) + 1);
    storageWithLock([&] {
      if (wpFile) wpFile.close();
      if (ok) {
        SD.remove(wpFinal);
        ok = SD.rename(wpPart, wpFinal);
      }
      if (!ok) SD.remove(wpPart);
    });
    if (wpError.isEmpty() && wpGot != wpWant)
      wpError = name + " must be exactly " + String((unsigned)wpWant) + " bytes (got " + String((unsigned)wpGot) + ")";
    else if (!ok && wpError.isEmpty()) wpError = "Could not save " + name;
    if (ok) {
      wpSaved.push_back(name);
      if (name.substring(0, name.lastIndexOf('.')) == wallpaper::current()) wpTouchedCurrent = true;
    }
    wpFinal = "";
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (wpError.isEmpty()) wpError = "Upload aborted";
    storageWithLock([&] {
      if (wpFile) wpFile.close();
      if (!wpPart.isEmpty()) SD.remove(wpPart);
    });
    wpFinal = "";
  }
}

static void handleWallpaperUploadDone() {
  String err = wpError;
  if (err.isEmpty() && wpSaved.empty()) err = "No files received";
  if (wpTouchedCurrent) {  // the selected wallpaper was replaced: reload it on the Home Screen
    hal::lock();
    wallpaper::invalidate();
    os::rebuildHome();
    hal::unlock();
  }
  JsonDocument doc;
  JsonArray saved = doc["saved"].to<JsonArray>();
  for (const String &n : wpSaved) saved.add(n);
  wpReset();
  if (!err.isEmpty()) return server.send(400, "text/plain", err);
  String body;
  serializeJson(doc, body);
  server.send(200, "application/json", body);
}

static void handleWallpaperRemove() {
  String name = server.arg("name");
  if (!wallpaper::validName(name) || !storageReady()) return server.send(400, "text/plain", "bad name");
  bool found = false;
  storageWithLock([&] {
    for (const char *ext : {".rgb565", ".thumb"}) {
      String path = String(WP_DIR) + "/" + name + ext;
      if (SD.exists(path)) found = SD.remove(path) || found;
    }
  });
  if (!found) return server.send(404, "text/plain", "not found");
  if (name == wallpaper::current()) {  // back to the built-in gradient
    hal::lock();
    wallpaper::set("");
    os::rebuildHome();
    hal::unlock();
  }
  server.send(200, "text/plain", "ok");
}

// ---------- firmware push (tools/push_update.py) ----------

// POST /api/update: the request body is the image (firmware.bin.gz as built, or firmware.bin), sent as
// application/octet-stream with HTTP Basic auth "yos" + the Update Password. It is written while it
// arrives, so a push takes about as long as the transfer plus the last flash writes.
static String pushError;
static bool pushSeen = false, pushStarted = false, pushUnauthorized = false;  // per request
static uint32_t pushT0 = 0;

static void handleFirmwareChunk() {
  HTTPRaw &raw = server.raw();
  if (raw.status == RAW_START) {
    pushError = "";
    pushSeen = true;
    pushStarted = pushUnauthorized = false;
    pushT0 = millis();
    String pw = prefs::otaPassword();
    if (pw.isEmpty() || !server.authenticate("yos", pw.c_str())) { pushUnauthorized = true; return; }
    if (ghupdate::installing()) { pushError = "A GitHub update is being installed."; return; }
    if (!fwupdate::begin("Wi-Fi push", 0, pushError)) return;
    pushStarted = true;
    otaScreenBegin("Installing over Wi-Fi");
  } else if (raw.status == RAW_WRITE) {
    if (!pushStarted || !pushError.isEmpty()) return;  // refused: the rest of the body is read and dropped
    if (!fwupdate::write(raw.buf, raw.currentSize)) pushError = fwupdate::error();
    else otaScreenProgress(raw.totalSize, server.clientContentLength());
  } else if (raw.status == RAW_ABORTED && pushStarted) {
    fwupdate::abort();
    otaScreenFail();
    pushStarted = false;
  }
}

static void handleFirmwarePush() {
  bool seen = pushSeen;
  pushSeen = false;  // the next request starts clean, even if it carries no body
  if (!seen) return server.send(400, "text/plain", "No firmware in the request.\n");
  if (pushUnauthorized) return server.requestAuthentication(BASIC_AUTH, "yOS", "Wrong Update Password\n");
  if (pushStarted && pushError.isEmpty()) {
    Serial.printf("[web] STATS push received %d B in %lu ms\n", server.clientContentLength(),
                  (unsigned long)(millis() - pushT0));
    if (fwupdate::finish()) {
      server.send(200, "text/plain", "Installed yOS " + fwupdate::imageVersion() + ". Restarting.\n");
      otaScreenRestarting();
      delay(500);
      ESP.restart();
    }
  }
  if (pushStarted) {
    if (pushError.isEmpty()) pushError = fwupdate::error();
    fwupdate::abort();
    otaScreenFail();
    pushStarted = false;
  }
  server.send(409, "text/plain", pushError + "\n");
}

void webStart() {
#ifdef YOS_DEV_TOOLS
  server.on("/screenshot.bmp", handleScreenshot);
  server.on("/api/touch", handleTouch);
  // Backlight sweep: ?freq=Hz&duty=0-100 drives the PWM raw; freq=0 hands it back to the OS.
  // ?freq=Hz&save=1 makes Hz the OS frequency (as Settings › Display does, without the 10 s trial),
  // optionally with &level=10-255 for the brightness, so the real curve can be checked at that frequency.
  server.on("/api/backlight", [] {
    static lv_obj_t *note = nullptr;
    uint32_t f = server.arg("freq").toInt();
    float d = server.arg("duty").toFloat();
    if (server.arg("save") == "1") {
      if (f) prefs::setBacklightFreq(f);
      if (server.hasArg("level")) prefs::setBrightness(constrain(server.arg("level").toInt(), 10, 255));
      f = 0;  // back to OS control at the saved frequency
    }
    hal::lock();
    if (!note) {
      note = lv_label_create(lv_layer_top());
      lv_obj_set_style_text_font(note, theme::title, 0);
      lv_obj_set_style_text_color(note, lv_color_white(), 0);
      lv_obj_set_style_bg_color(note, lv_color_black(), 0);
      lv_obj_set_style_bg_opa(note, LV_OPA_COVER, 0);
      lv_obj_set_style_pad_all(note, 14, 0);
      lv_obj_set_style_radius(note, 12, 0);
      lv_obj_align(note, LV_ALIGN_CENTER, 0, 0);
    }
    if (f == 0) { lv_obj_del(note); note = nullptr; }
    else {
      if (server.hasArg("label")) lv_label_set_text_fmt(note, "Test %s", server.arg("label").c_str());
      else lv_label_set_text_fmt(note, "%lu Hz  %.0f%%", (unsigned long)f, d);
      lv_obj_align(note, LV_ALIGN_CENTER, 0, 0);
    }
    hal::unlock();
    hal::testBacklight(f, d);
    char buf[64];
    snprintf(buf, sizeof(buf), "ok: os freq %lu Hz, level %u", (unsigned long)prefs::backlightFreq(),
             prefs::brightness());
    server.send(200, "text/plain", buf);
  });
  server.on("/api/perf", [] {
    hal::Perf p = hal::takePerf();
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"frames\":%lu,\"window_ms\":%lu,\"fps\":%.1f,\"avg_ms\":%lu,\"max_ms\":%lu,\"flush_us\":%lu}",
             (unsigned long)p.frames, (unsigned long)p.windowMs, p.windowMs ? p.frames * 1000.0f / p.windowMs : 0.0f,
             (unsigned long)p.avgMs, (unsigned long)p.maxMs, (unsigned long)p.flushUs);
    server.send(200, "application/json", buf);
  });
  server.on("/api/appearance", [] {
    prefs::setAppearance(Appearance(server.arg("mode").toInt()));
    hal::lock();
    os::refreshAppearance();
    hal::unlock();
    server.send(200, "text/plain", "ok");
  });
#endif
  server.on("/sd", [] { server.send_P(200, "text/html", SD_PAGE); });
  server.on("/api/sd", HTTP_GET, handleSdList);
  server.on("/sd/download", HTTP_GET, handleSdDownload);
  server.on("/api/sd/delete", HTTP_POST, handleSdDelete);
  server.on("/api/sd/format", HTTP_POST, [] { startJob(JOB_FORMAT); });
  server.on("/api/sd/clear", HTTP_POST, [] { startJob(JOB_CLEAR_LOGS); });
  server.on("/apps", HTTP_GET, [] { server.send_P(200, "text/html", APPS_PAGE); });
  server.on("/api/apps", HTTP_GET, handleAppsList);
  server.on("/api/apps/install", HTTP_POST, handleAppInstall, handleAppUpload);
  server.on("/api/apps/remove", HTTP_POST, handleAppRemove);
  server.on("/wallpapers", HTTP_GET, [] { server.send_P(200, "text/html", WALLPAPERS_PAGE); });
  server.on("/api/wallpapers", HTTP_GET, handleWallpaperList);
  server.on("/api/wallpapers/upload", HTTP_POST, handleWallpaperUploadDone, handleWallpaperUpload);
  server.on("/api/wallpapers/remove", HTTP_POST, handleWallpaperRemove);
  server.on("/", [] { server.send_P(200, "text/html", PAGE); });
  server.on("/api/status", handleStatus);
  server.on("/api/update", HTTP_POST, handleFirmwarePush, handleFirmwareChunk);
  server.begin();
}

void webLoop() { server.handleClient(); }
