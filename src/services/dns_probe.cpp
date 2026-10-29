#include "services/dns_probe.h"
#include <WiFiUdp.h>
#include <esp_random.h>

static size_t buildQuery(uint8_t *buf, size_t cap, uint16_t id, const char *name) {
  size_t n = 0;
  const uint8_t header[12] = {
    uint8_t(id >> 8), uint8_t(id), 0x01, 0x00,  // recursion desired
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  memcpy(buf, header, sizeof(header));
  n = sizeof(header);
  const char *label = name;
  while (*label) {
    const char *dot = strchr(label, '.');
    size_t len = dot ? size_t(dot - label) : strlen(label);
    if (len == 0 || len > 63 || n + len + 1 >= cap - 5) return 0;
    buf[n++] = uint8_t(len);
    memcpy(buf + n, label, len);
    n += len;
    label += len + (dot ? 1 : 0);
  }
  buf[n++] = 0;
  const uint8_t tail[4] = {0x00, 0x01, 0x00, 0x01};  // QTYPE A, QCLASS IN
  memcpy(buf + n, tail, sizeof(tail));
  return n + sizeof(tail);
}

bool dnsProbe(const IPAddress &server, const char *name, uint32_t timeoutMs, float &rttMs) {
  uint8_t query[128];
  uint16_t id = uint16_t(esp_random());
  size_t len = buildQuery(query, sizeof(query), id, name);
  if (!len) return false;

  WiFiUDP udp;
  if (!udp.begin(0)) return false;
  uint32_t start = millis();
  bool ok = false;
  if (udp.beginPacket(server, 53) && udp.write(query, len) == len && udp.endPacket()) {
    while (millis() - start < timeoutMs) {
      if (udp.parsePacket() >= 12) {
        uint8_t resp[12];
        udp.read(resp, sizeof(resp));
        uint16_t rid = (resp[0] << 8) | resp[1];
        bool isResponse = resp[2] & 0x80;
        uint8_t rcode = resp[3] & 0x0F;
        uint16_t answers = (resp[6] << 8) | resp[7];
        if (rid == id && isResponse) {
          ok = rcode == 0 && answers > 0;
          break;
        }
      }
      delay(5);
    }
  }
  rttMs = millis() - start;
  udp.stop();
  return ok;
}
