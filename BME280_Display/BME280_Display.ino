// Umweltdaten-Anzeige fuer Heltec WiFi LoRa 32 (V1/V2), 128x64 OLED
// Unterstuetzt BME280 (Temp/Feuchte/Druck) und BMP280 (Temp/Druck).
//
// Der Sensor wird automatisch gesucht: OLED-Bus (SDA=4/SCL=15),
// SDA=21/SCL=22, SDA=23/SCL=22, SDA=22/SCL=23; Adressen 0x76 und 0x77.
//
// Hinweis: GPIO21 ist auf Heltec-Boards zugleich der Vext-Steuerpin.
// Wird der Sensor dort gefunden, laesst der Sketch Vext unangetastet.

#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Adafruit_BME280.h>
#include <Adafruit_BMP280.h>

// Standorthoehe in Metern ueber NN - noetig, um den gemessenen absoluten
// Luftdruck auf Meereshoehe zu reduzieren (QNH, wie im Wetterbericht).
static const float HOEHE_UEBER_NN = 380.0f;

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, RST_OLED, SCL_OLED, SDA_OLED);

// Barometrische Hoehenformel nach ISA-Standardatmosphaere
float aufMeereshoehe(float druckAbsolut, float hoehe) {
  return druckAbsolut / powf(1.0f - (hoehe / 44330.0f), 5.255f);
}

Adafruit_BME280 bme;

// Adafruit_BMP280 legt den Bus im Konstruktor fest - deshalb je ein Objekt
// pro Bus, und ein Zeiger auf das tatsaechlich benutzte.
Adafruit_BMP280  bmpWire0(&Wire);
Adafruit_BMP280  bmpWire1(&Wire1);
Adafruit_BMP280 *bmp = nullptr;

struct BusKandidat {
  TwoWire    *bus;
  int         sda, scl;
  bool        initNoetig;          // den OLED-Bus hat u8g2 schon aufgesetzt
  const char *beschreibung;
};

BusKandidat kandidaten[] = {
  { &Wire,  SDA_OLED, SCL_OLED, false, "SDA=4 SCL=15"  },
  { &Wire1, 21,       22,       true,  "SDA=21 SCL=22" },
  { &Wire1, 23,       22,       true,  "SDA=23 SCL=22" },
  { &Wire1, 22,       23,       true,  "SDA=22 SCL=23" },
};

enum SensorTyp { KEINER, TYP_BME280, TYP_BMP280 };

SensorTyp   typ        = KEINER;
const char *sensorBus  = "";
uint8_t     sensorAddr = 0;
bool        wire1Offen = false;

// Register 0xD0: 0x60 = BME280, 0x58 = BMP280
uint8_t leseChipId(TwoWire &bus, uint8_t addr) {
  bus.beginTransmission(addr);
  bus.write(0xD0);
  if (bus.endTransmission() != 0) return 0;
  if (bus.requestFrom((int)addr, 1) != 1) return 0;
  return bus.read();
}

bool sucheSensor() {
  for (auto &k : kandidaten) {
    if (k.initNoetig) {
      // Ohne end() bleibt die alte Pinzuordnung in der GPIO-Matrix haengen
      // und spaetere Kandidaten melden faelschlich einen Treffer.
      if (wire1Offen) { Wire1.end(); delay(5); }
      if (!Wire1.begin(k.sda, k.scl, 100000)) continue;
      wire1Offen = true;
      delay(5);
    }

    for (uint8_t addr : { (uint8_t)0x76, (uint8_t)0x77 }) {
      uint8_t id = leseChipId(*k.bus, addr);
      if (id == 0) continue;

      Serial.printf("  0x%02X auf %s antwortet, Chip-ID 0x%02X\n", addr, k.beschreibung, id);

      if (id == 0x60 && bme.begin(addr, k.bus)) {
        bme.setSampling(Adafruit_BME280::MODE_NORMAL,
                        Adafruit_BME280::SAMPLING_X2,
                        Adafruit_BME280::SAMPLING_X16,
                        Adafruit_BME280::SAMPLING_X1,
                        Adafruit_BME280::FILTER_X16,
                        Adafruit_BME280::STANDBY_MS_500);
        typ = TYP_BME280;
      } else if (id == 0x58) {
        Adafruit_BMP280 *kandidat = (k.bus == &Wire) ? &bmpWire0 : &bmpWire1;
        if (!kandidat->begin(addr)) continue;
        kandidat->setSampling(Adafruit_BMP280::MODE_NORMAL,
                              Adafruit_BMP280::SAMPLING_X2,
                              Adafruit_BMP280::SAMPLING_X16,
                              Adafruit_BMP280::FILTER_X16,
                              Adafruit_BMP280::STANDBY_MS_500);
        bmp = kandidat;
        typ = TYP_BMP280;
      } else {
        continue;
      }

      sensorBus  = k.beschreibung;
      sensorAddr = addr;
      Serial.printf("  -> %s bereit auf 0x%02X (%s)\n",
                    typ == TYP_BME280 ? "BME280" : "BMP280", addr, k.beschreibung);
      if (typ == TYP_BMP280)
        Serial.println("     BMP280: Temperatur und Druck, keine Luftfeuchte.");
      if (k.sda == 21 || k.scl == 21)
        Serial.println("     Hinweis: GPIO21 ist zugleich der Vext-Steuerpin.");
      return true;
    }
  }
  return false;
}

void zeigeWarte() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);
  u8g2.drawStr(0, 12, "Kein Sensor gefunden");
  u8g2.drawHLine(0, 16, 128);
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(0, 30, "Gesucht auf Pins:");
  u8g2.drawStr(4, 41, "4/15  21/22  23/22  22/23");
  u8g2.drawStr(0, 54, "Adressen 0x76 / 0x77");
  u8g2.sendBuffer();
}

void zeigeDaten(float t, float p, float h, bool hatFeuchte) {
  char buf[28];
  u8g2.clearBuffer();

  u8g2.setFont(u8g2_font_helvB14_tf);
  snprintf(buf, sizeof(buf), "%.1f \xb0""C", t);
  u8g2.drawUTF8((128 - u8g2.getUTF8Width(buf)) / 2, 21, buf);

  u8g2.drawHLine(6, 29, 116);

  float qnh = aufMeereshoehe(p, HOEHE_UEBER_NN);

  u8g2.setFont(u8g2_font_6x12_tf);
  if (hatFeuchte) snprintf(buf, sizeof(buf), "Feuchte  %.1f %%", h);
  else            snprintf(buf, sizeof(buf), "QNH  %.1f hPa", qnh);
  u8g2.drawUTF8((128 - u8g2.getUTF8Width(buf)) / 2, 44, buf);

  u8g2.setFont(u8g2_font_5x7_tf);
  if (hatFeuchte) snprintf(buf, sizeof(buf), "QNH %.1f  abs %.1f hPa", qnh, p);
  else            snprintf(buf, sizeof(buf), "abs %.1f hPa   %.0f m ueNN", p, HOEHE_UEBER_NN);
  u8g2.drawUTF8((128 - u8g2.getUTF8Width(buf)) / 2, 60, buf);

  u8g2.sendBuffer();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Umweltdaten BME280 / BMP280 ===");

  pinMode(LED_BUILTIN, OUTPUT);

  pinMode(RST_OLED, OUTPUT);
  digitalWrite(RST_OLED, LOW);  delay(20);
  digitalWrite(RST_OLED, HIGH); delay(20);

  u8g2.begin();
  u8g2.setContrast(255);

  Serial.println("Suche Sensor...");
  if (!sucheSensor()) Serial.println("  nichts gefunden - Neuversuch alle 2s");
}

void loop() {
  static uint32_t letzteSuche = 0;

  if (typ == KEINER) {
    zeigeWarte();
    if (millis() - letzteSuche > 2000) {
      letzteSuche = millis();
      sucheSensor();
    }
    delay(200);
    return;
  }

  float t, p, h = NAN;
  bool  hatFeuchte = (typ == TYP_BME280);

  if (hatFeuchte) {
    t = bme.readTemperature();
    p = bme.readPressure() / 100.0f;
    h = bme.readHumidity();
  } else {
    t = bmp->readTemperature();
    p = bmp->readPressure() / 100.0f;
  }

  zeigeDaten(t, p, h, hatFeuchte);

  float qnh = aufMeereshoehe(p, HOEHE_UEBER_NN);
  if (hatFeuchte) Serial.printf("%.2f C   abs %.2f hPa   QNH %.2f hPa   %.1f %%rF   [0x%02X, %s]\n",
                                t, p, qnh, h, sensorAddr, sensorBus);
  else            Serial.printf("%.2f C   abs %.2f hPa   QNH %.2f hPa   [0x%02X, %s]\n",
                                t, p, qnh, sensorAddr, sensorBus);

  digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
  delay(2000);
}
