// Brute-Force-Suche: an welchen Pins haengt der I2C-Sensor?
// Probiert alle freien Pin-Paare als SDA/SCL durch (auch vertauscht)
// und scannt dabei den kompletten Adressbereich.

#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, RST_OLED, SCL_OLED, SDA_OLED);

// Freie, als I2C nutzbare GPIOs (36-39 sind nur Eingaenge, 0/12 Strapping - raus)
const int pins[] = { 2, 13, 17, 21, 22, 23, 32, 33 };
const int nPins  = sizeof(pins) / sizeof(pins[0]);

int treffer = 0;

void meldung(const char *zeile1, const char *zeile2) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 12, "I2C-Pinsuche");
  u8g2.drawHLine(0, 16, 128);
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(0, 32, zeile1);
  u8g2.drawStr(0, 44, zeile2);
  u8g2.sendBuffer();
}

// Scannt einen Bus, gibt Anzahl gefundener Geraete zurueck
int scanne(TwoWire &bus, const char *wo) {
  int n = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    bus.beginTransmission(addr);
    if (bus.endTransmission() == 0) {
      const char *was = "";
      if (addr == 0x3C || addr == 0x3D) was = " (OLED)";
      if (addr == 0x76 || addr == 0x77) was = " <-- BME280/BMP280!";
      Serial.printf("  TREFFER: 0x%02X auf %s%s\n", addr, wo, was);
      n++;
    }
  }
  return n;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n===== I2C-Pinsuche =====");

  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW);
  delay(50);
  pinMode(RST_OLED, OUTPUT);
  digitalWrite(RST_OLED, LOW);  delay(20);
  digitalWrite(RST_OLED, HIGH); delay(20);
  u8g2.begin();

  // 1) Der Bus, an dem das OLED haengt
  Serial.println("--- OLED-Bus (SDA=4, SCL=15):");
  meldung("Pruefe OLED-Bus", "SDA=4 SCL=15");
  treffer += scanne(Wire, "SDA=4 SCL=15");

  // 2) Alle anderen Pin-Paare durchprobieren
  Serial.println("--- Brute-Force ueber freie Pins:");
  char z1[32], z2[32];
  for (int i = 0; i < nPins; i++) {
    for (int j = 0; j < nPins; j++) {
      if (i == j) continue;
      int sda = pins[i], scl = pins[j];

      snprintf(z1, sizeof(z1), "Teste SDA=%d SCL=%d", sda, scl);
      snprintf(z2, sizeof(z2), "Treffer bisher: %d", treffer);
      meldung(z1, z2);

      Wire1.end();
      delay(5);
      if (!Wire1.begin(sda, scl, 100000)) continue;
      delay(5);

      char wo[24];
      snprintf(wo, sizeof(wo), "SDA=%d SCL=%d", sda, scl);
      treffer += scanne(Wire1, wo);
    }
  }
  Wire1.end();

  Serial.printf("--- Fertig. %d Treffer insgesamt.\n", treffer);
  if (treffer <= 1) {
    Serial.println("Nur das OLED (oder gar nichts) gefunden.");
    Serial.println("Pruefen: VIN an 3V3? GND verbunden? CSB an 3V3 (I2C-Modus)?");
  }
}

void loop() {
  char z[32];
  snprintf(z, sizeof(z), "Treffer: %d", treffer);
  meldung("Suche beendet", z);
  Serial.println("(Suche beendet - Ausgabe oben)");
  delay(5000);
}
