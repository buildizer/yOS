#pragma once
#include <IPAddress.h>

// Sends one A query straight to `server` (bypassing the ESP's resolver cache).
// Returns true if a NOERROR answer with at least one record came back.
bool dnsProbe(const IPAddress &server, const char *name, uint32_t timeoutMs, float &rttMs);
