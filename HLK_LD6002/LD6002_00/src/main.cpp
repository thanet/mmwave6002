// mmwave6002/HLK_LD6002/LD6002_00 — raw UART sniffer for the HLK-LD6002.
//
// LD6002's documented UART default is 1,382,400 baud — too fast for a generic
// CP210x USB-serial bridge on a PC (silent failures, confirmed by testing).
// ESP32's hardware UART has no such ceiling, so this reads it directly and
// hex-dumps every byte to the USB serial monitor (115200) with timestamps and
// idle-gap markers, so the real frame format can be reverse-engineered from an
// actual capture instead of guessed from a datasheet with sparse protocol docs.
//
// Wiring (LD6002 -> ESP32):
//   GND -> GND        (VCC left unconnected — module is powered via its own USB)
//   TX0 -> GPIO16 (ESP32 RX2)
//   (RX0 deliberately NOT wired — it's already driven by the module's onboard
//    USB-UART bridge; adding a second driver there would contend on the line)
//
// Runtime baud switch: send "b<number>\n" over the USB serial monitor (e.g.
// "b921600") to reinit Serial2 at a new baud rate without reflashing — lets a
// baud sweep run interactively while probing for the real rate.

#include <Arduino.h>

#define RX2_PIN      16
#define TX2_PIN      17
#define GAP_MS       50     // idle gap longer than this prints a blank line (likely a frame boundary)

static uint32_t currentBaud = 115200;   // empirically confirmed via baud sweep 2026-08-23 - NOT the documented 1,382,400
static uint32_t lastByteAt = 0;
static uint32_t bytesThisSecond = 0;
static uint32_t nonZeroThisSecond = 0;
static uint32_t lastRateReport = 0;
static uint32_t colCount = 0;
static String cmdBuf;

void beginRadar(uint32_t baud) {
    Serial2.end();
    delay(20);
    Serial2.begin(baud, SERIAL_8N1, RX2_PIN, TX2_PIN);
    currentBaud = baud;
    lastByteAt = 0;
    colCount = 0;
    Serial.printf("\n=== Serial2 reinit @ %lu baud ===\n", (unsigned long)baud);
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println();
    Serial.println("=== mmWeave_Sensor raw sniffer ===");
    Serial.println("Send 'b<number>' e.g. b921600 to change Serial2 baud live.");
    beginRadar(currentBaud);
    lastRateReport = millis();
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
        uint8_t b = Serial2.read();
        bytesThisSecond++;
        if (b != 0x00) nonZeroThisSecond++;

        if (lastByteAt != 0 && (now - lastByteAt) > GAP_MS) {
            Serial.printf("\n[gap %lums]\n", (unsigned long)(now - lastByteAt));
            colCount = 0;
        }
        lastByteAt = now;

        Serial.printf("%02X ", b);
        colCount++;
        if (colCount >= 16) {
            Serial.println();
            colCount = 0;
        }
    }

    if (now - lastRateReport >= 1000) {
        Serial.printf("\n-- baud=%lu  %lu bytes/sec  nonzero=%lu --\n",
                      (unsigned long)currentBaud, (unsigned long)bytesThisSecond, (unsigned long)nonZeroThisSecond);
        bytesThisSecond = 0;
        nonZeroThisSecond = 0;
        lastRateReport = now;
        colCount = 0;
    }
}
