#pragma once
#include <Arduino.h>
#include <time.h>
#include "config.h"

struct TargetState {
  bool known = false;     // at least one probe has completed
  bool up = false;
  float rttMs = 0;
  uint8_t fails = 0;
  time_t since = 0;       // when the current up/down state began
};

struct DnsState {
  bool known = false;
  bool up = false;
  float rttMs = 0;
  uint8_t fails = 0;
  time_t since = 0;
  char lastName[32] = "";
};

static const int TOP_BLOCKED = 5;

struct AdguardState {
  bool known = false;
  bool ok = false;
  uint32_t queries = 0;
  uint32_t blocked = 0;
  float avgMs = 0;
  bool protectionOn = true;
  uint32_t pausedMs = 0;  // remaining pause time when protection is off
  char topBlocked[TOP_BLOCKED][40] = {};
  uint32_t topBlockedCount[TOP_BLOCKED] = {};
  int numTopBlocked = 0;
};

struct Event {
  time_t at;
  char what[24];
  bool up;
  uint32_t downSecs;      // for "up" events: how long it was down
};

static const int MAX_EVENTS = 64;
static const int NUM_PROBES = NUM_TARGETS + 1;                 // targets, then DNS
static const int HISTORY_LEN = 30 * 60 * 1000 / PING_INTERVAL_MS;  // 30 minutes

struct Snapshot {
  TargetState targets[NUM_TARGETS];
  DnsState dns;
  AdguardState adguard;
  Event events[MAX_EVENTS];   // newest first
  int numEvents;
  uint32_t totalEvents;       // never wraps back like numEvents caps
  uint32_t version;           // bumps on every change
};

// Seeds the outage log (newest first) from storage; call before monitorStart().
void monitorRestoreEvents(const Event *events, int n);
void monitorStart();
// Pauses or resumes probing (the task stays alive). Known states are kept but go stale.
void monitorSetEnabled(bool on);
bool monitorEnabled();
void monitorSnapshot(Snapshot &out);
uint32_t monitorVersion();
// Copies the probe's latency history, oldest first. Failed probes are NAN. Returns the sample count.
int monitorHistory(int probe, float *out, int max);
// AdGuard Home address and login (Settings › Apps › Netwatch). An empty password turns stats off.
struct AdguardConfig {
  String host, user, pass;
};
AdguardConfig monitorAdguardConfig();
void monitorSetAdguardConfig(const AdguardConfig &c);
// Asks the monitor task to pause (enable=false, for durationMs) or resume AdGuard protection.
void monitorSetProtection(bool enable, uint32_t durationMs);
