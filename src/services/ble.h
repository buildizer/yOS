#pragma once
#include <stdint.h>
#include <stddef.h>

// Bluetooth LE service (NimBLE). All radio work runs in its own task; the UI only posts commands
// and polls status(), so nothing here blocks the LVGL task.
void bleBegin();  // called once at boot: loads settings, starts the stack when enabled

namespace ble {
static const int MAX_LIST = 24;  // most devices kept from a scan
static const int MAX_PHONES = 3; // bonded phones listed (MAX_BONDS in platformio.ini is this + the device)
enum Link : uint8_t { LINK_IDLE, LINK_CONNECTING, LINK_CONNECTED };
// A phone pairing with our GATT server: the board shows a passkey, the phone types it.
enum Pairing : uint8_t { PAIR_NONE, PAIR_PASSKEY, PAIR_DONE, PAIR_FAILED };

struct Device {
  char name[32];   // "Unknown" when the device does not advertise a name
  char addr[18];   // "aa:bb:cc:dd:ee:ff"
  uint8_t type;    // BLE address type
  int8_t rssi;
  bool connectable;
};

struct Phone {     // a phone bonded to our GATT server
  char name[32];   // its GAP device name when we could read it, else "Phone"
  char addr[18];   // identity address
  uint8_t type;
};

struct Status {
  bool enabled;       // the setting
  bool ready;         // stack is up (enabled and initialised)
  bool discoverable;
  bool scanning;
  bool peerConnected; // a phone is connected to our GATT server
  Link link;          // link to the remembered device
  bool hasBond;       // a device is remembered
  bool paired;        // ...and the link is encrypted / bonded
  bool connectFailed; // last connect attempt failed (cleared by the next attempt)
  char target[32];    // device being connected to, or whose connect failed
  char bondName[32];
  char bondAddr[18];
  char error[56];     // stack-level failure (e.g. out of memory), empty when fine
  Pairing pairing;    // DONE / FAILED stay until clearPairing()
  uint32_t passkey;   // 0..999999, valid while pairing == PAIR_PASSKEY
  char peerName[32];  // the connecting phone's name, empty until read
  uint32_t version;   // bumped on every change: poll this to know when to redraw
};

Status status();
int devices(Device *out, int max);   // scan results, strongest first

void setEnabled(bool on);
void setDiscoverable(bool on);
void scan();                         // 5 s scan, results appear in devices()
void connect(const Device &d);       // connect + pair (Just Works) + remember
void connectBonded();                // reconnect to the remembered device
void disconnect();                   // keeps the device remembered
void forget();                       // disconnect and delete the bond

int phones(Phone *out, int max);     // phones bonded to the GATT server (only while the stack is up)
void forgetPhone(const Phone &p);    // drop its bond (and its link, if connected)
void cancelPairing();                // reject the pairing on screen: drops the phone's link
void clearPairing();                 // the UI has shown the DONE / FAILED result

struct HeapCost { uint32_t freeBefore, freeAfter, internalBefore, internalAfter; };
HeapCost lastStartCost();            // heap at the last stack start (before init / after init)
HeapCost lastStopCost();             // heap at the last stack stop (before deinit / after deinit)
}
