#pragma once
#include <Arduino.h>

// Software updates from GitHub Releases of a private repo.
// The board signs in once with GitHub's device flow (a code shown on screen, approved on a phone) and keeps
// the resulting read-only token in NVS. It checks the latest release, compares its tag with
// YOS_FIRMWARE_VERSION and streams firmware.bin over HTTPS into the spare OTA slot.
// Network work runs on a short-lived task; everything here is safe to call from the UI.
namespace ghupdate {

enum State : uint8_t {
  NOT_CONFIGURED,  // no GitHub App client ID in config.h
  SIGNED_OUT,
  SIGNING_IN,      // showing userCode, waiting for approval on github.com
  IDLE,            // signed in, nothing checked yet this boot
  CHECKING,
  UP_TO_DATE,
  AVAILABLE,
  INSTALLING,
  FAILED,          // error says why; signed-in state is kept unless the token was refused
};

struct Status {
  State state = SIGNED_OUT;
  bool signedIn = false; // holds a GitHub token
  String login;          // GitHub account the board is signed in as
  String userCode;       // SIGNING_IN: the code to enter on github.com/login/device
  String verifyUri;
  uint32_t codeExpiresMs = 0;  // millis() when the code stops working
  String latest;         // AVAILABLE / UP_TO_DATE: newest release version, without the "v"
  String notes;          // first lines of its release notes
  uint32_t size = 0;     // firmware.bin bytes
  time_t lastCheck = 0;  // wall-clock time of the last successful check
  String error;
};

void begin();          // load the saved sign-in; call once after prefs::load()
void loop();           // from loop(): runs the daily automatic check
Status status();
uint32_t version();    // bumps on every change, for UIs to refresh

void signIn();         // starts the device flow
void cancelSignIn();
void signOut();
void check();
void install();        // AVAILABLE only: shows the update screen and restarts when done
bool installing();     // a GitHub install owns the flash and the update screen; other updaters must wait

bool autoCheck();
void setAutoCheck(bool on);

int compareVersions(const char *a, const char *b);  // <0, 0, >0 for dotted numbers ("1.10" > "1.9")
}
