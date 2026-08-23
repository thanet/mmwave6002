// mmwave6002/HLK_LD6002/LD6002_00 — TinyFrame decoder for the HLK-LD6002B.
//
// Phase 1 was a raw hex-dump sniffer used to reverse-engineer the frame format
// from an actual capture. That's done: the module on hand is the LD6002B
// (3D human presence / position variant, not the plain breath/heart-rate
// LD6002), confirmed against Hi-Link's official
// "HLK-LD6002B human presence Communication protocol.pdf" (TinyFrame framing).
// This phase parses frames live and prints decoded presence/target data.
//
// TinyFrame layout:
//   SOF(1,=0x01) ID(2,BE) LEN(2,BE) TYPE(2,BE) HEAD_CKSUM(1)
//   DATA(LEN bytes, multi-byte fields little-endian) DATA_CKSUM(1)
// Checksum = ~(XOR of all bytes in the covered range) & 0xFF
//   - HEAD_CKSUM covers SOF..TYPE (the 7 header bytes)
//   - DATA_CKSUM covers DATA only
// Both verified byte-for-byte against a live capture 2026-08-23.
//
// Message types decoded here:
//   0x0A0A "anyone in area" — DATA = 4x uint32 LE (area0..area3), 1=occupied
//   0x0A04 "person location" — DATA = uint32 target_count, then per target:
//                               x,y,z float32 LE + dop_idx,cluster_id int32 LE
//                               (20 bytes/target)
// Hi-Link's PDF documents further types (zone config 0x0201-0x0205, status
// queries 0x0A0B-0x0A14, ...) — not decoded here, just logged by TYPE.
//
// Wiring (LD6002B -> ESP32):
//   GND -> GND        (VCC left unconnected — module is powered via its own USB)
//   TX0 -> GPIO16 (ESP32 RX2)
//   (RX0 deliberately NOT wired — it's already driven by the module's onboard
//    USB-UART bridge; adding a second driver there would contend on the line)
//
// Runtime baud switch: send "b<number>\n" over the USB serial monitor (e.g.
// "b921600") to reinit Serial2 at a new baud rate without reflashing.

#include <Arduino.h>
#include <string.h>

#define RX2_PIN      16
#define TX2_PIN      17
#define RADAR_BAUD   115200   // confirmed 2026-08-23 via baud sweep — NOT the documented 1,382,400
#define MAX_DATA     250

#define SOF            0x01
#define TYPE_PRESENCE  0x0A0A
#define TYPE_TARGETS   0x0A04

enum FrameState { ST_SOF, ST_ID, ST_LEN, ST_TYPE, ST_HEAD_CKSUM, ST_DATA, ST_DATA_CKSUM };

static FrameState state = ST_SOF;
static uint8_t  hdr[7];      // SOF, ID(2,BE), LEN(2,BE), TYPE(2,BE) — kept whole for the head checksum
static uint8_t  hdrIdx = 0;
static uint16_t frameId = 0, frameLen = 0, frameType = 0;
static uint8_t  data[MAX_DATA];
static uint16_t dataIdx = 0;
static uint32_t framesOk = 0, framesBad = 0;
static uint32_t lastStatsAt = 0;
static String   cmdBuf;

static uint8_t xorChecksum(const uint8_t* p, uint16_t n) {
    uint8_t x = 0;
    for (uint16_t i = 0; i < n; i++) x ^= p[i];
    return (uint8_t)~x;
}

static void resetFrame() {
    state = ST_SOF;
    hdrIdx = 0;
    dataIdx = 0;
}

static void handlePresence(const uint8_t* d, uint16_t len) {
    if (len < 16) {
        Serial.printf("[presence] short frame (%u bytes), skipping\n", len);
        return;
    }
    uint32_t area[4];
    memcpy(area, d, sizeof(area));
    Serial.printf("[presence] area0=%lu area1=%lu area2=%lu area3=%lu\n",
                  (unsigned long)area[0], (unsigned long)area[1],
                  (unsigned long)area[2], (unsigned long)area[3]);
}

static void handleTargets(const uint8_t* d, uint16_t len) {
    if (len < 4) {
        Serial.printf("[targets] short frame (%u bytes), skipping\n", len);
        return;
    }
    uint32_t count;
    memcpy(&count, d, 4);
    uint32_t need = 4 + count * 20;
    if (need > len) {
        Serial.printf("[targets] count=%lu but frame too short (%u < %lu bytes)\n",
                      (unsigned long)count, len, (unsigned long)need);
        return;
    }
    if (count == 0) {
        Serial.println("[targets] none");
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* t = d + 4 + i * 20;
        float x, y, z;
        int32_t dopIdx, clusterId;
        memcpy(&x,         t + 0,  4);
        memcpy(&y,         t + 4,  4);
        memcpy(&z,         t + 8,  4);
        memcpy(&dopIdx,    t + 12, 4);
        memcpy(&clusterId, t + 16, 4);
        Serial.printf("[target %lu/%lu] x=%.3fm y=%.3fm z=%.3fm dop=%ld cluster=%ld\n",
                      (unsigned long)(i + 1), (unsigned long)count, x, y, z,
                      (long)dopIdx, (long)clusterId);
    }
}

static void onByte(uint8_t b) {
    switch (state) {
    case ST_SOF:
        if (b == SOF) {
            hdr[0] = b;
            hdrIdx = 1;
            state = ST_ID;
        }
        break;

    case ST_ID:
        hdr[hdrIdx++] = b;
        if (hdrIdx == 3) state = ST_LEN;
        break;

    case ST_LEN:
        hdr[hdrIdx++] = b;
        if (hdrIdx == 5) state = ST_TYPE;
        break;

    case ST_TYPE:
        hdr[hdrIdx++] = b;
        if (hdrIdx == 7) {
            frameId   = (hdr[1] << 8) | hdr[2];
            frameLen  = (hdr[3] << 8) | hdr[4];
            frameType = (hdr[5] << 8) | hdr[6];
            state = ST_HEAD_CKSUM;
        }
        break;

    case ST_HEAD_CKSUM: {
        uint8_t expect = xorChecksum(hdr, 7);
        if (b != expect || frameLen > MAX_DATA) {
            framesBad++;
            resetFrame();
        } else {
            dataIdx = 0;
            state = (frameLen == 0) ? ST_DATA_CKSUM : ST_DATA;
        }
        break;
    }

    case ST_DATA:
        data[dataIdx++] = b;
        if (dataIdx == frameLen) state = ST_DATA_CKSUM;
        break;

    case ST_DATA_CKSUM: {
        uint8_t expect = xorChecksum(data, frameLen);
        if (b == expect) {
            framesOk++;
            switch (frameType) {
            case TYPE_PRESENCE: handlePresence(data, frameLen); break;
            case TYPE_TARGETS:  handleTargets(data, frameLen);  break;
            default:
                Serial.printf("[type 0x%04X] id=%u len=%u (unhandled)\n", frameType, frameId, frameLen);
                break;
            }
        } else {
            framesBad++;
        }
        resetFrame();
        break;
    }
    }
}

void beginRadar(uint32_t baud) {
    Serial2.end();
    delay(20);
    Serial2.begin(baud, SERIAL_8N1, RX2_PIN, TX2_PIN);
    resetFrame();
    framesOk = 0;
    framesBad = 0;
    Serial.printf("\n=== Serial2 reinit @ %lu baud ===\n", (unsigned long)baud);
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println();
    Serial.println("=== HLK-LD6002B TinyFrame decoder ===");
    Serial.println("Send 'b<number>' e.g. b921600 to change Serial2 baud live.");
    beginRadar(RADAR_BAUD);
    lastStatsAt = millis();
}

void loop() {
    uint32_t now = millis();

    // Runtime baud-change command from the USB serial monitor.
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (cmdBuf.length() > 1 && cmdBuf[0] == 'b') {
                uint32_t nb = cmdBuf.substring(1).toInt();
                if (nb > 0) beginRadar(nb);
            }
            cmdBuf = "";
        } else {
            cmdBuf += c;
        }
    }

    while (Serial2.available()) {
        onByte((uint8_t)Serial2.read());
    }

    if (now - lastStatsAt >= 5000) {
        Serial.printf("-- frames ok=%lu bad=%lu --\n", (unsigned long)framesOk, (unsigned long)framesBad);
        lastStatsAt = now;
    }
}
