#include <Arduino.h>

#include "smoke.h"

namespace {

void SerialPrint(const char* s) {
  Serial.print(s);
  Serial.flush();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  // Give the USB/UART bridge a moment; QEMU UART is ready immediately.
  delay(200);
  Serial.println("aster-sim-arduino boot");
  Serial.flush();
  aster::sim::RunSmoke(SerialPrint);
  Serial.flush();
}

void loop() {
  // Stay idle after the one-shot smoke so QEMU can observe ASTER_OK.
  delay(1000);
}
