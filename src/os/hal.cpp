#include "hal.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp32s3/rom/cache.h>
#include <esp_async_memcpy.h>
#include <lvgl.h>
#include "services/touch.h"
#include "os/prefs.h"

// ST7701 configured over 3-wire SPI, pixels over 16-bit RGB.
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

static Arduino_DataBus *bus = new Arduino_SWSPI(
    GFX_NOT_DEFINED /* DC */, 39 /* CS */, 48 /* SCK */, 47 /* MOSI */, GFX_NOT_DEFINED /* MISO */);
static Arduino_ESP32RGBPanel *panel = new Arduino_ESP32RGBPanel(
    18 /* DE */, 17 /* VSYNC */, 16 /* HSYNC */, 21 /* PCLK */,
    11 /* R0 */, 12 /* R1 */, 13 /* R2 */, 14 /* R3 */, 0 /* R4 */,
    8 /* G0 */, 20 /* G1 */, 3 /* G2 */, 46 /* G3 */, 9 /* G4 */, 10 /* G5 */,
    4 /* B0 */, 5 /* B1 */, 6 /* B2 */, 7 /* B3 */, 15 /* B4 */,
    1 /* hsync_polarity */, 10 /* hsync_front_porch */, 8 /* hsync_pulse_width */, 50 /* hsync_back_porch */,
    1 /* vsync_polarity */, 10 /* vsync_front_porch */, 8 /* vsync_pulse_width */, 20 /* vsync_back_porch */,
    0 /* pclk_active_neg */, 12000000 /* pclk 12 MHz = 42 Hz. 16 MHz made the panel starve for PSRAM
                                          bandwidth during animations: rows tore and shifted sideways. */);
static Arduino_RGB_Display *lcd = new Arduino_RGB_Display(
    W, H, panel, 0 /* rotation */, true /* auto_flush */,
    bus, GFX_NOT_DEFINED /* RST */, st7701_type9_init_operations, sizeof(st7701_type9_init_operations));

static volatile uint32_t perfFrames = 0, perfSumMs = 0, perfMaxMs = 0, perfStart = 0, perfFlushUs = 0;

// LVGL reports each finished refresh: how long it took and how many pixels it redrew.
static const int FPS_RING = 64;
static uint32_t fpsStamps[FPS_RING];
static uint32_t fpsHead = 0;

static void monitor(lv_disp_drv_t *, uint32_t ms, uint32_t px) {
  fpsStamps[fpsHead++ % FPS_RING] = millis();
  perfFrames++;
  perfSumMs += ms;
  if (ms > perfMaxMs) perfMaxMs = ms;
}

static lv_disp_draw_buf_t drawBuf;
static lv_disp_drv_t dispDrv;
static lv_indev_drv_t indevDrv;

// LVGL renders into two internal-RAM buffers. Copying a finished buffer into the panel's PSRAM
// framebuffer happens on core 0 while LVGL already renders the next buffer on core 1.
struct FlushJob {
  lv_disp_drv_t *drv;
  lv_area_t area;
  lv_color_t *px;
};
static QueueHandle_t flushQueue;

// Full-width bands go to PSRAM by GDMA, bypassing the cache and the CPU. A row is 960 bytes,
// a multiple of the 64-byte PSRAM DMA alignment, so any band of whole rows qualifies.
static const size_t DMA_ALIGN = 64;
static const bool USE_DMA_COPY = false;
static async_memcpy_t dma = nullptr;
static SemaphoreHandle_t dmaDone;

static IRAM_ATTR bool onDmaDone(async_memcpy_t, async_memcpy_event_t *, void *) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(dmaDone, &woken);
  return woken == pdTRUE;
}

static void copyTask(void *) {
  FlushJob job;
  for (;;) {
    xQueueReceive(flushQueue, &job, portMAX_DELAY);
    uint32_t t0 = micros();
    uint16_t *fb = lcd->getFramebuffer();
    const lv_area_t &a = job.area;
    int w = a.x2 - a.x1 + 1, rows = a.y2 - a.y1 + 1;
    uint16_t *dst = fb + a.y1 * W;
    size_t bytes = rows * W * sizeof(uint16_t);
    // GDMA is faster, but its bursts starve the panel's own PSRAM reads (visible tearing), so the
    // CPU copies instead. Kept switchable for a future IDF with LCD bounce buffers.
    bool viaDma = USE_DMA_COPY && dma && w == W && ((uint32_t)dst % DMA_ALIGN) == 0 && bytes % DMA_ALIGN == 0 &&
                  esp_async_memcpy(dma, dst, job.px, bytes, onDmaDone, nullptr) == ESP_OK;
    if (viaDma) {
      xSemaphoreTake(dmaDone, portMAX_DELAY);
    } else {
      // CPU path. (With DMA enabled, cached lines would have to be invalidated first; with DMA off the
      // cache is the only writer and must not be invalidated, or unflushed pixels are lost.)
      if (USE_DMA_COPY) Cache_Invalidate_Addr((uint32_t)dst, bytes);
      for (int y = 0; y < rows; y++)
        memcpy(dst + y * W + a.x1, job.px + y * w, w * sizeof(uint16_t));
      Cache_WriteBack_Addr((uint32_t)dst, bytes);
    }
    perfFlushUs += micros() - t0;
    lv_disp_flush_ready(job.drv);
  }
}

static void flush(lv_disp_drv_t *drv, const lv_area_t *a, lv_color_t *px) {
  FlushJob job{drv, *a, px};
  xQueueSend(flushQueue, &job, portMAX_DELAY);
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
  ledcSetup(BL_CHANNEL, freq, blBits);
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
  if (!blTesting && blDuty(blLevel) != blDuty(before)) ledcWrite(BL_CHANNEL, blDuty(blLevel));
}

static SemaphoreHandle_t uiLock = xSemaphoreCreateRecursiveMutex();

void hal::backlightOnNow() {
  blTarget = prefs::brightness();
  blLevel = blTarget;
  lastActivity = millis();
  ledcWrite(BL_CHANNEL, blDuty(blLevel));
}

void hal::setRotation(uint8_t r) {
  lv_disp_set_rotation(lv_disp_get_default(), (lv_disp_rot_t)(r & 3));
}

namespace hal {
void lock() { xSemaphoreTakeRecursive(uiLock, portMAX_DELAY); }
void unlock() { xSemaphoreGiveRecursive(uiLock); }

void begin() {
  lcd->begin();
  memset(lcd->getFramebuffer(), 0, W * H * sizeof(uint16_t));
  Cache_WriteBack_Addr((uint32_t)lcd->getFramebuffer(), W * H * sizeof(uint16_t));
  blSetFreq(prefs::backlightFreq());
  ledcAttachPin(PIN_BL, BL_CHANNEL);
  touchStart();

  flushQueue = xQueueCreate(2, sizeof(FlushJob));
  dmaDone = xSemaphoreCreateBinary();
  async_memcpy_config_t cfg = ASYNC_MEMCPY_DEFAULT_CONFIG();
  cfg.backlog = 32;
  cfg.psram_trans_align = DMA_ALIGN;
  cfg.sram_trans_align = 4;
  if (esp_async_memcpy_install(&cfg, &dma) != ESP_OK) {
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
  dispDrv.sw_rotate = 1;  // the panel can't rotate itself; LVGL rotates each flushed area
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
uint16_t *framebuffer() { return lcd->getFramebuffer(); }
void testBacklight(uint32_t freq, float dutyPct) {
  if (freq == 0) {
    blTesting = false;
    applyBacklightFreq();
    return;
  }
  blTesting = true;
  lastActivity = millis();
  blSetFreq(freq);
  ledcWrite(BL_CHANNEL, uint32_t(constrain(dutyPct, 0.0f, 100.0f) / 100.0f * blMax + 0.5f));
}
void applyBacklightFreq() {
  blSetFreq(prefs::backlightFreq());
  if (!blTesting) ledcWrite(BL_CHANNEL, blDuty(blLevel));
}
Perf takePerf() {
  Perf p{perfFrames, perfFrames ? perfSumMs / perfFrames : 0, perfMaxMs, millis() - perfStart,
         perfFrames ? perfFlushUs / perfFrames : 0};
  perfFrames = perfSumMs = perfMaxMs = perfFlushUs = 0;
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
