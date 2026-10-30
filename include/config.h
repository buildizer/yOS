#pragma once
#include <IPAddress.h>

#define HOSTNAME        "netwatch"
// Software updates come from GitHub Releases of this repo (an asset named firmware.bin, tag v<version>).
// The board signs in through the "yOS Updater" GitHub App's device flow; its client ID is public, not a secret.
#define GITHUB_REPO          "buildizer/yOS"
#define GITHUB_APP_CLIENT_ID ""
#define GITHUB_FW_ASSET      "firmware.bin"
// The App Store's catalog: store/index.json and one folder per app, on this branch of GITHUB_REPO.
// When the board is signed in to GitHub (above) it reads them through the contents API with that
// token, which works for a private repo; signed out it uses STORE_BASE_URL, which needs a public repo.
#define STORE_BRANCH         "main"
#define STORE_DIR            "store"
#define STORE_BASE_URL       "https://raw.githubusercontent.com/" GITHUB_REPO "/" STORE_BRANCH "/" STORE_DIR

#define TZ_INFO         "EET-2EEST,M4.5.5/0,M10.5.4/24"  // Africa/Cairo

#define PING_INTERVAL_MS   5000
#define ADGUARD_INTERVAL_MS 60000
#define FAILS_BEFORE_DOWN  2        // consecutive failures before a target counts as down

#define ADGUARD_IP      IPAddress(192, 168, 88, 90)
#define DNS_TEST_NAMES  { "google.com", "cloudflare.com", "apple.com", "wikipedia.org" }

struct TargetDef {
  const char *name;
  IPAddress ip;
};

static const TargetDef TARGETS[] = {
  {"Router",     IPAddress(192, 168, 88, 1)},
  {"WAN 1 Gateway", IPAddress(192, 168, 1, 1)},
  {"WAN 2 Gateway", IPAddress(192, 168, 2, 1)},
  {"AdGuard Pi", IPAddress(192, 168, 88, 90)},
  {"Deco",       IPAddress(192, 168, 88, 107)},
};
static const int NUM_TARGETS = sizeof(TARGETS) / sizeof(TARGETS[0]);
