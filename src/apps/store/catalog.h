#pragma once
#include <Arduino.h>
#include <vector>

// The App Store's catalog and downloads: store/index.json and one folder per app in the yOS repo on
// GitHub (STORE_* in config.h). Signed in to GitHub (Settings › General › About › Software Update) it
// reads them with the board's token through the contents API, which also works while the repo is
// private; signed out it uses the public raw URL.
// Network work, installs and removals run one after another on a short-lived task, like ghupdate; the
// script loader validates, stores and registers the apps, so they show up without a restart.
// Everything here is safe to call from the UI.
namespace appstore {

struct Listing {
  String id, name, icon, color, version;
  String summary;      // one line, under the name
  String description;
  String notes;        // What's New
  String requires;     // lowest yOS version it runs on ("" = any)
  String needs;        // e.g. "Wi-Fi" ("" = nothing)
  String developer;
  uint32_t size = 0;   // bytes of all its files
  std::vector<String> files;
};

enum State : uint8_t { EMPTY, LOADING, READY, FAILED };

struct Status {
  State state = EMPTY;
  String error;                // FAILED: why the catalog couldn't be loaded
  std::vector<Listing> apps;   // READY (kept while reloading)
  String featured;             // id of the app in the Discover banner
  String busy;                 // id being downloaded or removed ("" = none)
  uint8_t pct = 0;             // its download progress
  std::vector<String> queued;  // ids waiting their turn
  String jobError;             // the last install or removal that failed, until clearJobError()
};

void refresh();                    // (re)loads the catalog
void install(const String &id);    // installs or updates; waits behind a running job
void remove(const String &id);     // deletes an installed script app
bool pending(const String &id);    // busy or queued
Status status();
uint32_t version();                // bumps on every change, for UIs to refresh
void clearJobError();
}
