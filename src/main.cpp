#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DFRobot_DF1201S.h>
#include <ld2410.h>
#include "driver/gpio.h"
#include "usb/usb_host.h"
#include "hid_host.h"
#include "hid_usage_keyboard.h"
#include "hid_usage_mouse.h"

static const char *TAG = "LAOLA";

// --- Pin Definitions ---
#define DFPLAYER_TX   18
#define DFPLAYER_RX   17
#define LD2410_OUT    6
#define LD2410_TX     16
#define LD2410_RX     15

// --- Audio State ---
static volatile uint8_t volume = 20;
static volatile bool muted = false;
static const uint8_t VOL_MAX = 30;

HardwareSerial dfSerial(1);
DFRobot_DF1201S dfPlayer;
static bool dfPlayerReady = false;

HardwareSerial radarSerial(2);
ld2410 radar;
static bool radarReady = false;

enum PlayState { STATE_IDLE, STATE_PLAYING, STATE_COOLDOWN };
static PlayState playState = STATE_IDLE;
static bool lastPresence = false;
static unsigned long lastRadarPrint = 0;
static unsigned long playbackFinishedAt = 0;
static unsigned long playbackStartedAt = 0;
static unsigned long cooldownSeconds = 10;
static unsigned long lastTriggerAt = 0;
static unsigned long minTriggerInterval = 5000;
static volatile bool requestPlay = false;
static uint16_t lastCurTime = 0;
static unsigned long lastCurTimeChange = 0;
static volatile uint8_t selectedTrack = 1;
static const uint8_t TRACK_COUNT = 4;
static const char *trackNames[] = {"Crowd Fans Song", "Crowd Reaction", "Fans Cheering", "Jingle mit Outro"};

// --- Persistent Settings ---
Preferences prefs;

static void save_settings() {
    prefs.begin("laola", false);
    prefs.putUChar("volume", volume);
    prefs.putBool("muted", muted);
    prefs.putULong("cooldown", cooldownSeconds);
    prefs.putUChar("track", selectedTrack);
    prefs.end();
    printf("[NVS] Einstellungen gespeichert\n");
}

static void load_settings() {
    prefs.begin("laola", true);
    volume = prefs.getUChar("volume", 20);
    muted = prefs.getBool("muted", false);
    cooldownSeconds = prefs.getULong("cooldown", 10);
    selectedTrack = prefs.getUChar("track", 1);
    if (selectedTrack < 1 || selectedTrack > TRACK_COUNT) selectedTrack = 1;
    prefs.end();
    printf("[NVS] Einstellungen geladen: vol=%d cool=%lu track=%d\n", volume, cooldownSeconds, selectedTrack);
}

// --- WiFi AP & Web ---
const char *AP_SSID = "Laola-Setup";
const char *AP_PASS = "laolawm2026";
DNSServer dnsServer;
AsyncWebServer server(80);

// --- USB Host ---
QueueHandle_t hid_host_event_queue;
bool user_shutdown = false;

typedef struct {
    hid_host_device_handle_t hid_device_handle;
    hid_host_driver_event_t event;
    void *arg;
} hid_host_event_queue_t;

static const char *hid_proto_name_str[] = {"NONE", "KEYBOARD", "MOUSE"};

static void apply_volume() {
    if (!dfPlayerReady) return;
    if (muted) {
        dfPlayer.setVol(0);
        printf("[Audio] MUTED\n");
    } else {
        dfPlayer.setVol(volume);
        printf("[Audio] Volume: %d / %d\n", volume, VOL_MAX);
    }
}

static void volume_up() {
    if (volume < VOL_MAX) volume++;
    if (muted) muted = false;
    apply_volume();
}

static void volume_down() {
    if (volume > 0) volume--;
    if (muted) muted = false;
    apply_volume();
}

static void toggle_mute() {
    muted = !muted;
    apply_volume();
}

static void set_radar_sensitivity(int level) {
    int moveSens, statSens;
    switch (level) {
        case 0: moveSens = 25; statSens = 15; break;
        case 1: moveSens = 50; statSens = 30; break;
        case 2: moveSens = 75; statSens = 50; break;
        default: moveSens = 50; statSens = 30; break;
    }
    printf("[Radar] Empfindlichkeit: %d (Bew=%d, Ruhe=%d)\n", level, moveSens, statSens);
    for (int g = 0; g <= 8; g++) {
        radar.setGateSensitivityThreshold(g, moveSens, statSens);
        delay(50);
    }
    radar.requestRestart();
    delay(1500);
    radar.requestCurrentConfiguration();
    delay(200);
    while (radar.read()) {}
}

static void set_radar_range(int meters) {
    int gate = meters * 100 / 75;
    if (gate < 2) gate = 2;
    if (gate > 8) gate = 8;
    printf("[Radar] Reichweite: %dm (Gate %d)\n", meters, gate);
    radar.setMaxValues(gate, gate, radar.sensor_idle_time);
    radar.requestRestart();
    delay(1500);
    radar.requestCurrentConfiguration();
    delay(200);
    while (radar.read()) {}
}

// --- Web Page ---
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="de">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>Laola Setup</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:Helvetica,Arial,sans-serif;background:#f9f9f9;color:#262626;max-width:480px;margin:0 auto}
.header{background:#e20074;padding:14px 20px;display:flex;align-items:center;gap:14px}
.logo{display:flex;align-items:center;gap:10px}
.logo svg{height:36px}
.header h1{color:#fff;font-size:1.1em;font-weight:700;letter-spacing:1px;text-transform:uppercase}
.content{padding:14px}
.card{background:#fff;border-radius:8px;padding:18px;margin-bottom:14px;border-left:4px solid #e20074;box-shadow:0 1px 4px rgba(0,0,0,.08)}
.card h2{color:#e20074;font-size:.8em;margin-bottom:14px;padding-bottom:8px;border-bottom:1px solid #eee;font-weight:700;text-transform:uppercase;letter-spacing:1px}
.field{margin-bottom:16px}
.field-label{font-size:.82em;color:#666;margin-bottom:6px;font-weight:700}
.field-help{font-size:.72em;color:#999;margin-top:4px;line-height:1.4}
.slider-row{display:flex;align-items:center;gap:10px}
.slider-row input[type=range]{flex:1}
.slider-val{min-width:40px;text-align:center;font-weight:bold;color:#e20074;font-size:.95em;background:#f2f2f2;padding:4px 8px;border-radius:4px}
input[type=range]{-webkit-appearance:none;height:6px;border-radius:3px;background:#ddd;outline:none}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:24px;height:24px;border-radius:50%;background:#e20074;cursor:pointer;border:3px solid #fff;box-shadow:0 1px 4px rgba(0,0,0,.25)}
.btn{display:inline-block;padding:12px 24px;border:none;border-radius:4px;font-size:.82em;cursor:pointer;font-weight:700;transition:all .15s;text-transform:uppercase;letter-spacing:.5px}
.btn-magenta{background:#e20074;color:#fff}
.btn-magenta:active{background:#b8005e}
.btn-dark{background:#262626;color:#fff}
.btn-dark:active{background:#444}
.btn-outline{background:#fff;color:#e20074;border:2px solid #e20074}
.btn-outline.active{background:#e20074;color:#fff}
.btn-small{padding:10px 16px;font-size:.75em}
.btn-row{display:flex;gap:8px;margin-top:4px;flex-wrap:wrap}
.toggle-group{display:flex;gap:0;border-radius:4px;overflow:hidden;border:2px solid #e20074}
.toggle-group .tg-btn{flex:1;padding:10px 8px;text-align:center;font-size:.75em;font-weight:700;cursor:pointer;background:#fff;color:#e20074;border:none;text-transform:uppercase;letter-spacing:.5px;transition:all .15s}
.toggle-group .tg-btn.active{background:#e20074;color:#fff}
.status-card{background:#fff;border-radius:8px;padding:16px;border-left:4px solid #00a14b;box-shadow:0 1px 4px rgba(0,0,0,.08)}
.status-card h2{color:#00a14b;font-size:.8em;margin-bottom:12px;padding-bottom:8px;border-bottom:1px solid #eee;font-weight:700;text-transform:uppercase;letter-spacing:1px}
.status-grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.stat{background:#f9f9f9;border-radius:6px;padding:10px;text-align:center}
.stat-label{font-size:.65em;color:#999;text-transform:uppercase;letter-spacing:.5px;margin-bottom:4px}
.stat-value{font-size:1.1em;font-weight:700;color:#262626}
.stat-value.active{color:#00a14b}
.stat-value.inactive{color:#ccc}
.stat-value.playing{color:#e20074}
.toast{position:fixed;bottom:20px;left:50%;transform:translateX(-50%);background:#00a14b;color:#fff;padding:10px 24px;border-radius:4px;font-weight:700;display:none;z-index:100;font-size:.8em;text-transform:uppercase;letter-spacing:.5px}
.divider{height:1px;background:#eee;margin:12px 0}
</style>
</head>
<body>
<div class="header">
<h1>Laola Setup</h1>
<span style="color:rgba(255,255,255,.6);font-size:.55em;margin-left:auto;white-space:nowrap">powered by Exponatwerke</span>
</div>
<div class="content">

<div class="card">
<h2>Lautstaerke</h2>
<div class="field">
<div class="slider-row">
<span style="font-size:1.2em">&#128264;</span>
<input type="range" id="vol" min="0" max="30" value="20" oninput="setVol(this.value)">
<span class="slider-val" id="volVal">20</span>
</div>
</div>
<div class="btn-row">
<button class="btn btn-magenta btn-small" onclick="toggleMute()" id="muteBtn">Stumm</button>
<button class="btn btn-dark btn-small" onclick="testPlay()">&#9654; Ton testen</button>
</div>
</div>

<div class="card">
<h2>Sound</h2>
<div class="field">
<div class="field-label">Track auswaehlen</div>
<div class="field-help">Welcher Sound soll bei Erkennung abgespielt werden?</div>
<div class="toggle-group" style="margin-top:8px;flex-direction:column" id="trackGroup">
<button class="tg-btn" onclick="setTrack(1)">1 - Crowd Fans Song</button>
<button class="tg-btn" onclick="setTrack(2)">2 - Crowd Reaction</button>
<button class="tg-btn active" onclick="setTrack(3)">3 - Fans Cheering</button>
<button class="tg-btn" onclick="setTrack(4)">4 - Jingle mit Outro</button>
</div>
</div>
</div>

<div class="card">
<h2>Erkennung</h2>

<div class="field">
<div class="field-label">Reichweite</div>
<div class="field-help">Wie weit sollen Personen im Eingangsbereich erkannt werden?</div>
<div class="slider-row" style="margin-top:8px">
<span style="font-size:.8em;color:#999">1m</span>
<input type="range" id="range" min="1" max="6" value="3" step="1" oninput="$('rangeVal').textContent=this.value+'m'">
<span style="font-size:.8em;color:#999">6m</span>
<span class="slider-val" id="rangeVal">3m</span>
</div>
<div style="text-align:right;margin-top:4px">
<button class="btn btn-outline btn-small" onclick="setRange()">Anwenden</button>
</div>
</div>

<div class="divider"></div>

<div class="field">
<div class="field-label">Empfindlichkeit</div>
<div class="field-help">Niedrig = nur direkt davor, Hoch = auch seitlich / weiter weg</div>
<div class="toggle-group" style="margin-top:8px" id="sensGroup">
<button class="tg-btn" onclick="setSens(0)">Niedrig</button>
<button class="tg-btn active" onclick="setSens(1)">Mittel</button>
<button class="tg-btn" onclick="setSens(2)">Hoch</button>
</div>
</div>

<div class="divider"></div>

<div class="field">
<div class="field-label">Pause nach Abspielen</div>
<div class="field-help">Wie lange warten, bevor der Ton erneut ausgeloest wird? Verhindert Dauer-Abspielen.</div>
<div class="slider-row" style="margin-top:8px">
<span style="font-size:.8em;color:#999">0s</span>
<input type="range" id="cooldown" min="0" max="60" value="10" step="5" oninput="$('cdVal').textContent=this.value+'s'" onchange="setCooldown()">
<span style="font-size:.8em;color:#999">60s</span>
<span class="slider-val" id="cdVal">10s</span>
</div>
</div>

<div class="divider"></div>

<div class="field">
<div class="field-label">Sensor-Timeout</div>
<div class="field-help">Nach wie vielen Sekunden Stillstand gilt eine Person als &quot;weg&quot;?</div>
<div class="slider-row" style="margin-top:8px">
<span style="font-size:.8em;color:#999">1s</span>
<input type="range" id="timeout" min="1" max="30" value="5" oninput="$('toVal').textContent=this.value+'s'" onchange="setSensorTimeout()">
<span style="font-size:.8em;color:#999">30s</span>
<span class="slider-val" id="toVal">5s</span>
</div>
</div>
</div>

<div class="status-card">
<h2>Live-Status</h2>
<div class="status-grid">
<div class="stat">
<div class="stat-label">Person</div>
<div class="stat-value" id="stPres">--</div>
</div>
<div class="stat">
<div class="stat-label">Entfernung</div>
<div class="stat-value" id="stDist">--</div>
</div>
<div class="stat">
<div class="stat-label">Bewegung</div>
<div class="stat-value" id="stMove">--</div>
</div>
<div class="stat">
<div class="stat-label">Audio</div>
<div class="stat-value" id="stAudio">--</div>
</div>
</div>
</div>

</div>

<div class="toast" id="toast">Gespeichert!</div>

<script>
function $(id){return document.getElementById(id)}
function toast(msg){var t=$('toast');t.textContent=msg||'Gespeichert!';t.style.display='block';setTimeout(function(){t.style.display='none'},1500)}
function api(url,data){return fetch(url,data?{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:data}:{}).then(r=>r.json())}

function setVol(v){$('volVal').textContent=v;api('/api/volume','volume='+v)}
function toggleMute(){api('/api/mute','mute=1').then(d=>{$('muteBtn').textContent=d.muted?'Ton an':'Stumm';toast(d.muted?'Stumm geschaltet':'Ton an')})}
function testPlay(){api('/api/play','test=1').then(d=>{if(d.ok)toast('Wird abgespielt...');else toast(d.error||'Fehler')})}

function setRange(){api('/api/radar/range','meters='+$('range').value).then(()=>toast('Reichweite gesetzt'))}

function setSens(level){
var btns=$('sensGroup').querySelectorAll('.tg-btn');
btns.forEach(function(b,i){b.classList.toggle('active',i===level)});
api('/api/radar/sensitivity','level='+level).then(()=>toast('Empfindlichkeit gesetzt'))}

function setTrack(n){
var btns=$('trackGroup').querySelectorAll('.tg-btn');
btns.forEach(function(b,i){b.classList.toggle('active',i===n-1)});
api('/api/track','track='+n).then(()=>toast('Track '+n+' gewaehlt'))}
function setCooldown(){api('/api/cooldown','seconds='+$('cooldown').value).then(()=>toast())}
function setSensorTimeout(){api('/api/radar/timeout','timeout='+$('timeout').value).then(()=>toast())}

function updateStatus(){
api('/api/status').then(function(d){
var pe=$('stPres');
if(d.presence){pe.textContent='Erkannt';pe.className='stat-value active'}
else{pe.textContent='Niemand';pe.className='stat-value inactive'}
var dist=Math.max(d.move_dist,d.stat_dist);
$('stDist').textContent=dist>0?dist+'cm':'--';
$('stMove').textContent=d.moving?'Ja':'Nein';
$('stMove').className='stat-value '+(d.moving?'active':'inactive');
var au=$('stAudio');
if(d.playing){au.textContent='Spielt';au.className='stat-value playing'}
else if(d.cooldown){au.textContent='Pause';au.className='stat-value inactive'}
else{au.textContent='Bereit';au.className='stat-value active'}
$('vol').value=d.volume;$('volVal').textContent=d.volume;
$('muteBtn').textContent=d.muted?'Ton an':'Stumm';
$('cooldown').value=d.cooldown_sec;$('cdVal').textContent=d.cooldown_sec+'s';
var tb=$('trackGroup').querySelectorAll('.tg-btn');
tb.forEach(function(b,i){b.classList.toggle('active',i===d.track-1)});
}).catch(function(){})}

updateStatus();
setInterval(updateStatus,2000);
</script>
</body>
</html>
)rawliteral";

// --- HID Callbacks ---
static void hid_host_keyboard_report_callback(const uint8_t *const data, const int length) {
    if (length < 8) return;
    static uint8_t prev_keys[6] = {0};
    for (int i = 2; i < 8 && i < length; i++) {
        uint8_t k = data[i];
        if (k == 0) continue;
        bool found = false;
        for (int j = 0; j < 6; j++) { if (prev_keys[j] == k) { found = true; break; } }
        if (found) continue;
        switch (k) {
            case 0x4B: volume_up(); break;
            case 0x4E: volume_down(); break;
            case 0x2B: toggle_mute(); break;
            default: break;
        }
    }
    memcpy(prev_keys, &data[2], 6);
}

static void hid_host_consumer_report_callback(const uint8_t *const data, const int length) {
    int offset = (length >= 3 && data[0] != 0 && data[0] < 0x10) ? 1 : 0;
    if (length - offset >= 2) {
        uint16_t usage = data[offset] | (data[offset + 1] << 8);
        switch (usage) {
            case 0x00E9: volume_up(); break;
            case 0x00EA: volume_down(); break;
            case 0x00E2: toggle_mute(); break;
            default: break;
        }
    }
}

void hid_host_interface_callback(hid_host_device_handle_t hid_device_handle,
                                  const hid_host_interface_event_t event, void *arg) {
    uint8_t data[64] = {0};
    size_t data_length = 0;
    hid_host_dev_params_t dev_params;
    ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));
    switch (event) {
        case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
            ESP_ERROR_CHECK(hid_host_device_get_raw_input_report_data(hid_device_handle, data, 64, &data_length));
            if (HID_SUBCLASS_BOOT_INTERFACE == dev_params.sub_class) {
                if (HID_PROTOCOL_KEYBOARD == dev_params.proto) hid_host_keyboard_report_callback(data, data_length);
                else if (HID_PROTOCOL_MOUSE == dev_params.proto) hid_host_consumer_report_callback(data, data_length);
            } else {
                hid_host_consumer_report_callback(data, data_length);
            }
            break;
        case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
            ESP_ERROR_CHECK(hid_host_device_close(hid_device_handle));
            break;
        default: break;
    }
}

void hid_host_device_event(hid_host_device_handle_t hid_device_handle,
                            const hid_host_driver_event_t event, void *arg) {
    hid_host_dev_params_t dev_params;
    ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));
    const hid_host_device_config_t dev_config = { .callback = hid_host_interface_callback, .callback_arg = NULL };
    if (event == HID_HOST_DRIVER_EVENT_CONNECTED) {
        ESP_ERROR_CHECK(hid_host_device_open(hid_device_handle, &dev_config));
        hid_class_request_set_idle(hid_device_handle, 0, 0);
        ESP_ERROR_CHECK(hid_host_device_start(hid_device_handle));
        printf("[USB] Fernbedienung verbunden\n");
    }
}

static void usb_lib_task(void *arg) {
    const usb_host_config_t host_config = { .skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1 };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskNotifyGive((TaskHandle_t)arg);
    while (!user_shutdown) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
    vTaskDelay(10);
    ESP_ERROR_CHECK(usb_host_uninstall());
    vTaskDelete(NULL);
}

void hid_host_task(void *pvParameters) {
    hid_host_event_queue_t evt_queue;
    hid_host_event_queue = xQueueCreate(10, sizeof(hid_host_event_queue_t));
    while (!user_shutdown) {
        if (xQueueReceive(hid_host_event_queue, &evt_queue, pdMS_TO_TICKS(50)))
            hid_host_device_event(evt_queue.hid_device_handle, evt_queue.event, evt_queue.arg);
    }
    xQueueReset(hid_host_event_queue);
    vQueueDelete(hid_host_event_queue);
    vTaskDelete(NULL);
}

void hid_host_device_callback(hid_host_device_handle_t hid_device_handle,
                               const hid_host_driver_event_t event, void *arg) {
    const hid_host_event_queue_t evt_queue = { .hid_device_handle = hid_device_handle, .event = event, .arg = arg };
    xQueueSend(hid_host_event_queue, &evt_queue, 0);
}

// --- Web Server ---
void setup_webserver() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *r) { r->send_P(200, "text/html", INDEX_HTML); });
    server.on("/generate_204", HTTP_GET, [](AsyncWebServerRequest *r) { r->redirect("/"); });
    server.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest *r) { r->redirect("/"); });
    server.on("/connecttest.txt", HTTP_GET, [](AsyncWebServerRequest *r) { r->redirect("/"); });
    server.on("/redirect", HTTP_GET, [](AsyncWebServerRequest *r) { r->redirect("/"); });
    server.on("/fwlink", HTTP_GET, [](AsyncWebServerRequest *r) { r->redirect("/"); });

    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *r) {
        char json[300];
        snprintf(json, sizeof(json),
            "{\"presence\":%s,\"moving\":%s,\"move_dist\":%d,\"move_energy\":%d,"
            "\"stat_dist\":%d,\"stat_energy\":%d,"
            "\"playing\":%s,\"cooldown\":%s,\"volume\":%d,\"muted\":%s,\"cooldown_sec\":%lu,\"track\":%d}",
            radar.presenceDetected() ? "true" : "false",
            radar.movingTargetDetected() ? "true" : "false",
            radar.movingTargetDistance(), radar.movingTargetEnergy(),
            radar.stationaryTargetDistance(), radar.stationaryTargetEnergy(),
            playState == STATE_PLAYING ? "true" : "false",
            playState == STATE_COOLDOWN ? "true" : "false",
            (int)volume, muted ? "true" : "false", cooldownSeconds, (int)selectedTrack);
        r->send(200, "application/json", json);
    });

    server.on("/api/volume", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (r->hasParam("volume", true)) {
            volume = r->getParam("volume", true)->value().toInt();
            if (volume > VOL_MAX) volume = VOL_MAX;
            muted = false;
            apply_volume();
            save_settings();
        }
        char json[64]; snprintf(json, sizeof(json), "{\"volume\":%d}", (int)volume);
        r->send(200, "application/json", json);
    });

    server.on("/api/mute", HTTP_POST, [](AsyncWebServerRequest *r) {
        toggle_mute();
        save_settings();
        char json[64]; snprintf(json, sizeof(json), "{\"muted\":%s}", muted ? "true" : "false");
        r->send(200, "application/json", json);
    });

    server.on("/api/play", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (!dfPlayerReady) {
            r->send(200, "application/json", "{\"ok\":false,\"error\":\"DFPlayer nicht verbunden\"}");
            return;
        }
        printf("[Web] Test Play angefordert\n");
        requestPlay = true;
        r->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/track", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (r->hasParam("track", true)) {
            int t = r->getParam("track", true)->value().toInt();
            if (t >= 1 && t <= TRACK_COUNT) {
                selectedTrack = t;
                printf("[Web] Track: %d - %s\n", selectedTrack, trackNames[selectedTrack - 1]);
                save_settings();
            }
        }
        char json[64]; snprintf(json, sizeof(json), "{\"track\":%d}", (int)selectedTrack);
        r->send(200, "application/json", json);
    });

    server.on("/api/cooldown", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (r->hasParam("seconds", true)) {
            cooldownSeconds = r->getParam("seconds", true)->value().toInt();
            printf("[Web] Cooldown: %lu s\n", cooldownSeconds);
            save_settings();
        }
        r->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/radar/range", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (r->hasParam("meters", true)) {
            int m = r->getParam("meters", true)->value().toInt();
            set_radar_range(m);
        }
        r->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/radar/sensitivity", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (r->hasParam("level", true)) {
            int l = r->getParam("level", true)->value().toInt();
            set_radar_sensitivity(l);
        }
        r->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/radar/timeout", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (r->hasParam("timeout", true)) {
            int t = r->getParam("timeout", true)->value().toInt();
            printf("[Web] Sensor-Timeout: %d s\n", t);
            radar.setMaxValues(radar.max_moving_gate, radar.max_stationary_gate, t);
            radar.requestRestart();
            delay(1500);
            radar.requestCurrentConfiguration();
        }
        r->send(200, "application/json", "{\"ok\":true}");
    });

    server.onNotFound([](AsyncWebServerRequest *r) { r->redirect("/"); });
    server.begin();
}

void app_main(void) {
    ESP_LOGI(TAG, "Laola Sound Player");

    printf("\n========================================\n");
    printf("  Laola Setup - ESP32-S3\n");
    printf("========================================\n\n");

    load_settings();

    // --- WiFi AP ---
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    delay(100);
    printf("WiFi AP: '%s' -> http://%s\n\n", AP_SSID, WiFi.softAPIP().toString().c_str());
    dnsServer.start(53, "*", WiFi.softAPIP());

    // --- DFPlayer Pro ---
    dfSerial.begin(115200, SERIAL_8N1, DFPLAYER_RX, DFPLAYER_TX);
    delay(1000);

    if (!dfPlayer.begin(dfSerial)) {
        printf("ERROR: DFPlayer Pro nicht gefunden!\n");
        printf("  Pruefe: TX->GPIO%d, RX->GPIO%d, 3.3-5V, GND\n", DFPLAYER_TX, DFPLAYER_RX);
    } else {
        dfPlayerReady = true;
        dfPlayer.switchFunction(dfPlayer.MUSIC);
        delay(500);
        dfPlayer.setPlayMode(dfPlayer.SINGLE);
        dfPlayer.setPrompt(false);
        dfPlayer.setVol(volume);
        delay(200);
        dfPlayer.pause();
        int files = dfPlayer.getTotalFile();
        printf("DFPlayer Pro verbunden. Dateien: %d\n", files);
        if (files == 0) {
            printf("  WARNUNG: Keine MP3-Dateien auf SD!\n");
            printf("  Datei als 0001.mp3 im Root ablegen.\n");
        }
    }
    printf("Audio Volume: %d / %d\n\n", volume, VOL_MAX);

    // --- LD2410C ---
    radarSerial.begin(256000, SERIAL_8N1, LD2410_TX, LD2410_RX);
    delay(500);

    if (radar.begin(radarSerial)) {
        radarReady = true;
        printf("LD2410C verbunden.\n");
        radar.requestCurrentConfiguration();
        delay(200);
        while (radar.read()) {}
        printf("  Max Bewegungs-Gate: %d\n", radar.max_moving_gate);
        printf("  Max Ruhe-Gate: %d\n", radar.max_stationary_gate);
        printf("  Timeout: %d s\n", radar.sensor_idle_time);
    } else {
        printf("ERROR: LD2410C nicht gefunden!\n");
    }

    pinMode(LD2410_OUT, INPUT);

    // --- Web Server ---
    setup_webserver();
    printf("Webserver bereit.\n\n");

    // --- USB Host ---
    xTaskCreatePinnedToCore(usb_lib_task, "usb_events", 4096,
                            xTaskGetCurrentTaskHandle(), 2, NULL, 0);
    ulTaskNotifyTake(false, 1000);

    const hid_host_driver_config_t hid_host_driver_config = {
        .create_background_task = true, .task_priority = 5, .stack_size = 4096,
        .core_id = 0, .callback = hid_host_device_callback, .callback_arg = NULL
    };
    ESP_ERROR_CHECK(hid_host_install(&hid_host_driver_config));
    user_shutdown = false;
    xTaskCreate(&hid_host_task, "hid_task", 4 * 1024, NULL, 2, NULL);

    printf("Bereit! WLAN '%s' verbinden.\n\n", AP_SSID);
}

void setup() {
    Serial.begin(115200);
    Serial.setDebugOutput(true);
    app_main();
}

static void do_play() {
    if (!dfPlayerReady) return;
    printf("[Audio] Play Track %d - %s\n", selectedTrack, trackNames[selectedTrack - 1]);
    dfPlayer.setVol(muted ? 0 : volume);
    delay(100);
    dfPlayer.playFileNum(selectedTrack);
    delay(100);
    playState = STATE_PLAYING;
    playbackStartedAt = millis();
    lastCurTime = 0xFFFF;
    lastCurTimeChange = millis();
}

void loop() {
    dnsServer.processNextRequest();
    radar.read();

    // Web-Play-Request aus dem Main-Loop ausfuehren
    if (requestPlay) {
        requestPlay = false;
        do_play();
    }

    if (millis() - lastRadarPrint > 2000) {
        lastRadarPrint = millis();
        if (radar.presenceDetected()) {
            printf("[Radar] %s | %dcm (E:%d) | Ruhe: %dcm (E:%d)\n",
                   radar.movingTargetDetected() ? "Bewegung" : "Ruhend",
                   radar.movingTargetDistance(), radar.movingTargetEnergy(),
                   radar.stationaryTargetDistance(), radar.stationaryTargetEnergy());
        }
    }

    bool presence = digitalRead(LD2410_OUT) == HIGH;

    switch (playState) {
        case STATE_IDLE:
            if (presence && !lastPresence && dfPlayerReady) {
                unsigned long now = millis();
                if (now - lastTriggerAt >= minTriggerInterval) {
                    printf("[Trigger] Person erkannt -> Play\n");
                    lastTriggerAt = now;
                    do_play();
                }
            }
            break;

        case STATE_PLAYING:
            if (millis() - playbackStartedAt > 1500) {
                uint16_t cur = dfPlayer.getCurTime();
                if (cur != lastCurTime) {
                    lastCurTime = cur;
                    lastCurTimeChange = millis();
                }
                // Track fertig: curTime aendert sich seit >3s nicht mehr
                if (millis() - lastCurTimeChange > 3000) {
                    printf("[Audio] Fertig (keine Aenderung seit 3s, cur=%d)\n", cur);
                    playbackFinishedAt = millis();
                    playState = STATE_COOLDOWN;
                } else if (millis() - playbackStartedAt > 180000) {
                    printf("[Audio] Fertig (Timeout)\n");
                    playbackFinishedAt = millis();
                    playState = STATE_COOLDOWN;
                }
            }
            break;

        case STATE_COOLDOWN:
            if (millis() - playbackFinishedAt >= cooldownSeconds * 1000) {
                if (!presence) {
                    printf("[Cooldown] Abgelaufen + Person weg -> bereit\n");
                    playState = STATE_IDLE;
                }
            }
            break;
    }

    lastPresence = presence;
    delay(50);
}
