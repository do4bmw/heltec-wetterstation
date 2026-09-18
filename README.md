# Heltec ESP32 — Wetterstation und Werkzeuge

Arduino-Sketches für Heltec-ESP32-Boards: eine Wetterstation mit Webinterface
und Messwertverlauf, dazu ein paar Diagnose-Sketches, die beim Identifizieren
unbekannter Boards und Sensoren helfen.

Entstanden auf einem **Heltec WiFi LoRa 32 (V2)** mit einem BMP280 am I2C-Bus.

## Sketches

| Ordner | Zweck |
|---|---|
| `Wetter_Web/` | Wetterstation: OLED-Anzeige, Webinterface, JSON-API, 12-Stunden-Verlauf im Dateisystem |
| `BME280_Display/` | Schlanke Variante ohne Netz: Messwerte nur auf dem OLED |
| `I2C_Pinsuche/` | Probiert alle freien Pin-Paare als I2C durch und findet unbekannt verdrahtete Sensoren |
| `Probe/` | Board-Erkennung: I2C-Scan, SX127x-Test, Display-Geometrie |
| `HalloWelt_LoRa32/` | Minimalbeispiel für 128×64-OLED |
| `HalloWelt/` | Minimalbeispiel für 64×32-OLED (Wireless Stick) |

## Wetterstation

Auf dem OLED wechseln sich drei Seiten im Fünf-Sekunden-Takt ab: aktuelle
Messwerte, Temperaturverlauf als Sparkline, Netzwerkinfo mit Uhrzeit.

Im Browser unter `http://heltec-wetter.local/` (oder der IP) gibt es
Temperatur, Luftdruck und zwei Verlaufsdiagramme. Die Diagramme werden als
SVG direkt im Browser gezeichnet — keine externe Bibliothek, das Board
braucht keinen Internetzugang.

### Messwerte

Der Sketch erkennt **BME280** und **BMP280** automatisch über das Chip-ID-Register
(`0xD0`: `0x60` = BME280, `0x58` = BMP280). Der BMP280 liefert keine Luftfeuchte;
die Anzeige passt sich entsprechend an.

Der Luftdruck wird zusätzlich auf Meereshöhe reduziert (QNH), damit er mit dem
Wetterbericht vergleichbar ist. Die Standorthöhe steht oben im Sketch:

```cpp
static const float HOEHE_UEBER_NN = 380.0f;
```

Jeder Meter verschiebt das QNH um etwa 0,12 hPa.

### Verlauf

720 Punkte à 60 Sekunden = 12 Stunden, als Ringpuffer im RAM. Gemessen wird alle
2 Sekunden, pro Minute wandert ein gemittelter Punkt in den Puffer.

Jede Minute wird der Puffer nach LittleFS geschrieben — erst in eine temporäre
Datei, die dann umbenannt wird, damit bei einem Stromausfall mitten im Schreiben
die alte Datei heil bleibt. Nach einem Neustart geht damit höchstens ein Punkt
verloren.

Ein Punkt kostet 8 Byte, der ganze Verlauf also knapp 5,8 kB. Die
`spiffs`-Partition bietet bei `default_8MB` 1,5 MB — längere Zeiträume oder eine
feinere Auflösung sind problemlos möglich:

```cpp
static const uint16_t VERLAUF_PUNKTE  = 720;  // Anzahl Punkte
static const uint32_t PUNKT_INTERVALL = 60;   // Sekunden pro Punkt
```

Die Zeitachse kommt per NTP inklusive Sommerzeitregel für Berlin. Solange keine
gültige Zeit vorliegt, pausiert die Aufzeichnung bewusst, statt falsch
gestempelte Punkte zu sammeln. Lücken durch Neustarts stellt das Diagramm als
Linienbruch dar und nicht als erfundene Verbindungslinie.

### Schnittstellen

| Pfad | Inhalt |
|---|---|
| `/` | Webseite |
| `/api` | aktuelle Messwerte, Netz- und Zeitstatus als JSON |
| `/verlauf` | Historie als JSON, stückweise gesendet |

```json
{"temperatur":25.09,"druck_absolut":972.72,"qnh":1018.18,"feuchte":null,
 "hat_feuchte":false,"sensor":"BMP280","adresse":118,"i2c":"SDA=21 SCL=22",
 "hoehe_m":380,"laufzeit_s":420,"alter_s":1,"ssid":"...","rssi":-64,
 "ipv4":"...","ipv6":"...","zeit_text":"18.09.2026 19:57:14",
 "zeit_unix":1789753034,"zeit_ok":true,"ntp":"synchron","ntp_alter_s":132}
```

### Netz und Zeit

Das Board holt seine Adresse per DHCP und meldet sich zusätzlich per mDNS, ist
also ohne bekannte IP unter `heltec-wetter.local` erreichbar. **IPv6** ist
aktiv: Die globale Adresse kommt per SLAAC, der Webserver bindet auf `::` und
nimmt damit beide Protokolle an. Im Browser gehört eine IPv6-Adresse in eckige
Klammern: `http://[2a00:...]/`.

Die Seite zeigt Uhrzeit und NTP-Status an. Die Uhrzeit kommt dabei vom Board
und nicht aus der Browser-Zeit — nur so ist erkennbar, ob Zeitzone und
Sommerzeitregel auf dem ESP32 stimmen. Der NTP-Status nennt auch das Alter des
letzten Abgleichs: Verliert das Board den Zeitserver, läuft die Uhr weiter und
sieht plausibel aus, während das Alter unbemerkt wächst.

Ein Fallstrick beim Abfragen der IPv6-Adresse: `IPAddress` vergleicht auch den
Adresstyp, deshalb ist `globalIPv6() == IPAddress((uint32_t)0)` **immer** falsch
(IPv6 gegen IPv4-Null). Zuverlässig ist der Textvergleich gegen `"::"`. Die
globale Adresse steht je nach Router nach 1 bis 6 Sekunden bereit — die
Warteschleife sollte entsprechend großzügig bemessen sein.

Beim Start prüft das Board zusätzlich, ob es über die IPv6-**Standardroute**
nach draußen kommt: ein TCP-Connect auf `[2606:4700:4700::1111]:53`. Ein Ziel
im eigenen `/64` würde nur Neighbor Discovery belegen und das Gateway gar nicht
beanspruchen — deshalb eine Adresse ausserhalb. Das Ergebnis steht in `/api`
(`ipv6_extern`) und auf der Seite. Der Test läuft bewusst nur einmal beim Start:
Ein blockierender Verbindungsversuch im laufenden Betrieb würde den Webserver
für bis zu vier Sekunden anhalten. Das Alter des Tests wird deshalb mit
ausgewiesen.

## Verdrahtung

Der Sensor wird automatisch gesucht — auf dem OLED-Bus und auf drei weiteren
Pin-Paaren, jeweils an `0x76` und `0x77`. Getestet wurde:

```
Heltec WiFi LoRa 32 V2        BMP280 / BME280
  3V3  ──────────────────────  VIN
  GND  ──────────────────────  GND
  GPIO21 ────────────────────  SDA
  GPIO22 ────────────────────  SCL
```

**Achtung:** GPIO21 ist auf Heltec-Boards zugleich der Vext-Steuerpin. Der Sketch
fasst Vext deshalb nicht an. Auf dem getesteten V2-Board arbeitet das OLED ohne
Vext-Steuerung; falls dein Display dunkel bleibt, SDA auf GPIO23 umlegen.

Das OLED hängt fest an SDA=4, SCL=15, RST=16.

## Bauen und Flashen

```bash
arduino-cli core install esp32:esp32
arduino-cli lib install U8g2 "Adafruit BME280 Library" "Adafruit BMP280 Library"

cp Wetter_Web/geheim.h.beispiel Wetter_Web/geheim.h
# Zugangsdaten eintragen

arduino-cli compile --fqbn esp32:esp32:heltec_wifi_lora_32_V2 Wetter_Web
arduino-cli upload -p COM6 --fqbn esp32:esp32:heltec_wifi_lora_32_V2 Wetter_Web
```

FQBN je nach Board: `heltec_wifi_lora_32_V2`, `heltec_wifi_lora_32` (V1) oder
`heltec_wireless_stick`.

## Sonstiges

Die weiße LED blitzt 10 ms nach jeder Messung als Lebenszeichen — bewusst ohne
`delay()`, damit der Webserver dabei bedient wird. Dauer über `BLITZ_DAUER`.

## Noch offen

* MQTT ist vorbereitet, aber nicht aktiv. Alle Messwerte laufen durch eine
  `Messwerte`-Struktur, aus der Display, Webseite und API lesen — eine
  `mqttSenden()` würde aus derselben Quelle publizieren.
* Ohne echten BME280 fehlt die Luftfeuchte. Ein AHT20 oder SHT31 am selben Bus
  könnte sie ergänzen, ohne den BMP280 zu ersetzen.
