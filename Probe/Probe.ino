// Erkennungs-Sketch: welches Heltec-Board ist das?
// I2C-Scan + SX127x-Test, Display abwechselnd als 128x64 und 64x32.

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <U8g2lib.h>

#define OLED_SDA 4
#define OLED_SCL 15
#define OLED_RST 16
#define VEXT     21
#define LED_PIN  25

U8G2_SSD1306_128X64_NONAME_F_HW_I2C disp128(U8G2_R0, OLED_RST, OLED_SCL, OLED_SDA);
U8G2_SSD1306_64X32_1F_F_HW_I2C      disp64 (U8G2_R0, OLED_RST, OLED_SCL, OLED_SDA);

bool displayDa = false;

// SX127x an zwei moeglichen Pinbelegungen testen (WiFi LoRa 32 V1 / WiFi Kit 32)
uint8_t leseLoRaVersion(int sck, int miso, int mosi, int nss, int rst) {
  pinMode(rst, OUTPUT);
  digitalWrite(rst, LOW);  delay(10);
  digitalWrite(rst, HIGH); delay(10);

  SPI.begin(sck, miso, mosi, nss);
  pinMode(nss, OUTPUT);
  digitalWrite(nss, HIGH);

  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(nss, LOW);
  SPI.transfer(0x42 & 0x7F);           // RegVersion, Lesezugriff
  uint8_t v = SPI.transfer(0x00);
  digitalWrite(nss, HIGH);
  SPI.endTransaction();
  SPI.end();
  return v;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n===== Board-Erkennung =====");
  Serial.printf("Chip:  %s, %d Kerne, Rev %d\n",
                ESP.getChipModel(), ESP.getChipCores(), ESP.getChipRevision());
  Serial.printf("Flash: %u Bytes\n", ESP.getFlashChipSize());
  Serial.printf("MAC:   %s\n", WiFi.macAddress().c_str());

  pinMode(LED_PIN, OUTPUT);
  pinMode(VEXT, OUTPUT);
  digitalWrite(VEXT, LOW);
  delay(50);

  pinMode(OLED_RST, OUTPUT);
  digitalWrite(OLED_RST, LOW);  delay(20);
  digitalWrite(OLED_RST, HIGH); delay(20);

  Wire.begin(OLED_SDA, OLED_SCL);
  Serial.println("--- I2C-Scan (SDA=4, SCL=15):");
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("    gefunden: 0x%02X\n", a);
      if (a == 0x3C || a == 0x3D) displayDa = true;
    }
  }
  if (!displayDa) Serial.println("    kein Display gefunden");

  Serial.println("--- LoRa-Test:");
  uint8_t v1 = leseLoRaVersion(5, 19, 27, 18, 14);
  Serial.printf("    WiFi LoRa 32 V1 Pins (NSS=18): RegVersion = 0x%02X %s\n",
                v1, v1 == 0x12 ? "-> SX127x gefunden!" : "");
  uint8_t v2 = leseLoRaVersion(18, 19, 23, 5, 14);
  Serial.printf("    WiFi Kit 32 Pins (NSS=5):      RegVersion = 0x%02X %s\n",
                v2, v2 == 0x12 ? "-> SX127x gefunden!" : "");

  Serial.println("--- Display wechselt alle 4s zwischen 128x64 und 64x32.");

  if (displayDa) { disp128.begin(); disp64.begin(); }
}

void zeichne(U8G2 &d, const char *label) {
  d.begin();
  d.clearBuffer();
  d.drawFrame(0, 0, d.getDisplayWidth(), d.getDisplayHeight());
  d.setFont(u8g2_font_7x13B_tr);
  const char *t = "Hallo Welt";
  int w = d.getStrWidth(t);
  if (w > d.getDisplayWidth()) { d.setFont(u8g2_font_5x7_tr); w = d.getStrWidth(t); }
  d.drawStr((d.getDisplayWidth() - w) / 2, d.getDisplayHeight() / 2, t);
  d.setFont(u8g2_font_4x6_tr);
  d.drawStr((d.getDisplayWidth() - d.getStrWidth(label)) / 2, d.getDisplayHeight() - 3, label);
  d.sendBuffer();
}

void loop() {
  static bool gross = true;
  digitalWrite(LED_PIN, !digitalRead(LED_PIN));

  if (displayDa) {
    if (gross) zeichne(disp128, "128x64");
    else       zeichne(disp64,  "64x32");
    Serial.printf("zeige: %s\n", gross ? "128x64" : "64x32");
  } else {
    Serial.println("kein Display - LED blinkt");
  }
  gross = !gross;
  delay(4000);
}
