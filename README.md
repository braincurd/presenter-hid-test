# Laola - Interaktiver Sound Player (Telekom Shop Variante)

ESP32-S3-basierter Sound Player fuer den Eingangsbereich. Erkennt Personen per mmWave-Radar und spielt einen Begruessung-Sound ab. Steuerung ueber Wireless Presenter-Fernbedienung und Web-Interface (Captive Portal).

> **Telekom Shop Variante:** Diese Version spielt einen festen Sound (`0001.mp3`). Die Track-Auswahl wurde entfernt.

## Features

- **Personenerkennung** via HLK-LD2410C mmWave-Radar (kein PIR, funktioniert auch bei Stillstand)
- **MP3-Wiedergabe** ueber DFPlayer Pro (DFRobot DF1201S) mit Lautsprecher
- **Fester Sound** - spielt `0001.mp3` bei jeder Erkennung
- **Automatische Track-Erkennung** - Wiedergabedauer wird automatisch erkannt
- **Wireless Presenter** als Fernbedienung (USB HID via Dongle)
- **Captive Portal** zur Konfiguration per Smartphone
- **Persistente Einstellungen** bleiben nach Neustart erhalten
- **Anti-Fehl-Trigger** durch konfigurierbare Cooldown-Zeiten

## Hardware

| Komponente | Beschreibung |
|------------|-------------|
| ESP32-S3 DevKitC-1 (Freenove) | Mikrocontroller |
| HLK-LD2410C | 24GHz mmWave Praesenz-Sensor |
| DFPlayer Pro (DFRobot DF1201S) | MP3 Player Modul mit integriertem Verstaerker |
| Wireless Presenter (2.4GHz) | Fernbedienung mit USB-Dongle |
| Lautsprecher (4-8 Ohm) | Audio-Ausgabe |
| USB-A Buchse | Fuer Presenter-Dongle |
| microSD Karte (FAT32) | Fuer MP3-Dateien im DFPlayer Pro |

## Verkabelung

### DFPlayer Pro (DF1201S)

```
DFPlayer Pro           ESP32-S3
+--------------+
| VCC          | ---- 3.3V - 5V
| GND          | ---- GND
| RX           | ---- GPIO18
| TX           | ---- GPIO17
| SPK_1        | ---- Lautsprecher +
| SPK_2        | ---- Lautsprecher -
+--------------+
```

**Hinweis:** Der DFPlayer Pro arbeitet mit 3.3V-Logik und benoetigt keinen Vorwiderstand an RX. UART-Baudrate: 115200.

### HLK-LD2410C Radar-Sensor

```
LD2410C                ESP32-S3
+--------------+
| VCC          | ---- 3.3V
| GND          | ---- GND
| OUT          | ---- GPIO6
| TX           | ---- GPIO16
| RX           | ---- GPIO15
+--------------+
```

### USB-A Buchse (Presenter-Dongle)

```
USB-A Buchse           ESP32-S3
+--------------+
| VBUS (5V)    | ---- 5V
| D-           | ---- GPIO19
| D+           | ---- GPIO20
| GND          | ---- GND
+--------------+
```

### Komplette Pin-Belegung

| GPIO | Funktion |
|------|----------|
| 6    | LD2410C OUT (Praesenz-Signal) |
| 15   | LD2410C RX (UART) |
| 16   | LD2410C TX (UART) |
| 17   | DFPlayer Pro TX (UART 115200) |
| 18   | DFPlayer Pro RX (UART 115200) |
| 19   | USB Host D- (Presenter-Dongle) |
| 20   | USB Host D+ (Presenter-Dongle) |

## SD-Karte vorbereiten

1. microSD Karte (max. 32GB) mit **FAT32** formatieren
2. Eine MP3-Datei im Root-Verzeichnis ablegen:
   - `0001.mp3` — Begruessungs-Sound (auf max. Lautstaerke normalisiert)
3. SD-Karte in den DFPlayer Pro einsetzen

## Firmware flashen

### Voraussetzungen

- [PlatformIO](https://platformio.org/) (VS Code Extension oder CLI)
- USB-Kabel zum ESP32-S3

### Build & Upload

```bash
pio run -t upload
```

### Serial Monitor

```bash
pio device monitor
```

## Captive Portal - Bedienung

### Verbindung

1. Am Smartphone/Tablet WLAN-Einstellungen oeffnen
2. Netzwerk **"Laola-Setup"** waehlen
3. Passwort eingeben: **`laolawm2026`**
4. Das Captive Portal oeffnet sich automatisch
5. Falls nicht: Browser oeffnen und `http://192.168.4.1` aufrufen

### Einstellungen

#### Lautstaerke
- **Slider (0-30):** Lautstaerke regeln
- **Stumm-Button:** Ton ein/aus
- **Ton testen:** Spielt den Sound einmal testweise ab

#### Erkennung

- **Reichweite (1-6m):** Wie weit sollen Personen erkannt werden. Fuer einen Eingangsbereich empfohlen: 2-3m
- **Empfindlichkeit (Niedrig/Mittel/Hoch):**
  - Niedrig = nur direkt davor
  - Mittel = normaler Betrieb (empfohlen)
  - Hoch = auch seitlich und weiter entfernt
- **Pause nach Abspielen (0-60s):** Wartezeit bevor der Sound erneut ausgeloest wird. Verhindert Dauer-Abspielen bei stehenbleibenden Personen. Empfohlen: 10-30s
- **Sensor-Timeout (1-30s):** Nach wie vielen Sekunden Stillstand gilt eine Person als "weg"

#### Live-Status
- **Person:** Erkannt / Niemand
- **Entfernung:** Abstand in cm
- **Bewegung:** Ja / Nein
- **Audio:** Spielt / Pause / Bereit

### Empfohlene Einstellungen fuer Eingangsbereich

| Einstellung | Wert |
|-------------|------|
| Reichweite | 2-3m |
| Empfindlichkeit | Mittel |
| Pause nach Abspielen | 15-30s |
| Sensor-Timeout | 5s |

## Fernbedienung

Der Wireless Presenter wird per USB-Dongle an die USB-A Buchse angeschlossen.

| Taste | Funktion |
|-------|----------|
| Slide Backward (Page Up) | Lautstaerke erhoehen |
| Slide Forward (Page Down) | Lautstaerke verringern |
| Presenter-Taste (Tab) | Stumm schalten / Ton an |

## Funktionsweise

1. ESP32-S3 startet und oeffnet WLAN Access Point
2. Radar-Sensor ueberwacht den Eingangsbereich
3. Person betritt den Bereich → OUT-Pin geht HIGH
4. Sound wird einmal abgespielt (Track-Dauer wird automatisch erkannt)
5. Nach Ablauf des Sounds beginnt die Cooldown-Phase
6. Erst wenn Cooldown abgelaufen UND Person weg → bereit fuer naechste Erkennung

```
IDLE ──[Person erkannt]──> PLAYING ──[Sound fertig]──> COOLDOWN ──[Zeit+Person weg]──> IDLE
```

## Troubleshooting

| Problem | Loesung |
|---------|---------|
| DFPlayer Pro spielt nicht | SD-Karte FAT32 formatiert? Datei als `0001.mp3` im Root? UART-Baudrate 115200? |
| Sensor erkennt nichts | Verkabelung pruefen (TX/RX nicht vertauscht?). OUT-Pin an GPIO6? |
| Captive Portal oeffnet nicht | Manuell `http://192.168.4.1` im Browser aufrufen |
| Sound wird permanent getriggert | Pause nach Abspielen erhoehen (15-30s). Empfindlichkeit auf "Niedrig" |
| Presenter reagiert nicht | USB-Dongle in USB-A Buchse stecken. 5V an VBUS? |
| Einstellungen gehen verloren | Firmware-Update noetig? Einstellungen werden im NVS Flash gespeichert |

## Lizenz

MIT
