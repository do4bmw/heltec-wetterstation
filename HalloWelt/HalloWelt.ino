// Hallo Welt fuer Heltec Wireless Stick (V2) - 0.49" OLED, 64x32, SSD1306
// Board: esp32:esp32:heltec_wireless_stick

#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>

// HW-I2C mit den OLED-Pins des Boards: (Rotation, Reset, SCL, SDA)
U8G2_SSD1306_64X32_1F_F_HW_I2C u8g2(U8G2_R0, RST_OLED, SCL_OLED, SDA_OLED);

static void drawCentered(const char *text, int y) {
  int x = (u8g2.getDisplayWidth() - u8g2.getStrWidth(text)) / 2;
  u8g2.drawStr(x, y, text);
}

void i2cScan() {
  Serial.println("I2C-Scan:");
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  Geraet gefunden: 0x%02X\n", addr);
    }
  }
  Serial.println("I2C-Scan fertig.");
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nHallo Welt - Heltec Wireless Stick");

  pinMode(LED_BUILTIN, OUTPUT);

  // Vext einschalten (LOW = an) - versorgt die Peripherie
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW);
  delay(50);

  // OLED-Reset
  pinMode(RST_OLED, OUTPUT);
  digitalWrite(RST_OLED, LOW);
  delay(20);
  digitalWrite(RST_OLED, HIGH);
  delay(20);

  Wire.begin(SDA_OLED, SCL_OLED);
  i2cScan();

  u8g2.begin();
  u8g2.setContrast(255);
}

void loop() {
  static uint32_t sekunden = 0;
  char buf[16];
  snprintf(buf, sizeof(buf), "%lu s", (unsigned long)sekunden);

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_7x13B_tr);
  drawCentered("Hallo", 12);
  drawCentered("Welt!", 24);
  u8g2.setFont(u8g2_font_4x6_tr);
  drawCentered(buf, 32);
  u8g2.sendBuffer();

  digitalWrite(LED_BUILTIN, sekunden % 2);
  Serial.printf("laeuft seit %lu s\n", (unsigned long)sekunden);

  sekunden++;
  delay(1000);
}
