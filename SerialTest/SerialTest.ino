void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("SerialTest starting...");
}

void loop() {
  static uint32_t count = 0;
  Serial.println(count);
  count++;
  delay(500);
}
