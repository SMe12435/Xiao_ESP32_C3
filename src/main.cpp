// ═══════════════════════════════════════════════════════════════
// XIAO ESP32-C3 Audio Device Firmware
// Single-core RISC-V audio recorder with live streaming & offline recording
// ═══════════════════════════════════════════════════════════════

// ── 1. Includes and Pin Definitions ──────────────────────────

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ArduinoWebsockets.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <driver/i2s.h>
#include <esp_ota_ops.h>
#include <esp_http_client.h>
#include <esp_wifi.h>

#define PIN_I2S_WS    2
#define PIN_I2S_SCK   3
#define PIN_I2S_DATA  4
#define PIN_BUTTON    5
#define PIN_LED       6
#define PIN_SD_CS     7
#define PIN_SD_SCK    8
#define PIN_SD_MISO   9
#define PIN_SD_MOSI   10

#define SAMPLE_RATE       16000
#define I2S_PORT          I2S_NUM_0
#define DMA_BUF_COUNT     8
#define DMA_BUF_LEN       128
#define AUDIO_CHUNK_SAMPLES 512

#define DEFAULT_SERVER_URL "ws://15.206.232.216:8889"

#ifndef FW_VERSION
#define FW_VERSION "1.0.0"
#endif

using namespace websockets;

// ── 2. IMA ADPCM Encoder ─────────────────────────────────────

static const int16_t adpcmStepTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static const int8_t adpcmIndexTable[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

struct AdpcmState {
    int16_t predictor;
    int8_t  index;
};

static AdpcmState adpcmEncoder = {0, 0};

static uint8_t adpcm_encode_sample(int16_t sample) {
    int step = adpcmStepTable[adpcmEncoder.index];
    int diff = sample - adpcmEncoder.predictor;
    uint8_t nibble = 0;

    if (diff < 0) {
        nibble = 8;
        diff = -diff;
    }

    if (diff >= step) { nibble |= 4; diff -= step; }
    if (diff >= (step >> 1)) { nibble |= 2; diff -= (step >> 1); }
    if (diff >= (step >> 2)) { nibble |= 1; }

    // Reconstruct for predictor update
    int pred_diff = (step >> 3);
    if (nibble & 4) pred_diff += step;
    if (nibble & 2) pred_diff += (step >> 1);
    if (nibble & 1) pred_diff += (step >> 2);
    if (nibble & 8) pred_diff = -pred_diff;

    int32_t newPred = adpcmEncoder.predictor + pred_diff;
    if (newPred > 32767) newPred = 32767;
    if (newPred < -32768) newPred = -32768;
    adpcmEncoder.predictor = (int16_t)newPred;

    int newIndex = adpcmEncoder.index + adpcmIndexTable[nibble];
    if (newIndex < 0) newIndex = 0;
    if (newIndex > 88) newIndex = 88;
    adpcmEncoder.index = (int8_t)newIndex;

    return nibble & 0x0F;
}

static void adpcm_encode_block(const int16_t *pcm, size_t numSamples, uint8_t *out) {
    for (size_t i = 0; i < numSamples; i += 2) {
        uint8_t lo = adpcm_encode_sample(pcm[i]);
        uint8_t hi = (i + 1 < numSamples) ? adpcm_encode_sample(pcm[i + 1]) : 0;
        out[i / 2] = lo | (hi << 4);
    }
}

// ── 3. State Machine ─────────────────────────────────────────

enum AppState {
    STATE_SETUP,
    STATE_WIFI_CONNECTING,
    STATE_OTA_CHECK,
    STATE_CHECK_PAIRED,
    STATE_PAIRING,
    STATE_WS_CONNECTING,
    STATE_READY,
    STATE_STREAMING,
    STATE_RECORDING_OFFLINE,
    STATE_UPLOADING_PENDING,
    STATE_OTA_UPDATING
};

// ── 4. Global Variables ──────────────────────────────────────

static int32_t  i2sRawBuf[AUDIO_CHUNK_SAMPLES];
static int16_t  pcmChunk[AUDIO_CHUNK_SAMPLES];
static uint8_t  adpcmBuf[AUDIO_CHUNK_SAMPLES / 2];  // 256 bytes

static WebsocketsClient wsClient;
static Preferences prefs;

static AppState appState = STATE_SETUP;

static String wifiSSID;
static String wifiPass;
static String serverUrl;
static String deviceToken;
static String deviceMAC;
static String pairingCode;

static String serverHost;
static uint16_t serverPort = 8889;
static String serverPath;

static bool wsConnected   = false;
static bool wsAuthOk      = false;
static bool sdAvailable   = false;

static unsigned long lastPingMs       = 0;
static unsigned long lastOtaCheckMs   = 0;
static unsigned long lastUploadCheckMs = 0;
static unsigned long stateEnteredMs   = 0;
static int wifiRetryCount             = 0;

// Captive portal objects
static WebServer *portalServer = nullptr;
static DNSServer *dnsServer    = nullptr;

// Offline recording
static File recordingFile;
static uint32_t recordedBytes = 0;

// ── 5. LED Controller ────────────────────────────────────────

static uint16_t ledOnMs  = 0;
static uint16_t ledOffMs = 0;
static bool     ledState = false;
static unsigned long ledLastToggle = 0;

static void ledSetPattern(uint16_t onMs, uint16_t offMs) {
    ledOnMs  = onMs;
    ledOffMs = offMs;
}

static void ledUpdate() {
    if (ledOnMs == 0 && ledOffMs == 0) {
        digitalWrite(PIN_LED, LOW);
        return;
    }
    if (ledOffMs == 0) {
        // Solid on
        digitalWrite(PIN_LED, HIGH);
        return;
    }
    unsigned long now = millis();
    unsigned long interval = ledState ? ledOnMs : ledOffMs;
    if (now - ledLastToggle >= interval) {
        ledState = !ledState;
        digitalWrite(PIN_LED, ledState ? HIGH : LOW);
        ledLastToggle = now;
    }
}

// ── 6. Button Handler ────────────────────────────────────────

static bool     btnLastState   = HIGH;
static unsigned long btnDebounceMs = 0;
static bool     btnPressed     = false;
static unsigned long btnPressStartMs = 0;

static bool buttonWasPressed() {
    bool state = digitalRead(PIN_BUTTON);
    unsigned long now = millis();

    if (state != btnLastState && (now - btnDebounceMs) > 50) {
        btnDebounceMs = now;
        btnLastState = state;
        if (state == LOW) {
            btnPressStartMs = now;
        }
        if (state == HIGH && btnPressStartMs > 0) {
            btnPressed = true;
            btnPressStartMs = 0;
        }
    }

    if (btnPressed) {
        btnPressed = false;
        return true;
    }
    return false;
}

static bool buttonIsHeld() {
    return (digitalRead(PIN_BUTTON) == LOW);
}

// ── 7. DC Offset Removal ────────────────────────────────────

static int32_t dcOffset = 0;

static int16_t removeDcOffset(int16_t sample) {
    dcOffset += ((int32_t)sample - dcOffset) >> 8;
    int32_t corrected = (int32_t)sample - dcOffset;
    if (corrected > 32767) corrected = 32767;
    if (corrected < -32768) corrected = -32768;
    return (int16_t)corrected;
}

// ── 8. I2S Mic Setup and Read ────────────────────────────────

static void setupI2S() {
    i2s_config_t i2s_config = {};
    i2s_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    i2s_config.sample_rate = SAMPLE_RATE;
    i2s_config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
    i2s_config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    i2s_config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    i2s_config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    i2s_config.dma_buf_count = DMA_BUF_COUNT;
    i2s_config.dma_buf_len = DMA_BUF_LEN;
    i2s_config.use_apll = false;

    i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);

    i2s_pin_config_t pin_config = {};
    pin_config.bck_io_num   = PIN_I2S_SCK;
    pin_config.ws_io_num    = PIN_I2S_WS;
    pin_config.data_in_num  = PIN_I2S_DATA;
    pin_config.data_out_num = I2S_PIN_NO_CHANGE;

    i2s_set_pin(I2S_PORT, &pin_config);
}

static bool readMicChunk() {
    size_t bytesRead = 0;
    esp_err_t err = i2s_read(I2S_PORT, i2sRawBuf, sizeof(i2sRawBuf), &bytesRead, pdMS_TO_TICKS(100));
    if (err != ESP_OK || bytesRead == 0) return false;

    size_t samplesRead = bytesRead / sizeof(int32_t);
    for (size_t i = 0; i < samplesRead; i++) {
        pcmChunk[i] = (int16_t)(i2sRawBuf[i] >> 11);
        pcmChunk[i] = removeDcOffset(pcmChunk[i]);
    }
    return true;
}

// ── 9. SD Card Helpers ───────────────────────────────────────

static void setupSD() {
    SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    if (!SD.begin(PIN_SD_CS)) {
        Serial.println("[SD] Card mount failed");
        sdAvailable = false;
        return;
    }
    sdAvailable = true;
    Serial.printf("[SD] Card mounted, size: %lluMB\n", SD.cardSize() / (1024 * 1024));

    if (!SD.exists("/pending")) {
        SD.mkdir("/pending");
    }
}

static String getPendingFilename() {
    char buf[40];
    snprintf(buf, sizeof(buf), "/pending/rec_%08lX.wav", millis());
    return String(buf);
}

static void writeWavHeader(File &f, uint32_t dataSize) {
    uint32_t fileSize = 36 + dataSize;
    uint16_t channels = 1;
    uint32_t sampleRate = SAMPLE_RATE;
    uint16_t bitsPerSample = 16;
    uint32_t byteRate = sampleRate * channels * bitsPerSample / 8;
    uint16_t blockAlign = channels * bitsPerSample / 8;

    f.seek(0);
    f.write((const uint8_t*)"RIFF", 4);
    f.write((const uint8_t*)&fileSize, 4);
    f.write((const uint8_t*)"WAVE", 4);
    f.write((const uint8_t*)"fmt ", 4);
    uint32_t fmtSize = 16;
    f.write((const uint8_t*)&fmtSize, 4);
    uint16_t audioFormat = 1; // PCM
    f.write((const uint8_t*)&audioFormat, 2);
    f.write((const uint8_t*)&channels, 2);
    f.write((const uint8_t*)&sampleRate, 4);
    f.write((const uint8_t*)&byteRate, 4);
    f.write((const uint8_t*)&blockAlign, 2);
    f.write((const uint8_t*)&bitsPerSample, 2);
    f.write((const uint8_t*)"data", 4);
    f.write((const uint8_t*)&dataSize, 4);
}

static int countPendingFiles() {
    if (!sdAvailable) return 0;
    int count = 0;
    File dir = SD.open("/pending");
    if (!dir) return 0;
    File entry = dir.openNextFile();
    while (entry) {
        if (!entry.isDirectory() && String(entry.name()).endsWith(".wav")) {
            count++;
        }
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();
    return count;
}

// ── 10. WiFi Connection ──────────────────────────────────────

static bool connectWiFi() {
    if (wifiSSID.length() == 0) return false;

    Serial.printf("[WiFi] Connecting to '%s'...\n", wifiSSID.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < 20000) {
        delay(250);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WiFi] Connected, IP: %s, RSSI: %d\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
        return true;
    }

    Serial.println("[WiFi] Connection failed");
    WiFi.disconnect();
    return false;
}

static void parseServerUrl(const String &url);

// ── 11. Captive Portal ───────────────────────────────────────

static const char PORTAL_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>XIAO Audio Setup</title>
<style>
body{font-family:-apple-system,sans-serif;margin:0;padding:20px;background:#1a1a2e;color:#eee}
h1{color:#e94560;font-size:1.4em}
h2{color:#eee;font-size:1.1em;margin:0 0 8px}
.card{background:#16213e;border-radius:12px;padding:20px;margin:10px 0}
label{display:block;margin:12px 0 4px;font-size:.9em;color:#aaa}
input{width:100%;padding:10px;border:1px solid #333;border-radius:8px;
background:#0f3460;color:#eee;box-sizing:border-box;font-size:1em}
button{width:100%;padding:14px;margin-top:20px;border:none;border-radius:8px;
background:#e94560;color:#fff;font-size:1.1em;cursor:pointer}
button:active{background:#c73e54}
button:disabled{background:#555;cursor:default}
.net{padding:10px;margin:4px 0;background:#0f3460;border-radius:8px;cursor:pointer}
.net:active{background:#1a4a8a}
.rssi{float:right;color:#888;font-size:.8em}
.hint{color:#888;font-size:.8em;margin-top:4px}
.err{color:#e94560;text-align:center;margin-top:12px}
.ok{color:#4ecca3;text-align:center;margin-top:12px}
.hidden{display:none}
.spinner{text-align:center;padding:30px;color:#aaa}
.spinner::after{content:'';display:block;width:32px;height:32px;margin:12px auto;
border:3px solid #333;border-top:3px solid #e94560;border-radius:50%;
animation:spin 1s linear infinite}
@keyframes spin{to{transform:rotate(360deg)}}
.toggle{display:flex;margin:0 0 16px;border-radius:8px;overflow:hidden}
.toggle button{flex:1;margin:0;border-radius:0;padding:10px;font-size:.95em;background:#0f3460}
.toggle button.active{background:#e94560}
details summary{cursor:pointer;color:#aaa;font-size:.9em;margin-top:8px}
</style>
</head>
<body>
<h1>XIAO Audio Setup</h1>

<div id="step1">
<div class="card">
<h2>WiFi Network</h2>
<div id="nets">Scanning...</div>
<label>SSID</label>
<input id="ssid" placeholder="Network name">
<label>Password</label>
<input id="pass" type="password" placeholder="WiFi password">
<details><summary>Advanced</summary>
<label>Server URL</label>
<input id="server" value="ws://15.206.232.216:8889">
</details>
</div>
<div id="err1" class="err hidden"></div>
<button id="btnConnect" onclick="doConnect()">Connect</button>
</div>

<div id="stepSpinner" class="hidden">
<div class="spinner">Connecting to WiFi...</div>
</div>

<div id="step2" class="hidden">
<div class="card">
<h2>Your Account</h2>
<div class="toggle">
<button id="tabLogin" class="active" onclick="setAction('login')">Sign In</button>
<button id="tabSignup" onclick="setAction('signup')">Sign Up</button>
</div>
<label>Email</label>
<input id="email" type="email" placeholder="you@example.com">
<label>Password</label>
<input id="acctpass" type="password" placeholder="Account password">
</div>
<div id="err2" class="err hidden"></div>
<button id="btnPair" onclick="doPair()">Complete Setup</button>
</div>

<div id="stepDone" class="hidden">
<div class="card" style="text-align:center">
<h2 style="color:#4ecca3">Setup Complete!</h2>
<p class="hint">Device is rebooting into normal mode...</p>
</div>
</div>

<script>
var action='login';
fetch('/scan').then(r=>r.json()).then(nets=>{
 var h='';
 nets.forEach(function(n){
  h+='<div class="net" onclick="document.getElementById(\'ssid\').value=\''+n.ssid+'\'">'+
  n.ssid+'<span class="rssi">'+n.rssi+'dBm</span></div>';
 });
 document.getElementById('nets').innerHTML=h||'No networks found';
}).catch(function(){document.getElementById('nets').innerHTML='Scan failed';});

function setAction(a){
 action=a;
 document.getElementById('tabLogin').className=(a==='login'?'active':'');
 document.getElementById('tabSignup').className=(a==='signup'?'active':'');
}

function showErr(id,msg){
 var el=document.getElementById(id);el.textContent=msg;el.className='err';
}
function hideErr(id){
 var el=document.getElementById(id);el.textContent='';el.className='err hidden';
}

function doConnect(){
 var ssid=document.getElementById('ssid').value;
 if(!ssid){showErr('err1','WiFi SSID is required');return;}
 hideErr('err1');
 document.getElementById('btnConnect').disabled=true;
 var body=JSON.stringify({
  ssid:ssid,
  pass:document.getElementById('pass').value,
  server:document.getElementById('server').value
 });
 fetch('/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:body})
 .then(function(r){return r.json();})
 .then(function(){
  document.getElementById('step1').className='hidden';
  document.getElementById('stepSpinner').className='';
  pollWifi();
 }).catch(function(){
  showErr('err1','Failed to send WiFi config');
  document.getElementById('btnConnect').disabled=false;
 });
}

function pollWifi(){
 var attempts=0;
 var iv=setInterval(function(){
  fetch('/wifi-status').then(function(r){return r.json();}).then(function(d){
   if(d.connected){
    clearInterval(iv);
    document.getElementById('stepSpinner').className='hidden';
    document.getElementById('step2').className='';
   }
  }).catch(function(){});
  attempts++;
  if(attempts>60){
   clearInterval(iv);
   document.getElementById('stepSpinner').className='hidden';
   document.getElementById('step1').className='';
   document.getElementById('btnConnect').disabled=false;
   showErr('err1','WiFi connection timed out');
  }
 },2000);
}

function doPair(){
 var email=document.getElementById('email').value;
 var pass=document.getElementById('acctpass').value;
 if(!email||!pass){showErr('err2','Email and password are required');return;}
 hideErr('err2');
 document.getElementById('btnPair').disabled=true;
 var body=JSON.stringify({email:email,password:pass,action:action});
 fetch('/pair',{method:'POST',headers:{'Content-Type':'application/json'},body:body})
 .then(function(r){return r.json();})
 .then(function(d){
  if(d.success){
   document.getElementById('step2').className='hidden';
   document.getElementById('stepDone').className='';
  } else {
   showErr('err2',d.error||'Pairing failed');
   document.getElementById('btnPair').disabled=false;
  }
 }).catch(function(){
  showErr('err2','Connection to device lost');
  document.getElementById('btnPair').disabled=false;
 });
}
</script>
</body>
</html>
)rawliteral";

static bool portalWifiConnecting = false;

static void handlePortalRoot() {
    portalServer->send(200, "text/html", FPSTR(PORTAL_HTML));
}

static void handlePortalScan() {
    int n = WiFi.scanNetworks();
    String json = "[";
    for (int i = 0; i < n; i++) {
        if (i > 0) json += ",";
        json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
    }
    json += "]";
    WiFi.scanDelete();
    portalServer->send(200, "application/json", json);
}

static void handlePortalWifi() {
    String body = portalServer->arg("plain");

    auto extractJson = [](const String &src, const char *key) -> String {
        int idx = src.indexOf(key);
        if (idx < 0) return "";
        int colon = src.indexOf(':', idx);
        int qs = src.indexOf('"', colon + 1);
        int qe = src.indexOf('"', qs + 1);
        return (qs >= 0 && qe > qs) ? src.substring(qs + 1, qe) : "";
    };

    String ssid = extractJson(body, "\"ssid\"");
    String pass = extractJson(body, "\"pass\"");
    String srv  = extractJson(body, "\"server\"");

    prefs.begin("audio", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    if (srv.length() > 0) prefs.putString("server", srv);
    prefs.end();

    wifiSSID = ssid;
    wifiPass = pass;
    if (srv.length() > 0) {
        serverUrl = srv;
        parseServerUrl(serverUrl);
    }

    WiFi.mode(WIFI_AP_STA);
    WiFi.setSleep(false);
    delay(200);
    WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
    portalWifiConnecting = true;

    Serial.printf("[Portal] WiFi connecting to '%s' (AP+STA mode)\n", wifiSSID.c_str());
    portalServer->send(200, "application/json", "{\"ok\":true}");
}

static void handlePortalWifiStatus() {
    bool connected = (WiFi.status() == WL_CONNECTED);
    String ip = connected ? WiFi.localIP().toString() : "";
    Serial.printf("[Portal] wifi-status: %s ip=%s\n",
                  connected ? "connected" : "not connected", ip.c_str());
    portalServer->send(200, "application/json",
        "{\"connected\":" + String(connected ? "true" : "false") +
        ",\"ip\":\"" + ip + "\"}");
}

static void handlePortalPair() {
    String body = portalServer->arg("plain");

    auto extractJson = [](const String &src, const char *key) -> String {
        int idx = src.indexOf(key);
        if (idx < 0) return "";
        int colon = src.indexOf(':', idx);
        int qs = src.indexOf('"', colon + 1);
        int qe = src.indexOf('"', qs + 1);
        return (qs >= 0 && qe > qs) ? src.substring(qs + 1, qe) : "";
    };

    String email    = extractJson(body, "\"email\"");
    String password = extractJson(body, "\"password\"");
    String act      = extractJson(body, "\"action\"");
    if (act.length() == 0) act = "login";

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[Portal] WiFi not connected at pair time, reconnecting...");
        WiFi.reconnect();
        unsigned long waitStart = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - waitStart) < 15000) {
            delay(250);
            if ((millis() - waitStart) > 5000 && WiFi.status() != WL_CONNECTED) {
                WiFi.disconnect();
                delay(100);
                WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
                while (WiFi.status() != WL_CONNECTED && (millis() - waitStart) < 15000) {
                    delay(250);
                }
                break;
            }
        }
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[Portal] WiFi reconnect failed");
            portalServer->send(200, "application/json",
                "{\"success\":false,\"error\":\"WiFi not connected\"}");
            return;
        }
        Serial.printf("[Portal] WiFi reconnected, ip=%s\n", WiFi.localIP().toString().c_str());
    }

    WiFiClient client;
    if (!client.connect(serverHost.c_str(), serverPort)) {
        portalServer->send(200, "application/json",
            "{\"success\":false,\"error\":\"Cannot reach server\"}");
        return;
    }

    String reqBody = "{\"mac\":\"" + deviceMAC
                   + "\",\"email\":\"" + email
                   + "\",\"password\":\"" + password
                   + "\",\"action\":\"" + act + "\"}";

    String httpReq = "POST /api/devices/register-with-user HTTP/1.1\r\n"
                     "Host: " + serverHost + ":" + String(serverPort) + "\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: " + String(reqBody.length()) + "\r\n"
                     "Connection: close\r\n\r\n" + reqBody;

    client.print(httpReq);

    unsigned long timeout = millis();
    while (client.connected() && !client.available() && millis() - timeout < 10000) {
        delay(10);
    }

    String response = "";
    while (client.available()) {
        response += (char)client.read();
    }
    client.stop();

    int bodyStart = response.indexOf("\r\n\r\n");
    if (bodyStart < 0) {
        portalServer->send(200, "application/json",
            "{\"success\":false,\"error\":\"No response from server\"}");
        return;
    }
    String jsonBody = response.substring(bodyStart + 4);

    bool isError = (response.indexOf(" 4") >= 0 && response.indexOf("HTTP/1.") >= 0);
    if (isError) {
        String detail = extractJson(jsonBody, "\"detail\"");
        if (detail.length() == 0) detail = "Server rejected request";
        portalServer->send(200, "application/json",
            "{\"success\":false,\"error\":\"" + detail + "\"}");
        return;
    }

    if (jsonBody.indexOf("\"paired\"") >= 0 && jsonBody.indexOf("\"token\"") >= 0) {
        String token = extractJson(jsonBody, "\"token\"");
        if (token.length() > 0) {
            deviceToken = token;
            prefs.begin("audio", false);
            prefs.putString("token", deviceToken);
            prefs.end();
            Serial.printf("[Portal] Paired! Token: %s...%s\n",
                          deviceToken.substring(0, 8).c_str(),
                          deviceToken.substring(deviceToken.length() - 4).c_str());
            portalServer->send(200, "application/json", "{\"success\":true}");
            delay(2000);
            ESP.restart();
            return;
        }
    }

    portalServer->send(200, "application/json",
        "{\"success\":false,\"error\":\"Unexpected server response\"}");
}

static void startCaptivePortal() {
    Serial.println("[Portal] Starting captive portal...");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("XiaoAudio-Setup");
    delay(100);

    dnsServer = new DNSServer();
    dnsServer->start(53, "*", WiFi.softAPIP());

    portalServer = new WebServer(80);
    portalServer->on("/", handlePortalRoot);
    portalServer->on("/scan", handlePortalScan);
    portalServer->on("/wifi", HTTP_POST, handlePortalWifi);
    portalServer->on("/wifi-status", handlePortalWifiStatus);
    portalServer->on("/pair", HTTP_POST, handlePortalPair);
    portalServer->onNotFound(handlePortalRoot);
    portalServer->begin();

    portalWifiConnecting = false;
    ledSetPattern(1000, 1000);
    Serial.printf("[Portal] AP IP: %s\n", WiFi.softAPIP().toString().c_str());
}

static void stopCaptivePortal() {
    if (portalServer) { portalServer->stop(); delete portalServer; portalServer = nullptr; }
    if (dnsServer) { dnsServer->stop(); delete dnsServer; dnsServer = nullptr; }
    WiFi.softAPdisconnect(true);
}

// ── 12. URL Parser ───────────────────────────────────────────

static void parseServerUrl(const String &url) {
    // Parse ws://host:port/path
    String work = url;
    if (work.startsWith("ws://")) work = work.substring(5);
    else if (work.startsWith("wss://")) work = work.substring(6);

    int slashIdx = work.indexOf('/');
    String hostPort;
    if (slashIdx >= 0) {
        hostPort = work.substring(0, slashIdx);
        serverPath = work.substring(slashIdx);
    } else {
        hostPort = work;
        serverPath = "/";
    }

    int colonIdx = hostPort.indexOf(':');
    if (colonIdx >= 0) {
        serverHost = hostPort.substring(0, colonIdx);
        serverPort = hostPort.substring(colonIdx + 1).toInt();
    } else {
        serverHost = hostPort;
        serverPort = 80;
    }
}

// ── 13. Pairing Helpers ──────────────────────────────────────

static const char PAIRING_CHARS[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
static const int  PAIRING_CHARS_LEN = 30;

static String generatePairingCode() {
    String code = "";
    for (int i = 0; i < 6; i++) {
        code += PAIRING_CHARS[random(0, PAIRING_CHARS_LEN)];
    }
    return code;
}

static String getMacNoColons() {
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    return mac;
}

static bool registerForPairing() {
    WiFiClient client;
    if (!client.connect(serverHost.c_str(), serverPort)) {
        Serial.println("[Pair] HTTP connect failed");
        return false;
    }

    String body = "{\"mac\":\"" + deviceMAC + "\",\"code\":\"" + pairingCode + "\"}";
    String req = "POST /api/devices/register HTTP/1.1\r\n"
                 "Host: " + serverHost + ":" + String(serverPort) + "\r\n"
                 "Content-Type: application/json\r\n"
                 "Content-Length: " + String(body.length()) + "\r\n"
                 "Connection: close\r\n\r\n" + body;

    client.print(req);

    unsigned long timeout = millis();
    while (client.connected() && !client.available() && millis() - timeout < 5000) {
        delay(10);
    }

    String response = "";
    while (client.available()) {
        response += (char)client.read();
    }
    client.stop();

    return response.indexOf("201") >= 0 || response.indexOf("200") >= 0;
}

static int pollPairingStatus() {
    // Returns: 0 = not paired, 1 = paired (token saved), -1 = error
    WiFiClient client;
    if (!client.connect(serverHost.c_str(), serverPort)) return -1;

    String req = "GET /api/devices/status?mac=" + deviceMAC + " HTTP/1.1\r\n"
                 "Host: " + serverHost + ":" + String(serverPort) + "\r\n"
                 "Connection: close\r\n\r\n";

    client.print(req);

    unsigned long timeout = millis();
    while (client.connected() && !client.available() && millis() - timeout < 5000) {
        delay(10);
    }

    String response = "";
    while (client.available()) {
        response += (char)client.read();
    }
    client.stop();

    // Find the JSON body after headers
    int bodyStart = response.indexOf("\r\n\r\n");
    if (bodyStart < 0) return -1;
    String jsonBody = response.substring(bodyStart + 4);

    if (jsonBody.indexOf("\"paired\": true") >= 0 || jsonBody.indexOf("\"paired\":true") >= 0) {
        // Extract token
        int tokenIdx = jsonBody.indexOf("\"token\"");
        if (tokenIdx >= 0) {
            int colonIdx = jsonBody.indexOf(':', tokenIdx);
            int quoteStart = jsonBody.indexOf('"', colonIdx + 1);
            int quoteEnd = jsonBody.indexOf('"', quoteStart + 1);
            if (quoteStart >= 0 && quoteEnd > quoteStart) {
                deviceToken = jsonBody.substring(quoteStart + 1, quoteEnd);
                prefs.begin("audio", false);
                prefs.putString("token", deviceToken);
                prefs.end();
                Serial.println("[Pair] Paired! Token saved.");
                return 1;
            }
        }
    }
    return 0;
}

// ── 14. WebSocket Callbacks ──────────────────────────────────

static void onWsMessage(WebsocketsMessage msg) {
    if (msg.isText()) {
        String data = msg.data();
        Serial.printf("[WS] Received: %s\n", data.c_str());

        if (data.indexOf("\"auth_ok\"") >= 0) {
            wsAuthOk = true;
            Serial.println("[WS] Authenticated");
        }
        else if (data.indexOf("\"auth_error\"") >= 0) {
            Serial.println("[WS] Auth error -- clearing token");
            prefs.begin("audio", false);
            prefs.remove("token");
            prefs.end();
            deviceToken = "";
            wsConnected = false;
        }
        else if (data.indexOf("\"wifi_update\"") >= 0) {
            // Extract ssid and password, save, reboot
            int ssidStart = data.indexOf("\"ssid\"");
            int passStart = data.indexOf("\"password\"");
            if (ssidStart >= 0 && passStart >= 0) {
                auto extractValue = [](const String &src, int keyStart) -> String {
                    int colon = src.indexOf(':', keyStart);
                    int qs = src.indexOf('"', colon + 1);
                    int qe = src.indexOf('"', qs + 1);
                    return (qs >= 0 && qe > qs) ? src.substring(qs + 1, qe) : "";
                };
                String newSSID = extractValue(data, ssidStart);
                String newPass = extractValue(data, passStart);
                if (newSSID.length() > 0) {
                    prefs.begin("audio", false);
                    prefs.putString("ssid", newSSID);
                    prefs.putString("pass", newPass);
                    prefs.end();
                    Serial.println("[WS] WiFi updated, rebooting...");
                    delay(500);
                    ESP.restart();
                }
            }
        }
        else if (data.indexOf("\"paired\"") >= 0 && data.indexOf("\"token\"") >= 0) {
            auto extractValue = [](const String &src, const char *key) -> String {
                int keyIdx = src.indexOf(key);
                if (keyIdx < 0) return "";
                int colon = src.indexOf(':', keyIdx);
                int qs = src.indexOf('"', colon + 1);
                int qe = src.indexOf('"', qs + 1);
                return (qs >= 0 && qe > qs) ? src.substring(qs + 1, qe) : "";
            };
            String token = extractValue(data, "\"token\"");
            if (token.length() > 0) {
                deviceToken = token;
                prefs.begin("audio", false);
                prefs.putString("token", deviceToken);
                prefs.end();
                Serial.println("[WS] Token updated via WS");
            }
        }
    }
}

static void onWsEvent(WebsocketsEvent event, String data) {
    switch (event) {
        case WebsocketsEvent::ConnectionOpened:
            wsConnected = true;
            Serial.println("[WS] Connection opened");
            break;
        case WebsocketsEvent::ConnectionClosed:
            wsConnected = false;
            wsAuthOk = false;
            Serial.println("[WS] Connection closed");
            break;
        case WebsocketsEvent::GotPing:
            break;
        case WebsocketsEvent::GotPong:
            break;
    }
}

// ── 15. WebSocket Connect ────────────────────────────────────

static bool connectWebSocket() {
    parseServerUrl(serverUrl);

    String wsUrl = serverUrl;
    if (!wsUrl.endsWith("/")) {
        // Ensure path is set
    }

    String fullUrl = "ws://" + serverHost + ":" + String(serverPort)
                   + "/ws/device?token=" + deviceToken
                   + "&mac=" + deviceMAC
                   + "&fw=" + String(FW_VERSION);

    Serial.printf("[WS] Connecting to %s\n", fullUrl.c_str());

    wsClient.onMessage(onWsMessage);
    wsClient.onEvent(onWsEvent);
    wsAuthOk = false;

    bool ok = wsClient.connect(fullUrl);
    if (!ok) {
        Serial.println("[WS] Connection failed");
        wsConnected = false;
    }
    return ok;
}

// ── 16. OTA Check and Perform ────────────────────────────────

struct OtaInfo {
    bool available;
    String version;
    int firmwareId;
    String sha256;
    int size;
};

static OtaInfo checkForOtaUpdate() {
    OtaInfo info = {false, "", 0, "", 0};

    WiFiClient client;
    if (!client.connect(serverHost.c_str(), serverPort)) return info;

    String req = "GET /api/ota/check?mac=" + deviceMAC
               + "&version=" + String(FW_VERSION)
               + "&token=" + deviceToken
               + " HTTP/1.1\r\nHost: " + serverHost + ":" + String(serverPort)
               + "\r\nConnection: close\r\n\r\n";
    client.print(req);

    unsigned long timeout = millis();
    while (client.connected() && !client.available() && millis() - timeout < 10000) {
        delay(10);
    }

    String response = "";
    while (client.available()) {
        response += (char)client.read();
    }
    client.stop();

    int bodyStart = response.indexOf("\r\n\r\n");
    if (bodyStart < 0) return info;
    String body = response.substring(bodyStart + 4);

    if (body.indexOf("\"update\": true") >= 0 || body.indexOf("\"update\":true") >= 0) {
        info.available = true;

        auto extractStr = [](const String &src, const char *key) -> String {
            int idx = src.indexOf(key);
            if (idx < 0) return "";
            int c = src.indexOf(':', idx);
            int qs = src.indexOf('"', c + 1);
            int qe = src.indexOf('"', qs + 1);
            return (qs >= 0 && qe > qs) ? src.substring(qs + 1, qe) : "";
        };
        auto extractInt = [](const String &src, const char *key) -> int {
            int idx = src.indexOf(key);
            if (idx < 0) return 0;
            int c = src.indexOf(':', idx);
            String numStr = "";
            for (int i = c + 1; i < (int)src.length(); i++) {
                char ch = src.charAt(i);
                if (ch >= '0' && ch <= '9') numStr += ch;
                else if (numStr.length() > 0) break;
            }
            return numStr.toInt();
        };

        info.version = extractStr(body, "\"version\"");
        info.firmwareId = extractInt(body, "\"firmware_id\"");
        info.sha256 = extractStr(body, "\"sha256\"");
        info.size = extractInt(body, "\"size\"");

        Serial.printf("[OTA] Update available: v%s, id=%d, size=%d\n",
                      info.version.c_str(), info.firmwareId, info.size);
    }

    return info;
}

static bool performOtaUpdate(const OtaInfo &info) {
    if (WiFi.RSSI() < -80) {
        Serial.println("[OTA] Signal too weak, aborting");
        return false;
    }
    if (ESP.getFreeHeap() < 50000) {
        Serial.println("[OTA] Not enough heap, aborting");
        return false;
    }

    Serial.printf("[OTA] Downloading firmware id=%d (%d bytes)...\n", info.firmwareId, info.size);

    String url = "http://" + serverHost + ":" + String(serverPort)
               + "/api/ota/firmware/" + String(info.firmwareId)
               + "?token=" + deviceToken + "&mac=" + deviceMAC;

    esp_http_client_config_t httpConfig = {};
    httpConfig.url = url.c_str();
    httpConfig.timeout_ms = 30000;

    esp_http_client_handle_t httpClient = esp_http_client_init(&httpConfig);
    if (!httpClient) {
        Serial.println("[OTA] HTTP client init failed");
        return false;
    }

    esp_err_t err = esp_http_client_open(httpClient, 0);
    if (err != ESP_OK) {
        Serial.printf("[OTA] HTTP open failed: %s\n", esp_err_to_name(err));
        esp_http_client_cleanup(httpClient);
        return false;
    }

    int contentLen = esp_http_client_fetch_headers(httpClient);
    if (contentLen <= 0) {
        Serial.println("[OTA] No content received");
        esp_http_client_close(httpClient);
        esp_http_client_cleanup(httpClient);
        return false;
    }

    const esp_partition_t *updatePartition = esp_ota_get_next_update_partition(NULL);
    if (!updatePartition) {
        Serial.println("[OTA] No update partition found");
        esp_http_client_close(httpClient);
        esp_http_client_cleanup(httpClient);
        return false;
    }

    esp_ota_handle_t otaHandle;
    err = esp_ota_begin(updatePartition, OTA_WITH_SEQUENTIAL_WRITES, &otaHandle);
    if (err != ESP_OK) {
        Serial.printf("[OTA] Begin failed: %s\n", esp_err_to_name(err));
        esp_http_client_close(httpClient);
        esp_http_client_cleanup(httpClient);
        return false;
    }

    uint8_t otaBuf[4096];
    int totalRead = 0;
    bool success = true;

    while (totalRead < contentLen) {
        int readLen = esp_http_client_read(httpClient, (char*)otaBuf, sizeof(otaBuf));
        if (readLen <= 0) {
            Serial.println("[OTA] Read error");
            success = false;
            break;
        }

        err = esp_ota_write(otaHandle, otaBuf, readLen);
        if (err != ESP_OK) {
            Serial.printf("[OTA] Write failed: %s\n", esp_err_to_name(err));
            success = false;
            break;
        }

        totalRead += readLen;

        if (WiFi.RSSI() < -80 || ESP.getFreeHeap() < 50000) {
            Serial.println("[OTA] Conditions degraded, aborting");
            success = false;
            break;
        }

        if (totalRead % 32768 == 0) {
            Serial.printf("[OTA] %d / %d bytes\n", totalRead, contentLen);
        }
    }

    esp_http_client_close(httpClient);
    esp_http_client_cleanup(httpClient);

    if (!success) {
        esp_ota_abort(otaHandle);
        return false;
    }

    err = esp_ota_end(otaHandle);
    if (err != ESP_OK) {
        Serial.printf("[OTA] End failed: %s\n", esp_err_to_name(err));
        return false;
    }

    err = esp_ota_set_boot_partition(updatePartition);
    if (err != ESP_OK) {
        Serial.printf("[OTA] Set boot partition failed: %s\n", esp_err_to_name(err));
        return false;
    }

    Serial.println("[OTA] Update complete, rebooting...");
    delay(500);
    ESP.restart();
    return true; // never reached
}

// ── 17. Offline Recording ────────────────────────────────────

static void startOfflineRecording() {
    if (!sdAvailable) {
        Serial.println("[REC] No SD card, cannot record offline");
        return;
    }

    String filename = getPendingFilename();
    recordingFile = SD.open(filename, FILE_WRITE);
    if (!recordingFile) {
        Serial.println("[REC] Failed to open file for recording");
        return;
    }

    // Write placeholder WAV header (44 bytes of zero)
    uint8_t emptyHeader[44] = {0};
    recordingFile.write(emptyHeader, 44);
    recordedBytes = 0;

    appState = STATE_RECORDING_OFFLINE;
    stateEnteredMs = millis();
    ledSetPattern(500, 200);
    Serial.printf("[REC] Started offline recording: %s\n", filename.c_str());
}

static void stopOfflineRecording() {
    if (!recordingFile) return;

    // Finalize WAV header with actual data size
    writeWavHeader(recordingFile, recordedBytes);
    recordingFile.close();

    Serial.printf("[REC] Stopped. Recorded %lu bytes (%.1fs)\n",
                  recordedBytes, (float)recordedBytes / (SAMPLE_RATE * 2));
}

// ── 18. Pending Upload ───────────────────────────────────────

static bool uploadOneFile(const String &path) {
    File f = SD.open(path, FILE_READ);
    if (!f) return false;

    size_t fileSize = f.size();
    if (fileSize < 44) {
        f.close();
        SD.remove(path);
        Serial.printf("[Upload] Deleted corrupt file: %s\n", path.c_str());
        return true;
    }

    WiFiClient client;
    if (!client.connect(serverHost.c_str(), serverPort)) {
        f.close();
        return false;
    }

    String headers = "POST /api/upload HTTP/1.1\r\n"
                     "Host: " + serverHost + ":" + String(serverPort) + "\r\n"
                     "Content-Type: application/octet-stream\r\n"
                     "X-Device-Token: " + deviceToken + "\r\n"
                     "X-Device-MAC: " + deviceMAC + "\r\n"
                     "Content-Length: " + String(fileSize) + "\r\n"
                     "Connection: close\r\n\r\n";
    client.print(headers);

    uint8_t uploadBuf[1024];
    size_t sent = 0;
    while (sent < fileSize) {
        size_t toRead = min((size_t)sizeof(uploadBuf), fileSize - sent);
        size_t bytesRead = f.read(uploadBuf, toRead);
        if (bytesRead == 0) break;
        client.write(uploadBuf, bytesRead);
        sent += bytesRead;

        // Keep WS alive during uploads
        wsClient.poll();
    }
    f.close();

    unsigned long timeout = millis();
    while (client.connected() && !client.available() && millis() - timeout < 10000) {
        delay(10);
    }

    String response = "";
    while (client.available()) {
        response += (char)client.read();
    }
    client.stop();

    if (response.indexOf("201") >= 0) {
        SD.remove(path);
        Serial.printf("[Upload] Uploaded and deleted: %s\n", path.c_str());
        return true;
    }

    Serial.printf("[Upload] Server rejected: %s\n", path.c_str());
    return false;
}

static void uploadPendingRecordings() {
    if (!sdAvailable || !wsConnected) return;

    File dir = SD.open("/pending");
    if (!dir) return;

    File entry = dir.openNextFile();
    while (entry) {
        if (!entry.isDirectory() && String(entry.name()).endsWith(".wav")) {
            String path = String("/pending/") + entry.name();
            entry.close();

            if (!uploadOneFile(path)) {
                dir.close();
                return; // Retry next cycle
            }

            // Check WS is still alive
            wsClient.poll();
            if (!wsConnected) {
                dir.close();
                return;
            }
        } else {
            entry.close();
        }
        entry = dir.openNextFile();
    }
    dir.close();
}

// ── 19. setup() ──────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n[Boot] XIAO ESP32-C3 Audio v" FW_VERSION);

    // LED and button
    pinMode(PIN_LED, OUTPUT);
    pinMode(PIN_BUTTON, INPUT_PULLUP);
    digitalWrite(PIN_LED, LOW);

    // OTA rollback check
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t otaState;
    if (esp_ota_get_state_partition(running, &otaState) == ESP_OK) {
        if (otaState == ESP_OTA_IMG_PENDING_VERIFY) {
            Serial.println("[OTA] Confirming new firmware...");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }

    // Button hold at boot: 3s = WiFi reconfigure, 5s = full factory reset
    if (digitalRead(PIN_BUTTON) == LOW) {
        Serial.println("[Boot] Button held -- 3s: WiFi reset, 5s: factory reset");
        ledSetPattern(100, 100);
        unsigned long holdStart = millis();
        bool didWifiReset = false;
        while (digitalRead(PIN_BUTTON) == LOW && (millis() - holdStart) < 5000) {
            ledUpdate();
            if (!didWifiReset && (millis() - holdStart) >= 3000) {
                didWifiReset = true;
                ledSetPattern(50, 50);
                Serial.println("[Boot] 3s reached -- release now for WiFi reset, keep holding for factory reset");
            }
            delay(10);
        }
        if (millis() - holdStart >= 5000) {
            Serial.println("[Boot] Factory reset!");
            prefs.begin("audio", false);
            prefs.clear();
            prefs.end();
            delay(500);
            ESP.restart();
        } else if (didWifiReset) {
            Serial.println("[Boot] WiFi reset (keeping token)");
            prefs.begin("audio", false);
            prefs.remove("ssid");
            prefs.remove("pass");
            prefs.end();
            delay(500);
            ESP.restart();
        }
    }

    // I2S mic
    setupI2S();

    // SD card
    setupSD();

    // Load NVS
    prefs.begin("audio", true);
    wifiSSID     = prefs.getString("ssid", "");
    wifiPass     = prefs.getString("pass", "");
    serverUrl    = prefs.getString("server", DEFAULT_SERVER_URL);
    deviceToken  = prefs.getString("token", "");
    prefs.end();

    deviceMAC = getMacNoColons();
    parseServerUrl(serverUrl);

    Serial.printf("[NVS] SSID='%s' server='%s' token=%s MAC=%s\n",
                  wifiSSID.c_str(), serverUrl.c_str(),
                  deviceToken.length() > 0 ? "set" : "empty",
                  deviceMAC.c_str());

    // Decide initial state
    if (wifiSSID.length() == 0) {
        appState = STATE_SETUP;
        startCaptivePortal();
    } else {
        appState = STATE_WIFI_CONNECTING;
        wifiRetryCount = 0;
        stateEnteredMs = millis();
        ledSetPattern(200, 200);
    }
}

// ── 20. loop() ───────────────────────────────────────────────

void loop() {
    ledUpdate();
    bool btnPress = buttonWasPressed();
    unsigned long now = millis();

    switch (appState) {

    // ── SETUP (captive portal) ──
    case STATE_SETUP:
        if (dnsServer) dnsServer->processNextRequest();
        if (portalServer) portalServer->handleClient();
        break;

    // ── WIFI CONNECTING ──
    case STATE_WIFI_CONNECTING: {
        bool connected = connectWiFi();
        if (connected) {
            appState = STATE_OTA_CHECK;
            stateEnteredMs = now;
        } else {
            wifiRetryCount++;
            if (wifiRetryCount >= 3) {
                Serial.println("[WiFi] 3 failures, falling back to portal");
                appState = STATE_SETUP;
                startCaptivePortal();
            } else {
                Serial.printf("[WiFi] Retry %d/3...\n", wifiRetryCount);
                delay(2000);
            }
        }
        break;
    }

    // ── OTA CHECK (only when paired) ──
    case STATE_OTA_CHECK: {
        if (deviceToken.length() > 0) {
            OtaInfo ota = checkForOtaUpdate();
            if (ota.available) {
                appState = STATE_OTA_UPDATING;
                ledSetPattern(300, 300);
                performOtaUpdate(ota);
            }
            lastOtaCheckMs = now;
            appState = STATE_WS_CONNECTING;
            stateEnteredMs = now;
            ledSetPattern(150, 150);
        } else {
            Serial.println("[OTA] Skipping -- device not paired yet");
            appState = STATE_CHECK_PAIRED;
            stateEnteredMs = now;
        }
        break;
    }

    // ── CHECK PAIRED (fallback -- pairing normally happens in portal) ──
    case STATE_CHECK_PAIRED:
        appState = STATE_PAIRING;
        pairingCode = generatePairingCode();
        Serial.printf("PAIRING CODE: %s\n", pairingCode.c_str());
        registerForPairing();
        stateEnteredMs = now;
        ledSetPattern(200, 800);
        break;

    // ── PAIRING ──
    case STATE_PAIRING:
        if (now - stateEnteredMs > 3000) {
            stateEnteredMs = now;
            int result = pollPairingStatus();
            if (result == 1) {
                Serial.println("[Pair] Paired successfully!");
                appState = STATE_WS_CONNECTING;
                stateEnteredMs = now;
                ledSetPattern(150, 150);
            } else if (result == -1) {
                Serial.println("[Pair] Poll error, retrying...");
            }
        }
        break;

    // ── WS CONNECTING ──
    case STATE_WS_CONNECTING: {
        bool ok = connectWebSocket();
        if (ok) {
            // Wait briefly for auth_ok
            unsigned long wsWait = millis();
            while (!wsAuthOk && (millis() - wsWait) < 5000) {
                wsClient.poll();
                delay(10);
            }
            if (wsAuthOk) {
                appState = STATE_READY;
                stateEnteredMs = now;
                ledSetPattern(0, 0); // Solid on handled below
                ledOnMs = 1; ledOffMs = 0; // Solid
                Serial.println("[State] READY");
            } else {
                Serial.println("[WS] Auth timeout");
                wsClient.close();
                wsConnected = false;
                delay(5000);
            }
        } else {
            Serial.println("[WS] Connect failed, retry in 5s");
            delay(5000);
        }
        break;
    }

    // ── READY ──
    case STATE_READY:
        wsClient.poll();

        // Periodic WebSocket ping
        if (now - lastPingMs > 15000) {
            lastPingMs = now;
            wsClient.ping();
        }

        // Periodic OTA check (every 1 hour)
        if (now - lastOtaCheckMs > 3600000UL) {
            OtaInfo ota = checkForOtaUpdate();
            if (ota.available) {
                appState = STATE_OTA_UPDATING;
                ledSetPattern(300, 300);
                performOtaUpdate(ota);
                // If fails, stays in READY
                appState = STATE_READY;
            }
            lastOtaCheckMs = now;
        }

        // Upload pending files every 10 seconds
        if (now - lastUploadCheckMs > 10000 && sdAvailable) {
            lastUploadCheckMs = now;
            if (countPendingFiles() > 0) {
                appState = STATE_UPLOADING_PENDING;
                stateEnteredMs = now;
                ledSetPattern(800, 400);
            }
        }

        // WS disconnected?
        if (!wsConnected) {
            Serial.println("[State] WS disconnected, reconnecting...");
            appState = STATE_WS_CONNECTING;
            stateEnteredMs = now;
            ledSetPattern(150, 150);
            break;
        }

        // Button press
        if (btnPress) {
            if (wsConnected) {
                // Start live streaming
                adpcmEncoder.predictor = 0;
                adpcmEncoder.index = 0;
                dcOffset = 0;

                // Send start marker: [sample_rate:u32, 0x00000001:u32]
                uint8_t startMarker[8];
                uint32_t sr = SAMPLE_RATE;
                uint32_t marker = 1;
                memcpy(startMarker, &sr, 4);
                memcpy(startMarker + 4, &marker, 4);
                wsClient.sendBinary((const char*)startMarker, 8);

                appState = STATE_STREAMING;
                stateEnteredMs = now;
                ledSetPattern(50, 50);
                Serial.println("[State] STREAMING");
            } else {
                startOfflineRecording();
            }
        }
        break;

    // ── STREAMING ──
    case STATE_STREAMING:
        wsClient.poll();

        if (readMicChunk()) {
            adpcm_encode_block(pcmChunk, AUDIO_CHUNK_SAMPLES, adpcmBuf);
            wsClient.sendBinary((const char*)adpcmBuf, sizeof(adpcmBuf));
        }

        // Check for WS disconnect
        if (!wsConnected) {
            Serial.println("[Stream] WS disconnected during stream");
            appState = STATE_WS_CONNECTING;
            stateEnteredMs = now;
            ledSetPattern(150, 150);
            break;
        }

        // Button stops streaming
        if (btnPress) {
            // Send stop sentinel
            uint8_t stopSentinel[4] = {0xFF, 0xFF, 0xFF, 0xFF};
            wsClient.sendBinary((const char*)stopSentinel, 4);

            appState = STATE_READY;
            stateEnteredMs = now;
            ledOnMs = 1; ledOffMs = 0;
            Serial.println("[State] Stopped streaming -> READY");
        }
        break;

    // ── RECORDING OFFLINE ──
    case STATE_RECORDING_OFFLINE:
        if (readMicChunk()) {
            if (recordingFile) {
                size_t written = recordingFile.write((uint8_t*)pcmChunk,
                    AUDIO_CHUNK_SAMPLES * sizeof(int16_t));
                recordedBytes += written;
            }
        }

        if (btnPress) {
            stopOfflineRecording();

            // Try reconnecting
            if (connectWiFi()) {
                appState = STATE_WS_CONNECTING;
                stateEnteredMs = now;
                ledSetPattern(150, 150);
            } else {
                appState = STATE_READY;
                stateEnteredMs = now;
                ledOnMs = 1; ledOffMs = 0;
            }
            Serial.println("[State] Stopped offline recording");
        }
        break;

    // ── UPLOADING PENDING ──
    case STATE_UPLOADING_PENDING:
        uploadPendingRecordings();
        appState = STATE_READY;
        stateEnteredMs = now;
        ledOnMs = 1; ledOffMs = 0;
        break;

    // ── OTA UPDATING ──
    case STATE_OTA_UPDATING:
        // performOtaUpdate handles everything and reboots on success
        // If we reach here, it failed
        Serial.println("[OTA] Update failed, returning to ready");
        appState = STATE_READY;
        stateEnteredMs = now;
        ledOnMs = 1; ledOffMs = 0;
        break;
    }
}
