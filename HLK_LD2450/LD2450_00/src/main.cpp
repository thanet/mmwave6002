#include <Arduino.h>

HardwareSerial radar(2);

#define FRAME_END_1 0x55
#define FRAME_END_2 0xCC

uint8_t buffer[128];
int bufIndex = 0;

int16_t readInt16(uint8_t *buf, int pos) {
  return (int16_t)(buf[pos] | (buf[pos + 1] << 8));
}

void setup() {
  Serial.begin(115200);
  radar.begin(256000, SERIAL_8N1, 16, 17); // RX, TX
  Serial.println("LD2450 started");
}

void processFrame(uint8_t *buf, int len) {
  Serial.println("---- Frame ----");

  // Target data usually starts at byte 6
  int offset = 6;

  for (int i = 0; i < 3; i++) {
    int16_t x = readInt16(buf, offset);
    int16_t y = readInt16(buf, offset + 2);
    int16_t speed = readInt16(buf, offset + 4);

    if (x != 0 || y != 0) {
      Serial.printf(
        "Target %d: X=%d mm  Y=%d mm  Speed=%d\n",
        i + 1, x, y, speed
      );
    }

    offset += 8;
  }
}

void loop() {
  while (radar.available()) {
    uint8_t b = radar.read();
    buffer[bufIndex++] = b;

    if (bufIndex >= 2 &&
        buffer[bufIndex - 2] == FRAME_END_1 &&
        buffer[bufIndex - 1] == FRAME_END_2) {

      processFrame(buffer, bufIndex);
      bufIndex = 0;
    }

    if (bufIndex >= sizeof(buffer)) {
      bufIndex = 0;
    }
  }
  delay(1000);
}
