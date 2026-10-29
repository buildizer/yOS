#pragma once
#include <Arduino.h>
#include "services/monitor.h"

bool storageStart();
bool storageReady();
uint64_t storageTotalBytes();
uint64_t storageUsedBytes();

void storageAppendEvent(const Event &e);
int storageLoadEvents(Event *out, int max);  // newest first
// Appends one line to today's CSV: per-probe average RTT (blank if all failed) and lost count.
void storageAppendMinute(time_t at, const float *avgMs, const int *lost, int numProbes);

// Run fn while holding the SD lock (the SD library is not thread safe).
void storageWithLock(std::function<void()> fn);

// Background maintenance jobs (formatting can take several seconds).
enum StorageJob { JOB_NONE, JOB_FORMAT, JOB_CLEAR_LOGS };
enum JobState { JOB_IDLE, JOB_RUNNING, JOB_DONE_OK, JOB_DONE_FAIL };
bool storageStartJob(StorageJob job);   // false if a job is already running
StorageJob storageCurrentJob();
JobState storageJobState();

struct StorageInfo {
  bool ready;
  uint64_t total, used;
  int logFiles;
  uint32_t logBytes;
};
StorageInfo storageInfo();
