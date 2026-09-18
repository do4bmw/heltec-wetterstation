// Wetterstation mit Webinterface - Heltec WiFi LoRa 32 (V2)
// BMP280/BME280 am I2C, Anzeige auf dem OLED, Webseite im WLAN.
//
// Erreichbar per IPv4, IPv6 und als http://heltec-wetter.local/
// JSON-Schnittstellen: /api (aktuelle Werte), /verlauf (12-Stunden-Historie)
//
// Der Verlauf liegt als Ringpuffer im RAM und wird jede Minute auf das
// LittleFS-Dateisystem geschrieben, damit er einen Neustart uebersteht.

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <time.h>
#include <esp_sntp.h>
#include <U8g2lib.h>
#include <Adafruit_BME280.h>
#include <Adafruit_BMP280.h>

#include "geheim.h"

static const float    HOEHE_UEBER_NN = 380.0f;   // Meter ueber NN, fuer QNH
static const uint32_t MESSINTERVALL  = 2000;     // ms
static const uint32_t SEITENWECHSEL  = 5000;     // ms, Display
static const uint32_t BLITZ_DAUER    = 10;       // ms, LED-Blitz nach jeder Messung

// Verlauf: 720 Punkte a 60 s = 12 Stunden. Ein Punkt kostet 8 Byte,
// der komplette Puffer also knapp 5,8 kB - im Dateisystem (1,5 MB) waeren
// auch Wochen drin, hier reicht der gewuenschte Halbtag.
static const uint16_t VERLAUF_PUNKTE     = 720;
static const uint32_t PUNKT_INTERVALL    = 60;      // Sekunden pro Punkt
static const uint32_t SPEICHER_INTERVALL = 60000;   // ms, jede Minute sichern
static const char     VERLAUF_DATEI[]    = "/verlauf.bin";
static const char     VERLAUF_TEMP[]     = "/verlauf.tmp";

// Zeitzone Europa/Berlin inkl. Sommerzeitregel
static const char TZ_BERLIN[] = "CET-1CEST,M3.5.0,M10.5.0/3";

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, RST_OLED, SCL_OLED, SDA_OLED);
WebServer server(80);

Adafruit_BME280  bme;
Adafruit_BMP280  bmpWire0(&Wire);
Adafruit_BMP280  bmpWire1(&Wire1);
Adafruit_BMP280 *bmp = nullptr;

// ---------------------------------------------------------------- Sensor

struct BusKandidat {
  TwoWire    *bus;
  int         sda, scl;
  bool        initNoetig;
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

struct Messwerte {
  bool  gueltig    = false;
  float temperatur = NAN;
  float druckAbs   = NAN;
  float qnh        = NAN;
  float feuchte    = NAN;
  bool  hatFeuchte = false;
} messwerte;

float aufMeereshoehe(float druckAbsolut, float hoehe) {
  return druckAbsolut / powf(1.0f - (hoehe / 44330.0f), 5.255f);
}

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
        bme.setSampling(Adafruit_BME280::MODE_NORMAL, Adafruit_BME280::SAMPLING_X2,
                        Adafruit_BME280::SAMPLING_X16, Adafruit_BME280::SAMPLING_X1,
                        Adafruit_BME280::FILTER_X16, Adafruit_BME280::STANDBY_MS_500);
        typ = TYP_BME280;
      } else if (id == 0x58) {
        Adafruit_BMP280 *kandidat = (k.bus == &Wire) ? &bmpWire0 : &bmpWire1;
        if (!kandidat->begin(addr)) continue;
        kandidat->setSampling(Adafruit_BMP280::MODE_NORMAL, Adafruit_BMP280::SAMPLING_X2,
                              Adafruit_BMP280::SAMPLING_X16, Adafruit_BMP280::FILTER_X16,
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
      return true;
    }
  }
  return false;
}

const char *sensorName() {
  switch (typ) {
    case TYP_BME280: return "BME280";
    case TYP_BMP280: return "BMP280";
    default:         return "keiner";
  }
}

void messen() {
  if (typ == KEINER) { messwerte.gueltig = false; return; }

  if (typ == TYP_BME280) {
    messwerte.temperatur = bme.readTemperature();
    messwerte.druckAbs   = bme.readPressure() / 100.0f;
    messwerte.feuchte    = bme.readHumidity();
    messwerte.hatFeuchte = true;
  } else {
    messwerte.temperatur = bmp->readTemperature();
    messwerte.druckAbs   = bmp->readPressure() / 100.0f;
    messwerte.feuchte    = NAN;
    messwerte.hatFeuchte = false;
  }
  messwerte.qnh     = aufMeereshoehe(messwerte.druckAbs, HOEHE_UEBER_NN);
  messwerte.gueltig = true;
}

// ---------------------------------------------------------------- Verlauf

struct Punkt {
  uint32_t zeit;   // Unix-Zeit des Minutenslots
  int16_t  temp;   // 0.01 Grad C
  uint16_t qnh;    // 0.1 hPa
};

Punkt    verlauf[VERLAUF_PUNKTE];
uint16_t verlaufKopf   = 0;    // naechster Schreibindex
uint16_t verlaufAnzahl = 0;
bool     fsBereit      = false;
bool     verlaufSchmutzig = false;

// Mittelung ueber die laufende Minute
double   summeTemp = 0, summeQnh = 0;
uint16_t proben    = 0;
uint32_t aktuellerSlot = 0;

bool zeitGueltig() { return time(nullptr) > 1700000000; }  // ab Nov 2023

// ---------------------------------------------------------------- NTP-Status

volatile uint32_t letzterAbgleich = 0;   // Unix-Zeit des letzten NTP-Syncs

void ntpAbgeglichen(struct timeval *tv) {
  letzterAbgleich = (uint32_t)tv->tv_sec;
  Serial.println("NTP: Zeit abgeglichen");
}

// sntp_get_sync_status() faellt nach dem Setzen der Zeit sofort wieder auf
// RESET zurueck und taugt daher nicht als Dauerzustand. Massgeblich ist,
// ob der Sync-Callback schon einmal gefeuert hat.
const char *ntpStatus() {
  if (sntp_get_sync_status() == SNTP_SYNC_STATUS_IN_PROGRESS) return "Abgleich laeuft";
  if (letzterAbgleich)                                        return "synchron";
  if (zeitGueltig())                                          return "Zeit gesetzt, kein Abgleich";
  return "keine Antwort";
}

// Lokale Zeit des Boards als Text - nutzt die eingestellte Zeitzone,
// ist also unabhaengig davon, wo der Browser steht.
void zeitText(char *ziel, size_t n) {
  if (!zeitGueltig()) { snprintf(ziel, n, "nicht gesetzt"); return; }
  time_t    t = time(nullptr);
  struct tm lt;
  localtime_r(&t, &lt);
  strftime(ziel, n, "%d.%m.%Y %H:%M:%S", &lt);
}

void verlaufAnhaengen(uint32_t zeit, float temp, float qnh) {
  verlauf[verlaufKopf].zeit = zeit;
  verlauf[verlaufKopf].temp = (int16_t)lroundf(temp * 100.0f);
  verlauf[verlaufKopf].qnh  = (uint16_t)lroundf(qnh * 10.0f);
  verlaufKopf = (verlaufKopf + 1) % VERLAUF_PUNKTE;
  if (verlaufAnzahl < VERLAUF_PUNKTE) verlaufAnzahl++;
  verlaufSchmutzig = true;
}

// Sammelt Messwerte und legt pro Minute genau einen gemittelten Punkt ab
void verlaufFuettern() {
  if (!messwerte.gueltig || !zeitGueltig()) return;

  uint32_t jetzt = (uint32_t)time(nullptr);
  uint32_t slot  = jetzt / PUNKT_INTERVALL;

  if (aktuellerSlot == 0) aktuellerSlot = slot;

  if (slot != aktuellerSlot) {
    if (proben > 0) {
      verlaufAnhaengen(aktuellerSlot * PUNKT_INTERVALL,
                       (float)(summeTemp / proben), (float)(summeQnh / proben));
    }
    summeTemp = summeQnh = 0;
    proben    = 0;
    aktuellerSlot = slot;
  }

  summeTemp += messwerte.temperatur;
  summeQnh  += messwerte.qnh;
  proben++;
}

struct DateiKopf {
  char     magie[4];   // "WV02"
  uint16_t anzahl;
  uint16_t kopf;
};

bool speichereVerlauf() {
  if (!fsBereit) return false;

  File f = LittleFS.open(VERLAUF_TEMP, "w");
  if (!f) { Serial.println("Verlauf: Datei nicht schreibbar"); return false; }

  DateiKopf k = { { 'W', 'V', '0', '2' }, verlaufAnzahl, verlaufKopf };
  bool ok = f.write((uint8_t *)&k, sizeof(k)) == sizeof(k);
  ok = ok && f.write((uint8_t *)verlauf, sizeof(verlauf)) == sizeof(verlauf);
  f.close();

  if (!ok) { LittleFS.remove(VERLAUF_TEMP); return false; }

  // Erst schreiben, dann tauschen - so bleibt bei Stromausfall die alte
  // Datei heil statt halb ueberschrieben zurueckzubleiben.
  LittleFS.remove(VERLAUF_DATEI);
  if (!LittleFS.rename(VERLAUF_TEMP, VERLAUF_DATEI)) return false;

  verlaufSchmutzig = false;
  return true;
}

bool ladeVerlauf() {
  if (!fsBereit || !LittleFS.exists(VERLAUF_DATEI)) return false;

  File f = LittleFS.open(VERLAUF_DATEI, "r");
  if (!f) return false;

  DateiKopf k;
  bool ok = f.read((uint8_t *)&k, sizeof(k)) == sizeof(k);
  ok = ok && memcmp(k.magie, "WV02", 4) == 0;
  ok = ok && k.anzahl <= VERLAUF_PUNKTE && k.kopf < VERLAUF_PUNKTE;
  ok = ok && f.read((uint8_t *)verlauf, sizeof(verlauf)) == sizeof(verlauf);
  f.close();

  if (!ok) { Serial.println("Verlauf: Datei unbrauchbar, wird verworfen"); return false; }

  verlaufAnzahl = k.anzahl;
  verlaufKopf   = k.kopf;
  Serial.printf("Verlauf: %u Punkte aus dem Dateisystem geladen\n", verlaufAnzahl);
  return true;
}

// Index des i-ten Punktes in chronologischer Reihenfolge (0 = aeltester)
uint16_t verlaufIndex(uint16_t i) {
  return (verlaufKopf + VERLAUF_PUNKTE - verlaufAnzahl + i) % VERLAUF_PUNKTE;
}

// ---------------------------------------------------------------- Display

void zeigeMesswerte() {
  char buf[36];
  u8g2.clearBuffer();

  if (!messwerte.gueltig) {
    u8g2.setFont(u8g2_font_6x12_tf);
    u8g2.drawStr(0, 12, "Kein Sensor gefunden");
    u8g2.drawHLine(0, 16, 128);
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 32, "Gesucht auf Pins:");
    u8g2.drawStr(4, 43, "4/15  21/22  23/22  22/23");
    u8g2.sendBuffer();
    return;
  }

  u8g2.setFont(u8g2_font_helvB14_tf);
  snprintf(buf, sizeof(buf), "%.1f \xb0""C", messwerte.temperatur);
  u8g2.drawUTF8((128 - u8g2.getUTF8Width(buf)) / 2, 21, buf);

  u8g2.drawHLine(6, 29, 116);

  u8g2.setFont(u8g2_font_6x12_tf);
  if (messwerte.hatFeuchte) snprintf(buf, sizeof(buf), "Feuchte  %.1f %%", messwerte.feuchte);
  else                      snprintf(buf, sizeof(buf), "QNH  %.1f hPa", messwerte.qnh);
  u8g2.drawUTF8((128 - u8g2.getUTF8Width(buf)) / 2, 44, buf);

  u8g2.setFont(u8g2_font_5x7_tf);
  if (messwerte.hatFeuchte)
    snprintf(buf, sizeof(buf), "QNH %.1f  abs %.1f", messwerte.qnh, messwerte.druckAbs);
  else
    snprintf(buf, sizeof(buf), "abs %.1f hPa  %.0f m", messwerte.druckAbs, HOEHE_UEBER_NN);
  u8g2.drawUTF8((128 - u8g2.getUTF8Width(buf)) / 2, 60, buf);

  u8g2.sendBuffer();
}

void zeigeNetz() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tf);

  if (WiFi.status() != WL_CONNECTED) {
    u8g2.drawStr(0, 12, "WLAN");
    u8g2.drawHLine(0, 16, 128);
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 30, "verbinde mit");
    u8g2.drawStr(0, 40, WLAN_SSID);
    u8g2.sendBuffer();
    return;
  }

  char buf[48];
  snprintf(buf, sizeof(buf), "%s  %d dBm", WLAN_SSID, WiFi.RSSI());
  u8g2.drawStr(0, 11, buf);
  u8g2.drawHLine(0, 15, 128);

  u8g2.drawStr(0, 31, WiFi.localIP().toString().c_str());

  u8g2.setFont(u8g2_font_5x7_tf);
  if (zeitGueltig()) {
    time_t    t = time(nullptr);
    struct tm lt;
    localtime_r(&t, &lt);
    strftime(buf, sizeof(buf), "%d.%m.%Y  %H:%M", &lt);
    u8g2.drawStr(0, 45, buf);
  } else {
    u8g2.drawStr(0, 45, "Zeit noch nicht gesetzt");
  }

  snprintf(buf, sizeof(buf), "%s.local", HOSTNAME);
  u8g2.drawStr(0, 59, buf);

  u8g2.sendBuffer();
}

// Kleiner Temperaturverlauf auf dem OLED
void zeigeVerlauf() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tf);

  if (verlaufAnzahl < 2) {
    u8g2.drawStr(0, 12, "Verlauf Temperatur");
    u8g2.drawHLine(0, 16, 128);
    u8g2.drawStr(0, 34, "sammle noch Daten...");
    char b[32];
    snprintf(b, sizeof(b), "%u von %u Punkten", verlaufAnzahl, VERLAUF_PUNKTE);
    u8g2.drawStr(0, 46, b);
    u8g2.sendBuffer();
    return;
  }

  int16_t tMin = INT16_MAX, tMax = INT16_MIN;
  for (uint16_t i = 0; i < verlaufAnzahl; i++) {
    int16_t v = verlauf[verlaufIndex(i)].temp;
    if (v < tMin) tMin = v;
    if (v > tMax) tMax = v;
  }
  if (tMax - tMin < 50) {            // mindestens 0,5 Grad Spanne zeigen
    int16_t mitte = (tMin + tMax) / 2;
    tMin = mitte - 25;
    tMax = mitte + 25;
  }

  char b[24];
  snprintf(b, sizeof(b), "Temp %.1f bis %.1f C", tMin / 100.0f, tMax / 100.0f);
  u8g2.drawStr(0, 7, b);

  // Zeichenflaeche: x 0..127, y 12..54
  const int yOben = 12, yUnten = 54;
  u8g2.drawHLine(0, yUnten + 1, 128);

  int letztesX = -1, letztesY = -1;
  for (uint16_t i = 0; i < verlaufAnzahl; i++) {
    int x = (verlaufAnzahl == 1) ? 127 : (int)((long)i * 127 / (verlaufAnzahl - 1));
    int16_t v = verlauf[verlaufIndex(i)].temp;
    int y = yUnten - (int)((long)(v - tMin) * (yUnten - yOben) / (tMax - tMin));
    if (letztesX >= 0) u8g2.drawLine(letztesX, letztesY, x, y);
    else               u8g2.drawPixel(x, y);
    letztesX = x;
    letztesY = y;
  }

  float stunden = verlaufAnzahl * PUNKT_INTERVALL / 3600.0f;
  snprintf(b, sizeof(b), "letzte %.1f h", stunden);
  u8g2.drawStr(0, 63, b);
  snprintf(b, sizeof(b), "%.1f C", verlauf[verlaufIndex(verlaufAnzahl - 1)].temp / 100.0f);
  u8g2.drawStr(128 - u8g2.getStrWidth(b), 63, b);

  u8g2.sendBuffer();
}

// ---------------------------------------------------------------- Web

const char SEITE[] PROGMEM = R"HTML(<!doctype html>
<html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Wetterstation</title>
<style>
:root{--bg:#f4f5f7;--card:#fff;--fg:#1b1d21;--dim:#6b7280;--line:#e3e5e9;
--akz:#2563eb;--warm:#dc6803;--gitter:#eceef1}
@media(prefers-color-scheme:dark){:root{--bg:#15171a;--card:#1e2126;--fg:#e8eaed;
--dim:#9aa1ab;--line:#2c3036;--akz:#60a5fa;--warm:#f59e0b;--gitter:#282c32}}
*{box-sizing:border-box}
body{margin:0;padding:24px 16px;background:var(--bg);color:var(--fg);
font:15px/1.5 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
.wrap{max-width:620px;margin:0 auto}
h1{font-size:20px;margin:0 0 4px}
h2{font-size:13px;margin:0 0 2px;font-weight:600}
.sub{color:var(--dim);font-size:13px;margin-bottom:20px}
.gross{background:var(--card);border:1px solid var(--line);border-radius:12px;
padding:24px;text-align:center;margin-bottom:12px}
.temp{font-size:52px;font-weight:600;letter-spacing:-1px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-bottom:12px}
.karte{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:16px}
.label{color:var(--dim);font-size:12px;text-transform:uppercase;letter-spacing:.5px}
.wert{font-size:24px;font-weight:600;margin-top:4px}
.einheit{font-size:14px;font-weight:400;color:var(--dim)}
.spanne{color:var(--dim);font-size:12px;margin-bottom:8px}
svg{width:100%;height:auto;display:block;overflow:visible}
.gl{stroke:var(--gitter);stroke-width:1}
.ax{fill:var(--dim);font-size:9px}
table{width:100%;border-collapse:collapse;background:var(--card);
border:1px solid var(--line);border-radius:12px;overflow:hidden}
td{padding:9px 16px;border-bottom:1px solid var(--line);font-size:13px}
tr:last-child td{border-bottom:none}
td:first-child{color:var(--dim);width:42%}
td:last-child{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;word-break:break-all}
.fuss{color:var(--dim);font-size:12px;margin-top:16px;text-align:center}
a{color:var(--akz)}
</style></head><body><div class="wrap">
<h1>Wetterstation</h1>
<div class="sub" id="sub">&nbsp;</div>
<div class="gross"><div class="temp"><span id="t">--</span>&thinsp;&deg;C</div></div>
<div class="grid">
<div class="karte"><div class="label">Luftdruck QNH</div>
<div class="wert"><span id="q">--</span> <span class="einheit">hPa</span></div></div>
<div class="karte"><div class="label" id="l2">Luftdruck absolut</div>
<div class="wert"><span id="p">--</span> <span class="einheit" id="e2">hPa</span></div></div>
</div>
<div class="karte" style="margin-bottom:12px">
<h2>Temperatur</h2><div class="spanne" id="st">&nbsp;</div><div id="ct"></div></div>
<div class="karte" style="margin-bottom:12px">
<h2>Luftdruck QNH</h2><div class="spanne" id="sq">&nbsp;</div><div id="cq"></div></div>
<table>
<tr><td>Uhrzeit (Board)</td><td id="zt">--</td></tr>
<tr><td>NTP</td><td id="nt">--</td></tr>
<tr><td>Sensor</td><td id="s">--</td></tr>
<tr><td>I2C</td><td id="i2c">--</td></tr>
<tr><td>Standorth&ouml;he</td><td id="h">--</td></tr>
<tr><td>IPv4</td><td id="ip4">--</td></tr>
<tr><td>IPv6</td><td id="ip6">--</td></tr>
<tr><td>IPv6 nach au&szlig;en</td><td id="v6x">--</td></tr>
<tr><td>WLAN</td><td id="w">--</td></tr>
<tr><td>Laufzeit</td><td id="up">--</td></tr>
<tr><td>Messpunkte</td><td id="np">--</td></tr>
</table>
<div class="fuss">Werte alle 5&nbsp;s, Verlauf alle 60&nbsp;s &middot;
<a href="/api">/api</a> &middot; <a href="/verlauf">/verlauf</a></div>
</div><script>
const g=i=>document.getElementById(i);
function zeit(s){const t=Math.floor(s/86400),h=Math.floor(s%86400/3600),
m=Math.floor(s%3600/60);return(t?t+" d ":"")+(h||t?h+" h ":"")+m+" min"}
function kurz(s){return s<60?s+" s":zeit(s)}
function uhr(u){const d=new Date(u*1000);
return String(d.getHours()).padStart(2,"0")+":"+String(d.getMinutes()).padStart(2,"0")}

// Zeichnet einen Linienverlauf als SVG. x-Achse ist Unix-Zeit.
function chart(ziel,zeiten,werte,farbe,dez,einheit,raster){
 const W=600,H=170,lk=44,rk=8,ok=10,uk=28;
 if(!werte.length){g(ziel).innerHTML='<div style="color:var(--dim);font-size:13px;'+
 'padding:18px 0">Noch keine Daten &ndash; der Verlauf f&uuml;llt sich.</div>';return null}
 let mn=Math.min(...werte),mx=Math.max(...werte);
 const spanne=mx-mn, luft=spanne<1e-9?1:spanne*0.12;
 mn-=luft;mx+=luft;
 const t0=zeiten[0],t1=zeiten[zeiten.length-1],dt=Math.max(1,t1-t0);
 const px=t=>lk+(t-t0)/dt*(W-lk-rk);
 const py=v=>ok+(mx-v)/(mx-mn)*(H-ok-uk);
 let s='<svg viewBox="0 0 '+W+' '+H+'" preserveAspectRatio="none" role="img">';
 for(let i=0;i<=4;i++){const v=mn+(mx-mn)*i/4,y=py(v);
  s+='<line class="gl" x1="'+lk+'" y1="'+y+'" x2="'+(W-rk)+'" y2="'+y+'"/>';
  s+='<text class="ax" x="'+(lk-6)+'" y="'+(y+3)+'" text-anchor="end">'+v.toFixed(dez)+'</text>'}
 const schritt=Math.max(1,Math.round(dt/4));
 for(let i=0;i<=4;i++){const t=t0+schritt*i;if(t>t1+30)break;const x=px(t);
  s+='<text class="ax" x="'+x+'" y="'+(H-4)+'" text-anchor="middle">'+uhr(t)+'</text>'}
 // Bei Luecken (Neustart, Stromausfall) die Linie unterbrechen, statt
 // quer durch den Zeitraum zu ziehen und Messwerte vorzutaeuschen.
 const luecke=Math.max(2,raster*2.5);
 let d="",neu=true,insel=[];
 for(let i=0;i<werte.length;i++){
  if(i&&zeiten[i]-zeiten[i-1]>luecke){
   if(neu)insel.push(i-1);
   neu=true}
  d+=(neu?"M":"L")+px(zeiten[i]).toFixed(1)+" "+py(werte[i]).toFixed(1)+" ";
  neu=false}
 if(werte.length===1)insel.push(0);
 s+='<path d="'+d+'" fill="none" stroke="'+farbe+'" stroke-width="2" '+
    'stroke-linejoin="round" stroke-linecap="round"/>';
 // Einzelne Punkte ohne Nachbarn haetten sonst keine sichtbare Linie
 for(const i of insel)
  s+='<circle cx="'+px(zeiten[i]).toFixed(1)+'" cy="'+py(werte[i]).toFixed(1)+
     '" r="2" fill="'+farbe+'"/>';
 s+='</svg>';
 g(ziel).innerHTML=s;
 return{mn:Math.min(...werte),mx:Math.max(...werte),einheit:einheit,dez:dez}
}
async function ladeWerte(){try{
 const d=await(await fetch("/api",{cache:"no-store"})).json();
 g("t").textContent=d.temperatur.toFixed(1);
 g("q").textContent=d.qnh.toFixed(1);
 if(d.hat_feuchte){g("l2").textContent="Luftfeuchte";
  g("p").textContent=d.feuchte.toFixed(1);g("e2").textContent="%"}
 else g("p").textContent=d.druck_absolut.toFixed(1);
 g("zt").textContent=d.zeit_text;
 g("nt").textContent=d.ntp+(d.ntp_alter_s>=0?", letzter Abgleich vor "+kurz(d.ntp_alter_s):"");
 g("s").textContent=d.sensor+" @ 0x"+d.adresse.toString(16);
 g("i2c").textContent=d.i2c;
 g("h").textContent=d.hoehe_m+" m über NN";
 g("ip4").textContent=d.ipv4;
 g("ip6").textContent=d.ipv6||"keine Adresse";
 g("v6x").textContent=d.ipv6_extern===null?"nicht geprueft":
  (d.ipv6_extern?"erreichbar":"kein Durchkommen")+
  " — "+d.ipv6_testziel+", "+d.ipv6_extern_ms+" ms, Test vor "+kurz(d.ipv6_extern_alter_s);
 g("w").textContent=d.ssid+" ("+d.rssi+" dBm)";
 g("up").textContent=zeit(d.laufzeit_s);
 g("sub").textContent="Messung von vor "+d.alter_s+" s";
}catch(e){g("sub").textContent="Keine Verbindung zum Board"}}
async function ladeVerlauf(){try{
 const v=await(await fetch("/verlauf",{cache:"no-store"})).json();
 const zt=v.zeit.map(z=>v.von+z);
 const tm=v.temp.map(x=>x/10),qn=v.qnh.map(x=>x/10);
 const a=chart("ct",zt,tm,"var(--warm)",1,"°C",v.dt);
 const b=chart("cq",zt,qn,"var(--akz)",1,"hPa",v.dt);
 if(a)g("st").textContent=a.mn.toFixed(1)+" bis "+a.mx.toFixed(1)+" °C";
 if(b)g("sq").textContent=b.mn.toFixed(1)+" bis "+b.mx.toFixed(1)+" hPa";
 g("np").textContent=v.n+" von "+v.max+" (je "+v.dt+" s)";
}catch(e){}}
ladeWerte();ladeVerlauf();
setInterval(ladeWerte,5000);setInterval(ladeVerlauf,60000);
</script></body></html>)HTML";

uint32_t letzteMessung = 0;

// Achtung: IPAddress vergleicht auch den Adresstyp, deshalb ist
// "v6 == IPAddress((uint32_t)0)" immer falsch (IPv6 gegen IPv4-Null).
// Der Textvergleich gegen "::" ist der zuverlaessige Weg.
bool v6Gesetzt(const IPAddress &a) { return a.toString() != "::"; }

// Globale IPv6-Adresse, sonst die link-local, sonst leer
String ipv6Text() {
  IPAddress v6 = WiFi.globalIPv6();
  if (v6Gesetzt(v6)) return v6.toString();
  IPAddress ll = WiFi.linkLocalIPv6();
  if (v6Gesetzt(ll)) return ll.toString();
  return "";
}

// Prueft, ob das Board ueber die IPv6-Standardroute nach draussen kommt.
// Eine Adresse im eigenen /64 wuerde nur Neighbor Discovery belegen und das
// Gateway gar nicht beanspruchen - deshalb ein Ziel ausserhalb: ein
// TCP-Connect auf den DNS-Port von Cloudflare. Gelingt er, ist eine
// Default-Route vorhanden und in Benutzung.
static const char     IPV6_TESTZIEL[] = "2606:4700:4700::1111";
static const uint16_t IPV6_TESTPORT   = 53;

struct {
  bool     gelaufen  = false;
  bool     ok        = false;
  uint32_t dauerMs   = 0;
  uint32_t beiMillis = 0;
} v6Test;

void pruefeIpv6Route() {
  IPAddress ziel;
  if (!ziel.fromString(IPV6_TESTZIEL)) {
    Serial.println("  IPv6 raus: Zieladresse unlesbar");
    return;
  }

  NetworkClient c;
  uint32_t t0    = millis();
  bool     ok    = c.connect(ziel, IPV6_TESTPORT, 4000);
  uint32_t dauer = millis() - t0;
  c.stop();

  v6Test.gelaufen  = true;
  v6Test.ok        = ok;
  v6Test.dauerMs   = dauer;
  v6Test.beiMillis = millis();

  Serial.printf("  IPv6 raus: %s ([%s]:%u, %lu ms)\n",
                ok ? "erreichbar, Standardroute wird genutzt" : "kein Durchkommen",
                IPV6_TESTZIEL, IPV6_TESTPORT, (unsigned long)dauer);
}

void handleSeite() {
  server.send_P(200, "text/html; charset=utf-8", SEITE);
}

void handleApi() {
  char json[1120];
  char feuchte[16];
  if (messwerte.hatFeuchte) snprintf(feuchte, sizeof(feuchte), "%.1f", messwerte.feuchte);
  else                      snprintf(feuchte, sizeof(feuchte), "null");

  char zeitStr[32];
  zeitText(zeitStr, sizeof(zeitStr));

  long abgleichAlter = -1;
  if (zeitGueltig() && letzterAbgleich)
    abgleichAlter = (long)((uint32_t)time(nullptr) - letzterAbgleich);

  snprintf(json, sizeof(json),
    "{\"temperatur\":%.2f,\"druck_absolut\":%.2f,\"qnh\":%.2f,"
    "\"feuchte\":%s,\"hat_feuchte\":%s,\"sensor\":\"%s\",\"adresse\":%u,"
    "\"i2c\":\"%s\",\"hoehe_m\":%.0f,\"laufzeit_s\":%lu,\"alter_s\":%lu,"
    "\"ssid\":\"%s\",\"rssi\":%d,\"ipv4\":\"%s\",\"ipv6\":\"%s\","
    "\"ipv6_extern\":%s,\"ipv6_extern_ms\":%lu,\"ipv6_extern_alter_s\":%ld,"
    "\"ipv6_testziel\":\"%s\","
    "\"zeit_text\":\"%s\",\"zeit_unix\":%lu,\"zeit_ok\":%s,"
    "\"ntp\":\"%s\",\"ntp_alter_s\":%ld}",
    messwerte.temperatur, messwerte.druckAbs, messwerte.qnh,
    feuchte, messwerte.hatFeuchte ? "true" : "false",
    sensorName(), sensorAddr, sensorBus, HOEHE_UEBER_NN,
    (unsigned long)(millis() / 1000),
    (unsigned long)((millis() - letzteMessung) / 1000),
    WLAN_SSID, WiFi.RSSI(),
    WiFi.localIP().toString().c_str(), ipv6Text().c_str(),
    v6Test.gelaufen ? (v6Test.ok ? "true" : "false") : "null",
    (unsigned long)v6Test.dauerMs,
    v6Test.gelaufen ? (long)((millis() - v6Test.beiMillis) / 1000) : -1L,
    IPV6_TESTZIEL,
    zeitStr, (unsigned long)time(nullptr), zeitGueltig() ? "true" : "false",
    ntpStatus(), abgleichAlter);

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json; charset=utf-8", json);
}

// Der Verlauf ist mit 720 Punkten zu gross fuer einen String im RAM,
// deshalb stueckweise senden. Zeiten als Sekunden-Offset zu "von".
void handleVerlauf() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json; charset=utf-8", "");

  uint32_t von = verlaufAnzahl ? verlauf[verlaufIndex(0)].zeit : 0;

  char kopf[128];
  snprintf(kopf, sizeof(kopf),
           "{\"von\":%lu,\"n\":%u,\"max\":%u,\"dt\":%lu,\"zeit\":[",
           (unsigned long)von, verlaufAnzahl, VERLAUF_PUNKTE,
           (unsigned long)PUNKT_INTERVALL);
  server.sendContent(kopf);

  String block;
  block.reserve(1024);

  for (uint16_t i = 0; i < verlaufAnzahl; i++) {
    if (i) block += ',';
    block += (uint32_t)(verlauf[verlaufIndex(i)].zeit - von);
    if (block.length() > 900) { server.sendContent(block); block = ""; }
  }
  block += "],\"temp\":[";
  for (uint16_t i = 0; i < verlaufAnzahl; i++) {
    if (i) block += ',';
    block += (int)lroundf(verlauf[verlaufIndex(i)].temp / 10.0f);  // 0.1 Grad
    if (block.length() > 900) { server.sendContent(block); block = ""; }
  }
  block += "],\"qnh\":[";
  for (uint16_t i = 0; i < verlaufAnzahl; i++) {
    if (i) block += ',';
    block += verlauf[verlaufIndex(i)].qnh;                          // 0.1 hPa
    if (block.length() > 900) { server.sendContent(block); block = ""; }
  }
  block += "]}";
  server.sendContent(block);
  server.sendContent("");
}

// ---------------------------------------------------------------- MQTT (spaeter)
//
// Vorbereitet, aber noch nicht aktiv. Zum Nachruesten reicht:
//   arduino-cli lib install PubSubClient
// Die Messwerte liegen fertig in messwerte{} - eine mqttSenden()-Funktion
// wuerde daraus publizieren und im selben Takt wie messen() aus loop()
// aufgerufen. Broker, Topic-Praefix und Zugangsdaten gehoeren dann nach
// geheim.h, nicht hierher.

// ---------------------------------------------------------------- Setup

void wlanVerbinden() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.enableIPv6(true);            // muss vor begin() stehen
  WiFi.begin(WLAN_SSID, WLAN_PASSWORT);

  Serial.printf("Verbinde mit %s ", WLAN_SSID);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("verbunden, RSSI %d dBm\n", WiFi.RSSI());
    Serial.printf("  IPv4     : %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("  Gateway  : %s\n", WiFi.gatewayIP().toString().c_str());

    // SLAAC braucht nach dem Verbinden einen Moment, bis die globale Adresse
    // steht; die link-local ist meist sofort da.
    uint32_t v6start = millis();
    while (!v6Gesetzt(WiFi.globalIPv6()) && millis() - v6start < 10000) delay(250);
    Serial.printf("  IPv6 link: %s\n", WiFi.linkLocalIPv6().toString().c_str());
    Serial.printf("  IPv6 glob: %s (nach %lu ms)\n",
                  WiFi.globalIPv6().toString().c_str(), (unsigned long)(millis() - v6start));

    if (MDNS.begin(HOSTNAME)) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("  Name     : http://%s.local/\n", HOSTNAME);
    }

    // Uhrzeit holen - ohne sie haette der Verlauf keine brauchbare Zeitachse
    sntp_set_time_sync_notification_cb(ntpAbgeglichen);
    configTzTime(TZ_BERLIN, "de.pool.ntp.org", "europe.pool.ntp.org", "pool.ntp.org");
    Serial.print("Warte auf NTP ");
    start = millis();
    while (!zeitGueltig() && millis() - start < 10000) { delay(250); Serial.print("."); }
    Serial.println();
    if (zeitGueltig()) {
      time_t    t = time(nullptr);
      struct tm lt;
      localtime_r(&t, &lt);
      char b[40];
      strftime(b, sizeof(b), "%d.%m.%Y %H:%M:%S", &lt);
      Serial.printf("  Zeit     : %s\n", b);
    } else {
      Serial.println("  Zeit     : keine NTP-Antwort, Verlauf pausiert");
    }
  } else {
    Serial.println("WLAN-Verbindung fehlgeschlagen - wird weiter versucht");
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Wetterstation mit Webinterface ===");

  // Die weisse LED blitzt nur kurz nach jeder Messung - siehe loop().
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  pinMode(RST_OLED, OUTPUT);
  digitalWrite(RST_OLED, LOW);  delay(20);
  digitalWrite(RST_OLED, HIGH); delay(20);
  u8g2.begin();
  u8g2.setContrast(255);

  memset(verlauf, 0, sizeof(verlauf));

  fsBereit = LittleFS.begin(true);   // true = bei Bedarf formatieren
  if (fsBereit) {
    Serial.printf("Dateisystem: %u kB gesamt, %u kB belegt\n",
                  (unsigned)(LittleFS.totalBytes() / 1024),
                  (unsigned)(LittleFS.usedBytes() / 1024));
    ladeVerlauf();
  } else {
    Serial.println("Dateisystem konnte nicht eingebunden werden");
  }

  Serial.println("Suche Sensor...");
  if (sucheSensor()) messen();
  else Serial.println("  nichts gefunden - Neuversuch laeuft weiter");
  letzteMessung = millis();

  zeigeNetz();
  wlanVerbinden();
  if (WiFi.status() == WL_CONNECTED) pruefeIpv6Route();

  server.on("/",        handleSeite);
  server.on("/api",     handleApi);
  server.on("/verlauf", handleVerlauf);
  server.onNotFound([]() { server.send(404, "text/plain", "Nicht gefunden\n"); });
  server.begin();
  Serial.println("Webserver laeuft auf Port 80 (IPv4 + IPv6)");
}

// ---------------------------------------------------------------- Loop

void loop() {
  static uint32_t letzterWechsel = 0, letzteSuche = 0;
  static uint32_t letzterWlanCheck = 0, letzteSicherung = 0;
  static uint8_t  seite = 0;

  server.handleClient();

  uint32_t jetzt = millis();

  // LED-Blitz beenden. Bewusst ohne delay(), damit der Webserver
  // waehrend der 25 ms weiter bedient wird.
  static uint32_t ledAn = 0;
  if (ledAn && jetzt - ledAn >= BLITZ_DAUER) {
    digitalWrite(LED_BUILTIN, LOW);
    ledAn = 0;
  }

  if (jetzt - letzteMessung >= MESSINTERVALL) {
    letzteMessung = jetzt;
    if (typ == KEINER) {
      if (jetzt - letzteSuche >= 2000) { letzteSuche = jetzt; sucheSensor(); }
    } else {
      messen();
      verlaufFuettern();
      digitalWrite(LED_BUILTIN, HIGH);   // kurzer Blitz als Lebenszeichen
      ledAn = jetzt;
    }
  }

  if (jetzt - letzterWechsel >= SEITENWECHSEL) {
    letzterWechsel = jetzt;
    seite = (seite + 1) % 3;
    if      (seite == 0) zeigeMesswerte();
    else if (seite == 1) zeigeVerlauf();
    else                 zeigeNetz();
  }

  if (jetzt - letzteSicherung >= SPEICHER_INTERVALL) {
    letzteSicherung = jetzt;
    if (verlaufSchmutzig && speichereVerlauf())
      Serial.printf("Verlauf gesichert (%u Punkte)\n", verlaufAnzahl);
  }

  if (jetzt - letzterWlanCheck >= 15000) {
    letzterWlanCheck = jetzt;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WLAN weg - verbinde neu");
      WiFi.disconnect();
      WiFi.begin(WLAN_SSID, WLAN_PASSWORT);
    }
  }
}
