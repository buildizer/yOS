#include "services/storage.h"
#include <SD.h>
#include <SPI.h>
#include "ff.h"
#include "diskio_impl.h"
#include "sd_diskio.h"
#include <vector>

// microSD slot on the Guition ESP32-4848S040.
#define SD_CS 42
#define SD_SCK 48
#define SD_MOSI 47
#define SD_MISO 41

static const uint32_t SD_FREQ = 20000000, SD_FORMAT_FREQ = 10000000;

static const char *DIR = "/netwatch";
static const char *EVENTS_FILE = "/netwatch/events.csv";

static SPIClass sdSpi(HSPI);
static SemaphoreHandle_t sdLock;
static bool ready = false;

// Driver entry points from the Arduino SD library (sd_diskio.cpp).
DSTATUS ff_sd_initialize(uint8_t pdrv);
DSTATUS ff_sd_status(uint8_t pdrv);
DRESULT ff_sd_read(uint8_t pdrv, uint8_t *buffer, DWORD sector, UINT count);
DRESULT ff_sd_write(uint8_t pdrv, const uint8_t *buffer, DWORD sector, UINT count);
DRESULT ff_sd_ioctl(uint8_t pdrv, uint8_t cmd, void *buff);

// This card intermittently rejects multi-block writes (token error 0x33); a retry always gets through.
static DRESULT retryWrite(uint8_t pdrv, const uint8_t *buf, DWORD sector, UINT count) {
  for (int attempt = 1; attempt <= 5; attempt++) {
    if (ff_sd_write(pdrv, buf, sector, count) == RES_OK) return RES_OK;
    Serial.printf("[sd] write %lu+%u failed, attempt %d\n", (unsigned long)sector, count, attempt);
    vTaskDelay(pdMS_TO_TICKS(100 * attempt));
  }
  return RES_ERROR;
}

static DRESULT retryIoctl(uint8_t pdrv, uint8_t cmd, void *buff) {
  for (int attempt = 1; attempt <= 5; attempt++) {
    DRESULT r = ff_sd_ioctl(pdrv, cmd, buff);
    if (r == RES_OK || cmd != CTRL_SYNC) return r;
    Serial.printf("[sd] sync failed, attempt %d\n", attempt);
    vTaskDelay(pdMS_TO_TICKS(100 * attempt));
  }
  return RES_ERROR;
}

static const ff_diskio_impl_t retryingDriver = {ff_sd_initialize, ff_sd_status, ff_sd_read, retryWrite, retryIoctl};

static bool mount() {
  if (!SD.begin(SD_CS, sdSpi, SD_FREQ) || SD.cardType() == CARD_NONE) return false;
  // SD.begin registers the plain driver as FatFs drive 0; swap in the retrying one.
  FATFS *fs;
  DWORD freeClusters;
  if (f_getfree("0:", &freeClusters, &fs) == FR_OK) ff_diskio_register(0, &retryingDriver);
  else Serial.println("SD: card is not drive 0, retries disabled");
  if (!SD.exists(DIR)) SD.mkdir(DIR);
  return true;
}

bool storageStart() {
  sdLock = xSemaphoreCreateMutex();
  sdSpi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!mount()) {
    Serial.println("SD card not found");
    return false;
  }
  ready = true;
  Serial.printf("SD card: %llu MB, %llu MB used\n", SD.totalBytes() >> 20, SD.usedBytes() >> 20);
  return true;
}

bool storageReady() { return ready; }

uint64_t storageTotalBytes() {
  uint64_t v = 0;
  if (ready) storageWithLock([&] { v = SD.totalBytes(); });
  return v;
}

uint64_t storageUsedBytes() {
  uint64_t v = 0;
  if (ready) storageWithLock([&] { v = SD.usedBytes(); });
  return v;
}

void storageWithLock(std::function<void()> fn) {
  xSemaphoreTake(sdLock, portMAX_DELAY);
  fn();
  xSemaphoreGive(sdLock);
}

void storageAppendEvent(const Event &e) {
  if (!ready) return;
  storageWithLock([&] {
    bool isNew = !SD.exists(EVENTS_FILE);
    File f = SD.open(EVENTS_FILE, FILE_APPEND);
    if (!f) return;
    if (isNew) f.println("epoch,what,state,down_secs");
    f.printf("%lu,%s,%s,%lu\n", (unsigned long)e.at, e.what, e.up ? "UP" : "DOWN", (unsigned long)e.downSecs);
    f.close();
  });
}

int storageLoadEvents(Event *out, int max) {
  if (!ready) return 0;
  int count = 0;
  storageWithLock([&] {
    if (!SD.exists(EVENTS_FILE)) return;
    File f = SD.open(EVENTS_FILE, FILE_READ);
    if (!f) return;
    // Keep a ring of the last `max` lines, then hand them back newest first.
    Event *ring = (Event *)ps_malloc(sizeof(Event) * max);
    if (!ring) { f.close(); return; }
    int head = 0;
    while (f.available()) {
      String line = f.readStringUntil('\n');
      unsigned long at, down;
      char what[24], state[8];
      if (sscanf(line.c_str(), "%lu,%23[^,],%7[^,],%lu", &at, what, state, &down) != 4) continue;
      Event &e = ring[head];
      e.at = at;
      strlcpy(e.what, what, sizeof(e.what));
      e.up = strcmp(state, "UP") == 0;
      e.downSecs = down;
      head = (head + 1) % max;
      if (count < max) count++;
    }
    f.close();
    for (int i = 0; i < count; i++) out[i] = ring[(head - 1 - i + max) % max];
    free(ring);
  });
  return count;
}

void storageAppendMinute(time_t at, const float *avgMs, const int *lost, int numProbes) {
  if (!ready) return;
  struct tm tm;
  localtime_r(&at, &tm);
  char path[40], ts[20];
  strftime(path, sizeof(path), "/netwatch/%Y-%m-%d.csv", &tm);
  strftime(ts, sizeof(ts), "%H:%M", &tm);
  storageWithLock([&] {
    bool isNew = !SD.exists(path);
    File f = SD.open(path, FILE_APPEND);
    if (!f) return;
    if (isNew) {
      f.print("time");
      for (int i = 0; i < numProbes; i++) {
        const char *name = i < NUM_TARGETS ? TARGETS[i].name : "DNS";
        f.printf(",%s ms,%s lost", name, name);
      }
      f.println();
    }
    f.print(ts);
    for (int i = 0; i < numProbes; i++) {
      if (isnan(avgMs[i])) f.printf(",,%d", lost[i]);
      else f.printf(",%.1f,%d", avgMs[i], lost[i]);
    }
    f.println();
    f.close();
  });
}

StorageInfo storageInfo() {
  StorageInfo info{ready, 0, 0, 0, 0};
  if (!ready) return info;
  storageWithLock([&] {
    info.total = SD.totalBytes();
    info.used = SD.usedBytes();
    File dir = SD.open(DIR);
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      if (!f.isDirectory()) {
        info.logFiles++;
        info.logBytes += f.size();
      }
      f.close();
    }
    dir.close();
  });
  return info;
}

// ---- maintenance jobs ----

static volatile StorageJob currentJob = JOB_NONE;
static volatile JobState jobState = JOB_IDLE;

static bool clearLogs() {
  bool ok = true;
  File dir = SD.open(DIR);
  if (!dir) return false;
  std::vector<String> names;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (!f.isDirectory()) names.push_back(String(DIR) + "/" + f.name());
    f.close();
  }
  dir.close();
  for (const String &n : names) ok &= SD.remove(n);
  return ok;
}

static bool formatCard() {
  // Format the raw card (mounted or not) through the retrying driver, then remount normally.
  SD.end();
  ready = false;
  uint8_t pdrv = sdcard_init(SD_CS, &sdSpi, SD_FORMAT_FREQ);
  if (pdrv == 0xFF) {
    Serial.println("format: no card");
    return false;
  }
  ff_diskio_register(pdrv, &retryingDriver);

  const size_t workLen = 32 * 1024;  // bigger buffer = fewer, larger writes
  BYTE *work = (BYTE *)heap_caps_malloc(workLen, MALLOC_CAP_DMA);
  FRESULT res = FR_NOT_ENOUGH_CORE;
  if (work) {
    char drv[3] = {char('0' + pdrv), ':', 0};
    uint32_t t0 = millis();
    const MKFS_PARM opt = {FM_ANY, 0, 0, 0, 0};  // FatFs picks FAT type and cluster size
    res = f_mkfs(drv, &opt, work, workLen);
    free(work);
    Serial.printf("format: f_mkfs -> %d after %lu ms\n", res, millis() - t0);
  }
  sdcard_uninit(pdrv);
  ready = mount();
  return res == FR_OK && ready;
}

static void jobTask(void *) {
  bool ok = false;
  storageWithLock([&] { ok = currentJob == JOB_FORMAT ? formatCard() : ready && clearLogs(); });
  jobState = ok ? JOB_DONE_OK : JOB_DONE_FAIL;
  Serial.printf("storage job %d %s\n", currentJob, ok ? "ok" : "FAILED");
  vTaskDelete(nullptr);
}

bool storageStartJob(StorageJob job) {
  if (jobState == JOB_RUNNING) return false;
  currentJob = job;
  jobState = JOB_RUNNING;
  // Core 1: a long format never yields, and core 0's idle task is watched by the task watchdog.
  xTaskCreatePinnedToCore(jobTask, "sdjob", 8192, nullptr, 1, nullptr, 1);
  return true;
}

StorageJob storageCurrentJob() { return currentJob; }
JobState storageJobState() { return jobState; }
