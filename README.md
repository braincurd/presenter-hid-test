# Laola - Interaktiver Sound Player

ESP32-S3-basierter Sound Player fuer den Eingangsbereich. Erkennt Personen per mmWave-Radar und spielt einen Begruessung-Sound ab. Steuerung ueber Wireless Presenter-Fernbedienung und Web-Interface (Captive Portal).

## Features

- **Personenerkennung** via HLK-LD2410C mmWave-Radar (kein PIR, funktioniert auch bei Stillstand)
- **MP3-Wiedergabe** ueber DFPlayer Mini mit Lautsprecher
- **Wireless Presenter** als Fernbedienung (USB HID via Dongle)
- **Captive Portal** zur Konfiguration per Smartphone
- **Persistente Einstellungen** bleiben nach Neustart erhalten
- **Anti-Fehl-Trigger** durch konfigurierbare Cooldown-Zeiten

## Hardware

| Komponente | Beschreibung |
|------------|-------------|
| ESP32-S3 DevKitC-1 (Freenove) | Mikrocontroller |
| HLK-LD2410C | 24GHz mmWave Praesenz-Sensor |
| DFPlayer Mini | MP3 Player Modul |
| Wireless Presenter (2.4GHz) | Fernbedienung mit USB-Dongle |
| Lautsprecher (4-8 Ohm) | Audio-Ausgabe |
| USB-A Buchse | Fuer Presenter-Dongle |
| microSD Karte (FAT32) | Fuer MP3-Dateien im DFPlayer |

## Verkabelung

### DFPlayer Mini

```
DFPlayer Mini          ESP32-S3
+--------------+
| VCC          | ---- 5V
| GND          | ---- GND
| RX           | ---- GPIO17 (ueber 1kOhm Widerstand)
| TX           | ---- GPIO18
| SPK_1        | ---- Lautsprecher +
| SPK_2        | ---- Lautsprecher -
+--------------+
```

**Wichtig:** Zwischen GPIO17 und DFPlayer RX einen 1kOhm Widerstand einsetzen (Pegelschutz).

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
| 17   | DFPlayer RX (UART, ueber 1kOhm) |
| 18   | DFPlayer TX (UART) |
| 19   | USB Host D- (Presenter-Dongle) |
| 20   | USB Host D+ (Presenter-Dongle) |

## SD-Karte vorbereiten

1. microSD Karte (max. 32GB) mit **FAT32** formatieren
2. MP3-Datei als `0001.mp3` im Root-Verzeichnis ablegen
3. SD-Karte in den DFPlayer Mini einsetzen

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
- **Track-Laenge:** Dauer der MP3-Datei in Sekunden (fuer Timer-Erkennung)
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
4. Sound wird einmal abgespielt
5. Nach Ablauf des Sounds beginnt die Cooldown-Phase
6. Erst wenn Cooldown abgelaufen UND Person weg → bereit fuer naechste Erkennung

```
IDLE ──[Person erkannt]──> PLAYING ──[Sound fertig]──> COOLDOWN ──[Zeit+Person weg]──> IDLE
```

## Troubleshooting

| Problem | Loesung |
|---------|---------|
| DFPlayer spielt nicht | SD-Karte FAT32 formatiert? Datei als `0001.mp3` im Root? 1kOhm Widerstand an RX? |
| Sensor erkennt nichts | Verkabelung pruefen (TX/RX nicht vertauscht?). OUT-Pin an GPIO6? |
| Captive Portal oeffnet nicht | Manuell `http://192.168.4.1` im Browser aufrufen |
| Sound wird permanent getriggert | Pause nach Abspielen erhoehen (15-30s). Empfindlichkeit auf "Niedrig" |
| Presenter reagiert nicht | USB-Dongle in USB-A Buchse stecken. 5V an VBUS? |
| Einstellungen gehen verloren | Firmware-Update noetig? Einstellungen werden im NVS Flash gespeichert |

## Lizenz

MIT
