#pragma once
#include <Arduino.h>
#include <vector>

// Wi-Fi manager: saved networks (NVS), boot auto-connect, async scan and connect.
// All radio work runs in its own task; the UI only reads snapshots and posts requests, so it never blocks.
namespace wifimgr {
enum State { OFF, DISCONNECTED, CONNECTING, CONNECTED };

struct Network {
  String ssid;
  int rssi;
  bool secure;
  bool saved;
};

void begin();                          // load saved networks (joined in Settings › Wi-Fi), start the task
State state();
bool enabled();
void setEnabled(bool on);              // persisted; off = radio off

String currentSsid();                  // "" unless connected
String connectingSsid();               // "" unless an attempt is in progress
String failedSsid();                   // last attempt the person started that failed; cleared by the next attempt
String firstSavedSsid();               // for the boot splash, "" when none
int rssi();                            // dBm of the current network

void requestScan();                    // async; results appear in results()
bool scanning();
std::vector<Network> results();        // deduplicated, strongest first
std::vector<String> saved();
bool isSaved(const String &ssid);

// Joins a network. An empty password uses the stored one when the network is saved. A successful join is saved.
void connect(const String &ssid, const String &pass);
void forget(const String &ssid);       // removes it; leaves the network if it is the current one
}
