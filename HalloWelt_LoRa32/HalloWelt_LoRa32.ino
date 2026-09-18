// Hallo Welt fuer Heltec WiFi LoRa 32 (V1) - 0.96" OLED, 128x64, SSD1306
// Board: esp32:esp32:heltec_wifi_lora_32

#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, RST_OLED, SCL_OLED, SDA_OLED);

static void drawCentered(const char *text, int y) {
  u8g2.drawStr((u8g2.getDisplayWidth() - u8g2.getStrWidth(text)) / 2, y, text);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nHallo Welt - Heltec WiFi LoRa 32 (V1)");

  pinMode(LED_BUILTIN, OUTPUT);

  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW);      // Peripherie einschalten
  delay(50);

  pinMode(RST_OLED, OUTPUT);    // OLED-Reset
  digitalWrite(RST_OLED, LOW);  delay(20);
  digitalWrite(RST_OLED, HIGH); delay(20);

  u8g2.begin();
  u8g2.setContrast(255);
}

void loop() {
  static uint32_t sekunden = 0;
  char buf[24];

  u8g2.clearBuffer();

  u8g2.setFont(u8g2_font_ncenB10_tr);
  drawCentered("Hallo Welt!", 22);

  u8g2.drawHLine(8, 30, u8g2.getDisplayWidth() - 16);

  u8g2.setFont(u8g2_font_6x10_tf);
  drawCentered("WiFi LoRa 32 V1", 44);

  snprintf(buf, sizeof(buf), "Laufzeit: %lu s", (unsigned long)sekunden);
  drawCentered(buf, 58);

  u8g2.sendBuffer();

  digitalWrite(LED_BUILTIN, sekunden % 2);
  Serial.printf("laeuft seit %lu s\n", (unsigned long)sekunden);

  sekunden++;
  delay(1000);
}
