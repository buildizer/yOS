#include "hal.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_async_memcpy.h>
#include <esp_cache.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <lvgl.h>
#include "services/touch.h"
#include "os/prefs.h"

// ST7701 configured over 3-wire SPI (Arduino_GFX's software SPI and its init table),
// pixels over 16-bit RGB through ESP-IDF's esp_lcd RGB driver.
#define PIN_BL 38
#define BL_CHANNEL 0
// Backlight PWM. The frequency is a setting (prefs::backlightFreq) because units of this board differ:
// on one, 100 Hz-5 kHz at 40 % duty left the screen dark (its driver doesn't follow a slow carrier) and
// 20-40 kHz dimmed properly; on others 20 kHz goes black below 100 % and ~100 Hz works.
// 10 bits as long as freq x 1024 fits the 80 MHz APB clock (up to ~78 kHz), fewer above that.
static const uint8_t BL_BITS = 10;
static uint8_t blBits = BL_BITS;
static uint32_t blMax = (1 << BL_BITS) - 1;  // duty for 100 %: the core turns it into a steady high
static const int W = 480, H = 480;
static const size_t FB_BYTES = W * H * sizeof(uint16_t);

// 12 MHz = 42 Hz. The panel now reads through internal-RAM bounce buffers, so PSRAM bandwidth no longer
// tears rows, but each refill is an interrupt (every ~0.46 ms with 10-line buffers): keep the clock modest.
static const uint32_t PCLK_HZ = 12000000;
static const int HSYNC_PULSE = 8, HSYNC_BACK = 50, HSYNC_FRONT = 10;
static const int VSYNC_PULSE = 8, VSYNC_BACK = 20, VSYNC_FRONT = 10;
static const size_t BOUNCE_PX = W * 10;  // two of these in internal RAM (19 KB); must divide W x H
// The panel's refill interrupt lives on the core that creates the panel. It shares a core badly with the
// Bluetooth controller's interrupt (interrupt watchdog resets), so it goes to whichever core that isn't on.
#if defined(CONFIG_BT_CTRL_PINNED_TO_CORE) && CONFIG_BT_CTRL_PINNED_TO_CORE == 1
static const BaseType_t PANEL_CORE = 0;
#else
static const BaseType_t PANEL_CORE = 1;
#endif

static Arduino_DataBus *bus = new Arduino_SWSPI(
    GFX_NOT_DEFINED /* DC */, 39 /* CS */, 48 /* SCK */, 47 /* MOSI */, GFX_NOT_DEFINED /* MISO */);

static volatile uint32_t perfFrames = 0, perfSumMs = 0, perfMaxMs = 0, perfStart = 0, perfFlushUs = 0;

// LVGL reports each finished refresh: how long it took and how many pixels it redrew.
static const int FPS_RING = 64;
static uint32_t fpsStamps[FPS_RING];
static uint32_t fpsHead = 0;

static void monitor(lv_disp_drv_t *, uint32_t ms, uint32_t px) {
  fpsStamps[fpsHead++ % FPS_RING] = millis();
  perfFrames = perfFrames + 1;
  perfSumMs = perfSumMs + ms;
  if (ms > perfMaxMs) perfMaxMs = ms;
}

static lv_disp_draw_buf_t drawBuf;
static lv_disp_drv_t dispDrv;
static lv_indev_drv_t indevDrv;

// ---- panel and frame buffers ----
//
// Three PSRAM frame buffers. LVGL renders strips into internal RAM; each strip goes into a buffer the panel
// neither shows nor is about to show, and a finished frame is shown from the start of the next scan. The panel
// never shows a half-drawn frame (the tearing of the single-buffer driver while scrolling and swiping).
// A buffer that sat out some frames is first brought up to date by copying just the areas it missed from
// the newest frame, skipping what the new frame redraws anyway.
static esp_lcd_panel_handle_t panel = nullptr;
static const int MAX_FBS = 3;
static uint16_t *fbs[MAX_FBS] = {};
static int fbCount = 0;
static bool bounced = false;            // false: the LCD DMA reads PSRAM itself, CPU writes need a write-back
static volatile int fbShown = 0;        // scanned now (updated at the end of each scan)
static volatile int fbRequested = 0;    // newest finished frame, shown from the next scan
static int fbLatest = 0;                // newest finished frame (copy task side)
static int fbTarget = -1;               // the frame being drawn goes here; -1 between frames

static const int MAX_STALE = 16;
struct AreaList {
  lv_area_t a[MAX_STALE];
  int n = 0;
  // Full: everything collapses into one bounding box, which is always correct, just copies more.
  void add(const lv_area_t &r) {
    if (n < MAX_STALE) {
      a[n++] = r;
      return;
    }
    for (int i = 1; i < n; i++) _lv_area_join(&a[0], &a[0], &a[i]);
    _lv_area_join(&a[0], &a[0], &r);
    n = 1;
  }
};
static AreaList stale[MAX_FBS];  // per buffer: areas changed by frames drawn into the other buffers
static AreaList frameAreas;      // what the frame being drawn changes

// What LVGL is about to redraw, captured by the LVGL task when a frame's first strip is flushed (panel
// coordinates). A ring: the copy task may still be on the previous frame when the next one starts.
struct FrameInv {
  lv_area_t a[LV_INV_BUF_SIZE];
  int n;
};
static FrameInv frameInv[3];
static const FrameInv noInv = {};  // nothing known: every stale area is copied
static int frameInvNext = 0;

// The end of a scan: the driver has moved on to the requested buffer.
static IRAM_ATTR bool onFrameDone(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t *, void *) {
  fbShown = fbRequested;
  return false;
}

// Logical (what LVGL draws, rotated) to panel coordinates; the same mapping LVGL's own software rotation
// and its touch input use.
static lv_area_t toPanel(const lv_area_t &l, lv_disp_rot_t rot) {
  switch (rot) {
    case LV_DISP_ROT_90: return {(lv_coord_t)l.y1, (lv_coord_t)(H - 1 - l.x2), (lv_coord_t)l.y2, (lv_coord_t)(H - 1 - l.x1)};
    case LV_DISP_ROT_180: return {(lv_coord_t)(W - 1 - l.x2), (lv_coord_t)(H - 1 - l.y2), (lv_coord_t)(W - 1 - l.x1), (lv_coord_t)(H - 1 - l.y1)};
    case LV_DISP_ROT_270: return {(lv_coord_t)(W - 1 - l.y2), (lv_coord_t)l.x1, (lv_coord_t)(W - 1 - l.y1), (lv_coord_t)l.x2};
    default: return l;
  }
}

// After the CPU wrote rows y1..y2 (only needed when the LCD DMA reads the buffer straight from PSRAM).
static void writeBack(uint16_t *fb, int y1, int y2) {
  if (bounced) return;
  esp_cache_msync(fb + y1 * W, (y2 - y1 + 1) * W * sizeof(uint16_t), ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

static void copyArea(uint16_t *dst, const uint16_t *src, const lv_area_t &a) {
  size_t row = (a.x2 - a.x1 + 1) * sizeof(uint16_t);
  for (int y = a.y1; y <= a.y2; y++) memcpy(dst + y * W + a.x1, src + y * W + a.x1, row);
  writeBack(dst, a.y1, a.y2);
}

// First strip of a frame: pick the buffer and bring it up to date.
static void beginFrame(const FrameInv *inv) {
  frameAreas.n = 0;
  if (fbCount < 2) {
    fbTarget = 0;
    return;
  }
  int shown = fbShown, requested = fbRequested;
  fbTarget = 0;
  while (fbTarget == shown || fbTarget == requested) fbTarget++;
  AreaList &st = stale[fbTarget];
  for (int i = 0; i < st.n; i++) {
    bool redrawn = false;
    for (int k = 0; k < inv->n && !redrawn; k++) redrawn = _lv_area_is_in(&st.a[i], &inv->a[k], 0);
    if (!redrawn) copyArea(fbs[fbTarget], fbs[fbLatest], st.a[i]);
  }
  st.n = 0;
}

// Last strip: show the frame from the next scan. Switch first, then note it: the end-of-scan interrupt
// may run in between, and must never believe a buffer is free while the panel is about to read it.
static void endFrame() {
  if (fbCount >= 2) {
    esp_lcd_panel_draw_bitmap(panel, 0, 0, W, H, fbs[fbTarget]);
    fbRequested = fbTarget;
    for (int b = 0; b < fbCount; b++)
      if (b != fbTarget)
        for (int i = 0; i < frameAreas.n; i++) stale[b].add(frameAreas.a[i]);
  }
  fbLatest = fbTarget;
  fbTarget = -1;
}

// LVGL renders into two internal-RAM buffers. Copying a finished strip into the frame buffer happens on
// core 0 while LVGL already renders the next strip on core 1.
struct FlushJob {
  lv_disp_drv_t *drv;
  lv_area_t area;  // logical
  lv_color_t *px;
  lv_disp_rot_t rot;
  bool last;
  const FrameInv *first;  // set on a frame's first strip
};
static QueueHandle_t flushQueue;

// Full-width strips (unrotated) go to PSRAM by GDMA, bypassing the cache and the CPU. Without bounce
// buffers its bursts starved the panel's own PSRAM reads; now the panel reads internal RAM.
// A row is 960 bytes, a multiple of the 64-byte PSRAM DMA alignment, so any band of whole rows qualifies.
static const size_t DMA_ALIGN = 64;
static const bool USE_DMA_COPY = true;
static async_memcpy_handle_t dma = nullptr;
static SemaphoreHandle_t dmaDone;

static IRAM_ATTR bool onDmaDone(async_memcpy_handle_t, async_memcpy_event_t *, void *) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(dmaDone, &woken);
  return woken == pdTRUE;
}

static bool dmaCopy(uint16_t *dst, const lv_color_t *src, size_t bytes) {
  if (!dma || !bounced || ((uint32_t)dst % DMA_ALIGN) || bytes % DMA_ALIGN) return false;
  // Earlier CPU writes still in the cache must reach PSRAM before the DMA, or a later eviction would
  // overwrite its pixels; afterwards the cache drops its now stale copies of those lines.
  esp_cache_msync(dst, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  if (esp_async_memcpy(dma, dst, (void *)src, bytes, onDmaDone, nullptr) != ESP_OK) return false;
  xSemaphoreTake(dmaDone, portMAX_DELAY);
  esp_cache_msync(dst, bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
  return true;
}

// Writes a logical strip into the frame buffer, rotating it on the way when the screen is turned.
static void putStrip(uint16_t *fb, const FlushJob &job, const lv_area_t &p) {
  const lv_area_t &a = job.area;
  const uint16_t *s = (const uint16_t *)job.px;
  int w = a.x2 - a.x1 + 1, h = a.y2 - a.y1 + 1;
  switch (job.rot) {
    case LV_DISP_ROT_NONE:
      if (w == W && dmaCopy(fb + a.y1 * W, job.px, h * W * sizeof(uint16_t))) return;
      for (int y = 0; y < h; y++) memcpy(fb + (a.y1 + y) * W + a.x1, s + y * w, w * sizeof(uint16_t));
      break;
    case LV_DISP_ROT_180:
      for (int y = 0; y < h; y++) {
        uint16_t *d = fb + (H - 1 - (a.y1 + y)) * W + (W - 1 - a.x1);
        const uint16_t *r = s + y * w;
        for (int x = 0; x < w; x++) *d-- = r[x];
      }
      break;
    case LV_DISP_ROT_90:  // logical column x becomes panel row H-1-x, left to right = logical top to bottom
      for (int x = 0; x < w; x++) {
        uint16_t *d = fb + (H - 1 - (a.x1 + x)) * W + p.x1;
        for (int y = 0; y < h; y++) d[y] = s[y * w + x];
      }
      break;
    case LV_DISP_ROT_270:  // logical column x becomes panel row x, left to right = logical bottom to top
      for (int x = 0; x < w; x++) {
        uint16_t *d = fb + (a.x1 + x) * W + p.x1;
        for (int y = 0; y < h; y++) d[y] = s[(h - 1 - y) * w + x];
      }
      break;
  }
  writeBack(fb, p.y1, p.y2);
}

static void copyTask(void *) {
  FlushJob job;
  for (;;) {
    xQueueReceive(flushQueue, &job, portMAX_DELAY);
    uint32_t t0 = micros();
    if (job.first || fbTarget < 0) beginFrame(job.first ? job.first : &noInv);
    lv_area_t p = toPanel(job.area, job.rot);
    putStrip(fbs[fbTarget], job, p);
    frameAreas.add(p);
    if (job.last) endFrame();
    perfFlushUs = perfFlushUs + (micros() - t0);
    lv_disp_flush_ready(job.drv);
  }
}

static void flush(lv_disp_drv_t *drv, const lv_area_t *a, lv_color_t *px) {
  static bool inFrame = false;
  lv_disp_rot_t rot = (lv_disp_rot_t)drv->rotated;
  FlushJob job{drv, *a, px, rot, lv_disp_flush_is_last(drv), nullptr};
  if (!inFrame) {
    // A frame's first strip: note what this refresh redraws (LVGL's dirty areas, panel coordinates).
    FrameInv &fi = frameInv[frameInvNext];
    frameInvNext = (frameInvNext + 1) % 3;
    fi.n = 0;
    lv_disp_t *disp = _lv_refr_get_disp_refreshing();
    for (int i = 0; disp && i < disp->inv_p; i++)
      if (!disp->inv_area_joined[i]) fi.a[fi.n++] = toPanel(disp->inv_areas[i], rot);
    job.first = &fi;
  }
  inFrame = !job.last;
  xQueueSend(flushQueue, &job, portMAX_DELAY);
}

// Creates the RGB panel; runs on PANEL_CORE so the refill interrupt lands there.
static void panelTask(void *done) {
  esp_lcd_rgb_panel_config_t cfg = {};
  cfg.clk_src = LCD_CLK_SRC_DEFAULT;
  cfg.timings.pclk_hz = PCLK_HZ;
  cfg.timings.h_res = W;
  cfg.timings.v_res = H;
  cfg.timings.hsync_pulse_width = HSYNC_PULSE;
  cfg.timings.hsync_back_porch = HSYNC_BACK;
  cfg.timings.hsync_front_porch = HSYNC_FRONT;
  cfg.timings.vsync_pulse_width = VSYNC_PULSE;
  cfg.timings.vsync_back_porch = VSYNC_BACK;
  cfg.timings.vsync_front_porch = VSYNC_FRONT;
  cfg.data_width = 16;
  cfg.bits_per_pixel = 16;
  cfg.dma_burst_size = 64;
  cfg.hsync_gpio_num = 16;
  cfg.vsync_gpio_num = 17;
  cfg.de_gpio_num = 18;
  cfg.pclk_gpio_num = 21;
  cfg.disp_gpio_num = GPIO_NUM_NC;
  // D0-D4 blue, D5-D10 green, D11-D15 red
  const int data[16] = {4, 5, 6, 7, 15, 8, 20, 3, 46, 9, 10, 11, 12, 13, 14, 0};
  memcpy(cfg.data_gpio_nums, data, sizeof(data));
  cfg.flags.fb_in_psram = 1;
  // Three buffers with bounce buffers; with too little memory fewer, and at worst the old single buffer.
  for (int n = MAX_FBS; n >= 1 && !panel; n--) {
    cfg.num_fbs = n;
    cfg.bounce_buffer_size_px = n > 1 ? BOUNCE_PX : 0;
    if (esp_lcd_new_rgb_panel(&cfg, &panel) != ESP_OK) {
      panel = nullptr;
      continue;
    }
    fbCount = n;
    bounced = n > 1;
  }
  if (panel) {
    esp_lcd_rgb_panel_event_callbacks_t cbs = {};
    cbs.on_frame_buf_complete = onFrameDone;
    if (bounced) esp_lcd_rgb_panel_register_event_callbacks(panel, &cbs, nullptr);
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    void *b[MAX_FBS] = {};
    if (fbCount == 3) esp_lcd_rgb_panel_get_frame_buffer(panel, 3, &b[0], &b[1], &b[2]);
    else if (fbCount == 2) esp_lcd_rgb_panel_get_frame_buffer(panel, 2, &b[0], &b[1]);
    else esp_lcd_rgb_panel_get_frame_buffer(panel, 1, &b[0]);
    for (int i = 0; i < fbCount; i++) fbs[i] = (uint16_t *)b[i];
  }
  xSemaphoreGive((SemaphoreHandle_t)done);
  vTaskDelete(nullptr);
}

static bool panelBegin() {
  // The ST7701 itself: software reset, then the vendor's register table (Arduino_GFX "type 9").
  bus->begin();
  bus->sendCommand(0x01);
  delay(120);
  bus->batchOperation(st7701_type9_init_operations, sizeof(st7701_type9_init_operations));

  SemaphoreHandle_t done = xSemaphoreCreateBinary();
  xTaskCreatePinnedToCore(panelTask, "lcdinit", 4096, done, 5, nullptr, PANEL_CORE);
  xSemaphoreTake(done, portMAX_DELAY);
  vSemaphoreDelete(done);
  if (!panel) return false;
  for (int i = 0; i < fbCount; i++) {
    memset(fbs[i], 0, FB_BYTES);
    esp_cache_msync(fbs[i], FB_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  }
  Serial.printf("Display: %d frame buffer(s), %s, refill interrupt on core %d\n", fbCount,
                bounced ? "bounce buffers" : "no bounce buffers", (int)PANEL_CORE);
  return true;
}

// ---- touch + system gestures ----

static const int EDGE = 28, GESTURE_MIN = 70;
static volatile SystemGesture pendingGesture = GESTURE_NONE;
static uint32_t lastActivity = 0;
static uint32_t touchDownAt = 0;    // start of the current touch, 0 when up (backlight recovery hold)
static bool swallowTouch = false;  // the touch that woke the screen does nothing else

static struct {
  bool active;
  int x1, y1, x2, y2;
  uint32_t start, ms;
} injected;

static bool injectedRead(int16_t &x, int16_t &y) {
  uint32_t t = millis() - injected.start;
  if (t > injected.ms + 40) {  // hold a moment at the end, then release
    injected.active = false;
    return false;
  }
  float f = injected.ms ? min(1.0f, t / float(injected.ms)) : 1;
  x = injected.x1 + (injected.x2 - injected.x1) * f;
  y = injected.y1 + (injected.y2 - injected.y1) * f;
  return true;
}

// Panel coordinates to what the person sees, for the current rotation (matches lv_indev's mapping).
static void toLogical(int16_t &x, int16_t &y) {
  switch (dispDrv.rotated) {
    case LV_DISP_ROT_90: { int16_t t = x; x = y; y = W - 1 - t; break; }
    case LV_DISP_ROT_180: x = W - 1 - x; y = H - 1 - y; break;
    case LV_DISP_ROT_270: { int16_t t = x; x = H - 1 - y; y = t; break; }
    default: break;
  }
}

static void readTouch(lv_indev_drv_t *, lv_indev_data_t *data) {
  static bool wasDown = false;
  static int16_t sx, sy, lx, ly;
  int16_t x, y;
  bool down = injected.active ? injectedRead(x, y) : touchRead(x, y);
  if (down) {
    if (!wasDown) {
      sx = x; sy = y;
      touchDownAt = millis() | 1;
      if (hal::screenAsleep()) swallowTouch = true;
      hal::wake();
    }
    lx = x; ly = y;
    lastActivity = millis();
  } else if (wasDown) {
    touchDownAt = 0;
    // Edge swipes, judged on release: up from the bottom edge = home, right from the left edge = back.
    // Judge edge swipes in screen (rotated) coordinates; LVGL does the same for its own input.
    int16_t ax = sx, ay = sy, bx = lx, by = ly;
    toLogical(ax, ay);
    toLogical(bx, by);
    if (ay > H - EDGE && ay - by > GESTURE_MIN) pendingGesture = GESTURE_HOME;
    else if (ax < EDGE && bx - ax > GESTURE_MIN && abs(by - ay) < bx - ax) pendingGesture = GESTURE_BACK;
  }
  wasDown = down;
  if (swallowTouch) {
    data->state = LV_INDEV_STATE_REL;
    if (!down) swallowTouch = false;
    return;
  }
  data->point.x = lx;
  data->point.y = ly;
  data->state = down ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
}

// ---- backlight ----

static const uint32_t OFF_AFTER_MS = 30 * 60 * 1000, FADE_MS = 300;
static const uint32_t RECOVER_HOLD_MS = 10000;  // touch and hold this long: back to full brightness
static float blLevel = 0;
static bool blTesting = false;
static uint8_t blTarget = 0;
static uint32_t lastBlStep = 0;
static bool alarmOn = false;
static bool forcedOff = false;  // sleepNow(): dark until a touch or wake()

// Brightness levels (0..255, as stored in prefs and used for fading) are perceptually linear:
// duty = floor + (1 - floor) * (level/255)^1.8. Measured on this panel: below ~12-16 % duty the
// backlight driver switches off instead of dimming, so the lowest setting maps to 16 %. Level 0 is off.
static uint32_t blDuty(float level) {
  if (level <= 0.5f) return 0;
  const float floorDuty = 0.16f;
  float d = floorDuty + (1.0f - floorDuty) * powf(min(level, 255.0f) / 255.0f, 1.8f);
  return uint32_t(d * blMax + 0.5f);
}

static void blSetFreq(uint32_t freq) {
  blBits = BL_BITS;
  while (blBits > 4 && (uint64_t)freq << blBits > 80000000ULL) blBits--;
  blMax = (1 << blBits) - 1;
  // Arduino core 3 addresses LEDC by pin: the first call attaches, later ones only retime it.
  static bool attached = false;
  if (!attached) attached = ledcAttachChannel(PIN_BL, freq, blBits, BL_CHANNEL);
  else ledcChangeFrequency(PIN_BL, freq, blBits);
}

static void updateBacklight() {
  // A brightness/frequency pair this panel can't show leaves it black with no way to see the slider:
  // a long hold anywhere brings back 100 %, which lights at any frequency.
  if (touchDownAt && millis() - touchDownAt > RECOVER_HOLD_MS && prefs::brightness() < 255) {
    prefs::setBrightness(255);
    touchDownAt = 0;
  }
  uint32_t idle = millis() - lastActivity;
  uint8_t full = prefs::brightness();
  uint32_t dimAfter = prefs::dimAfterMin() * 60000UL;
  if (forcedOff) blTarget = 0;
  else if (alarmOn || dimAfter == 0) blTarget = full;
  else if (idle > OFF_AFTER_MS) blTarget = 0;
  else if (idle > dimAfter) blTarget = full * 2 / 5;  // dimmed, still readable across the room
  else blTarget = full;

  uint32_t dt = millis() - lastBlStep;
  lastBlStep = millis();
  float step = 255.0f * dt / FADE_MS;
  float before = blLevel;
  blLevel = blTarget > blLevel ? min<float>(blTarget, blLevel + step) : max<float>(blTarget, blLevel - step);
  if (!blTesting && blDuty(blLevel) != blDuty(before)) ledcWrite(PIN_BL, blDuty(blLevel));
}

static SemaphoreHandle_t uiLock = xSemaphoreCreateRecursiveMutex();

void hal::backlightOnNow() {
  blTarget = prefs::brightness();
  blLevel = blTarget;
  lastActivity = millis();
  ledcWrite(PIN_BL, blDuty(blLevel));
}

void hal::setRotation(uint8_t r) {
  lv_disp_set_rotation(lv_disp_get_default(), (lv_disp_rot_t)(r & 3));
}

namespace hal {
void lock() { xSemaphoreTakeRecursive(uiLock, portMAX_DELAY); }
void unlock() { xSemaphoreGiveRecursive(uiLock); }

void begin() {
  if (!panelBegin()) Serial.println("Display: RGB panel failed to start");
  blSetFreq(prefs::backlightFreq());
  touchStart();

  flushQueue = xQueueCreate(2, sizeof(FlushJob));
  dmaDone = xSemaphoreCreateBinary();
  async_memcpy_config_t cfg = ASYNC_MEMCPY_DEFAULT_CONFIG();
  cfg.backlog = 8;
  cfg.dma_burst_size = DMA_ALIGN;
  if (USE_DMA_COPY && esp_async_memcpy_install(&cfg, &dma) != ESP_OK) {
    Serial.println("GDMA copy unavailable, using CPU copy");
    dma = nullptr;
  }
  xTaskCreatePinnedToCore(copyTask, "lcdcopy", 3072, nullptr, 4, nullptr, 0);
  lv_init();
  const size_t bufPx = W * 30;  // two buffers, ~28 KB each: Wi-Fi needs the rest of internal RAM
  auto *b1 = (lv_color_t *)heap_caps_malloc(bufPx * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  auto *b2 = (lv_color_t *)heap_caps_malloc(bufPx * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  lv_disp_draw_buf_init(&drawBuf, b1, b2, bufPx);
  lv_disp_drv_init(&dispDrv);
  dispDrv.hor_res = W;
  dispDrv.ver_res = H;
  dispDrv.flush_cb = flush;
  dispDrv.monitor_cb = monitor;
  dispDrv.draw_buf = &drawBuf;
  dispDrv.sw_rotate = 0;  // rotation happens while copying into the frame buffer (copyTask)
  lv_disp_drv_register(&dispDrv);

  lv_indev_drv_init(&indevDrv);
  indevDrv.type = LV_INDEV_TYPE_POINTER;
  indevDrv.read_cb = readTouch;
  lv_indev_drv_register(&indevDrv);
  lastActivity = millis();
}

void loop() {
  lv_timer_handler();
  updateBacklight();
}

SystemGesture takeGesture() {
  SystemGesture g = pendingGesture;
  pendingGesture = GESTURE_NONE;
  return g;
}

void wake() {
  lastActivity = millis();
  forcedOff = false;
}
void sleepNow() { forcedOff = true; }
void setAlarm(bool on) {
  if (on && !alarmOn) wake();
  alarmOn = on;
}
bool screenAsleep() { return blTarget < prefs::brightness(); }
uint16_t *framebuffer() { return fbs[fbLatest]; }  // the newest finished frame
void testBacklight(uint32_t freq, float dutyPct) {
  if (freq == 0) {
    blTesting = false;
    applyBacklightFreq();
    return;
  }
  blTesting = true;
  lastActivity = millis();
  blSetFreq(freq);
  ledcWrite(PIN_BL, uint32_t(constrain(dutyPct, 0.0f, 100.0f) / 100.0f * blMax + 0.5f));
}
void applyBacklightFreq() {
  blSetFreq(prefs::backlightFreq());
  if (!blTesting) ledcWrite(PIN_BL, blDuty(blLevel));
}
Perf takePerf() {
  Perf p{perfFrames, perfFrames ? perfSumMs / perfFrames : 0, perfMaxMs, millis() - perfStart,
         perfFrames ? perfFlushUs / perfFrames : 0};
  perfFrames = 0;
  perfSumMs = 0;
  perfMaxMs = 0;
  perfFlushUs = 0;
  perfStart = millis();
  return p;
}
// Refreshes finished during the last second; unlike takePerf() this doesn't reset anything.
uint32_t currentFps() {
  uint32_t now = millis(), n = 0;
  for (int i = 0; i < FPS_RING; i++)
    if (fpsStamps[i] && now - fpsStamps[i] < 1000) n++;
  return n;
}
// 12 MHz pixel clock / ((480+10+8+50) x (480+10+8+20)) total clocks per frame.
float refreshHz() { return 12000000.0f / ((480 + 10 + 8 + 50) * (480 + 10 + 8 + 20)); }
void injectTouch(int x1, int y1, int x2, int y2, uint32_t ms) {
  injected = {true, x1, y1, x2, y2, millis(), ms};
}
}
