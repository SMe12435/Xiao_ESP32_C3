// XIAO ESP32-C3 Audio Device Firmware
// Captures I2S mic audio, streams via WebSocket, records to SD when offline.

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ArduinoWebsockets.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <esp_ota_ops.h>
#include <esp_http_client.h>
#include <esp_partition.h>
#include <driver/i2s.h>
#include <Adafruit_NeoPixel.h>
#include <SPI.h>
#include <SD.h>

using namespace websockets;

// ── Pin Definitions ─────────────────────────────────────
#define PIN_I2S_WS    2
#define PIN_I2S_SCK   3
#define PIN_I2S_DATA  4
#define PIN_BUTTON    5
#define PIN_LED       6
#define PIN_SD_CS     7
#define PIN_SD_SCK    8
#define PIN_SD_MISO   9
#define PIN_SD_MOSI   10

// ── Defaults ────────────────────────────────────────────
static const char* DEFAULT_SERVER = "ws://15.206.232.216:8888/ws/device";
static const char* AP_SSID        = "XiaoAudio-Setup";

// ── Audio ───────────────────────────────────────────────
static constexpr uint32_t SAMPLE_RATE = 16000;
static constexpr size_t   MIC_CHUNK  = 512;

// ── NeoPixel LED ────────────────────────────────────────
static Adafruit_NeoPixel pixel(1, PIN_LED, NEO_GRB + NEO_KHZ800);

//  Color map (0xRRGGBB, moderate brightness)
//
//  STATE                  COLOR        PATTERN
//  ─────────────────────  ───────────  ──────────────
//  Captive Portal/Setup   BLUE         Slow blink 1s
//  WiFi Connecting        YELLOW       Fast blink 200ms
//  Pairing                CYAN         Double blink
//  WebSocket Connecting   PURPLE       Breathing
//  Ready (idle)           GREEN        Solid
//  Live Streaming         RED          Rapid pulse
//  Recording Offline      ORANGE       Slow blink
//  Uploading Files        WHITE        Breathing
//  OTA Check/Update       CYAN         Breathing
//  Factory Reset          RED          Solid bright
//  Error                  RED          3x flash

#define COLOR_OFF     0x000000
#define COLOR_RED     0x600000
#define COLOR_GREEN   0x006000
#define COLOR_BLUE    0x000060
#define COLOR_YELLOW  0x604000
#define COLOR_CYAN    0x006060
#define COLOR_PURPLE  0x300060
#define COLOR_ORANGE  0x602000
#define COLOR_WHITE   0x606060

enum LedPattern {
    PAT_OFF, PAT_SOLID, PAT_SLOW_BLINK, PAT_FAST_BLINK,
    PAT_DOUBLE_BLINK, PAT_RAPID_PULSE, PAT_BREATHE
};

static uint32_t      ledColor = 0;
static LedPattern    ledPat = PAT_OFF;
static unsigned long ledTimer = 0;
static bool          ledPhaseOn = false;
static int           ledPhase = 0;

static void setLed(uint32_t color, LedPattern pat) {
    ledColor = color;
    ledPat = pat;
    ledTimer = millis();
    ledPhaseOn = true;
    ledPhase = 0;
    if (pat == PAT_SOLID) { pixel.setPixelColor(0, color); pixel.show(); }
    else if (pat == PAT_OFF) { pixel.setPixelColor(0, 0); pixel.show(); }
}

static void updateLed() {
    unsigned long now = millis();
    switch (ledPat) {
    case PAT_OFF:
    case PAT_SOLID:
        return;
    case PAT_SLOW_BLINK:
        if (now - ledTimer >= 1000) {
            ledTimer = now; ledPhaseOn = !ledPhaseOn;
            pixel.setPixelColor(0, ledPhaseOn ? ledColor : (uint32_t)COLOR_OFF);
            pixel.show();
        }
        break;
    case PAT_FAST_BLINK:
        if (now - ledTimer >= 200) {
            ledTimer = now; ledPhaseOn = !ledPhaseOn;
            pixel.setPixelColor(0, ledPhaseOn ? ledColor : (uint32_t)COLOR_OFF);
            pixel.show();
        }
        break;
    case PAT_DOUBLE_BLINK: {
        static const uint16_t t[] = {120, 100, 120, 700};
        if (now - ledTimer >= t[ledPhase % 4]) {
            ledTimer = now;
            ledPhase++;
            bool on = (ledPhase % 4 == 0 || ledPhase % 4 == 2);
            pixel.setPixelColor(0, on ? ledColor : (uint32_t)COLOR_OFF);
            pixel.show();
        }
        break;
    }
    case PAT_RAPID_PULSE:
        if (now - ledTimer >= 80) {
            ledTimer = now; ledPhaseOn = !ledPhaseOn;
            pixel.setPixelColor(0, ledPhaseOn ? ledColor : (uint32_t)COLOR_OFF);
            pixel.show();
        }
        break;
    case PAT_BREATHE:
        if (now - ledTimer >= 30) {
            ledTimer = now;
            uint16_t phase = (now / 5) % 512;
            uint8_t val = phase < 256 ? phase : 511 - phase;
            uint8_t r = (uint16_t)((ledColor >> 16) & 0xFF) * val / 255;
            uint8_t g = (uint16_t)((ledColor >> 8) & 0xFF) * val / 255;
            uint8_t b = (uint16_t)(ledColor & 0xFF) * val / 255;
            pixel.setPixelColor(0, pixel.Color(r, g, b));
            pixel.show();
        }
        break;
    }
}

static Preferences prefs;

// ── Button ──────────────────────────────────────────────
static bool     btnLast = HIGH;
static bool     btnDown = false;
static unsigned long btnDebounce = 0;
static unsigned long btnPressStart = 0;
static bool     btnResetTriggered = false;

static bool checkButton() {
    bool state = digitalRead(PIN_BUTTON);
    if (state != btnLast) { btnDebounce = millis(); btnLast = state; }
    if (millis() - btnDebounce > 50) {
        if (state == LOW && !btnDown) {
            btnDown = true;
            btnPressStart = millis();
            btnResetTriggered = false;
            return true;
        }
        if (state == HIGH) { btnDown = false; btnResetTriggered = false; }
    }
    return false;
}

static unsigned long resetHoldStart = 0;
static bool resetHolding = false;

static void checkLongPressReset() {
    bool pressed = (digitalRead(PIN_BUTTON) == LOW);
    if (pressed && !resetHolding) {
        resetHolding = true;
        resetHoldStart = millis();
    } else if (!pressed) {
        resetHolding = false;
    }
    if (resetHolding && (millis() - resetHoldStart) >= 7000) {
        Serial.println("[BTN] 7s long-press -- FACTORY RESET");
        setLed(COLOR_RED, PAT_SOLID);
        prefs.begin("audio", false);
        prefs.clear();
        prefs.end();
        delay(1000);
        ESP.restart();
    }
}

// ── IMA ADPCM ───────────────────────────────────────────
static const int16_t ADPCM_STEP[89] = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,
    50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,
    230,253,279,307,337,371,408,449,494,544,598,658,724,796,
    876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,
    2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,
    7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,
    20350,22385,24623,27086,29794,32767
};
static const int8_t ADPCM_IDX[16] = {
    -1,-1,-1,-1, 2,4,6,8, -1,-1,-1,-1, 2,4,6,8
};

struct AdpcmState { int16_t pred; int8_t idx; };
static AdpcmState adpcm;

static uint8_t adpcm_enc(int16_t sample) {
    int step = ADPCM_STEP[adpcm.idx];
    int diff = sample - adpcm.pred;
    uint8_t nib = 0;
    if (diff < 0) { nib = 8; diff = -diff; }
    if (diff >= step)        { nib |= 4; diff -= step; }
    if (diff >= (step >> 1)) { nib |= 2; diff -= (step >> 1); }
    if (diff >= (step >> 2)) { nib |= 1; }

    int delta = step >> 3;
    if (nib & 4) delta += step;
    if (nib & 2) delta += step >> 1;
    if (nib & 1) delta += step >> 2;

    adpcm.pred += (nib & 8) ? -delta : delta;
    if (adpcm.pred > 32767)  adpcm.pred = 32767;
    if (adpcm.pred < -32768) adpcm.pred = -32768;

    adpcm.idx += ADPCM_IDX[nib];
    if (adpcm.idx < 0)  adpcm.idx = 0;
    if (adpcm.idx > 88) adpcm.idx = 88;
    return nib;
}

static void adpcm_block(const int16_t* pcm, uint8_t* out, size_t n) {
    for (size_t i = 0; i < n; i += 2) {
        uint8_t lo = adpcm_enc(pcm[i]);
        uint8_t hi = adpcm_enc(pcm[i + 1]);
        out[i / 2] = (hi << 4) | (lo & 0x0F);
    }
}

// ── State Machine ───────────────────────────────────────
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

static const char* stateNames[] = {
    "SETUP", "WIFI_CONNECTING", "OTA_CHECK", "CHECK_PAIRED",
    "PAIRING", "WS_CONNECTING", "READY", "STREAMING",
    "RECORDING_OFFLINE", "UPLOADING_PENDING", "OTA_UPDATING"
};

static volatile AppState appState = STATE_SETUP;

static void setState(AppState s) {
    Serial.printf("[STATE] %s -> %s\n", stateNames[(int)appState], stateNames[(int)s]);
    appState = s;
}

// ── Globals ─────────────────────────────────────────────
static int32_t  i2sRaw[MIC_CHUNK];
static int16_t  pcmChunk[MIC_CHUNK];
static uint8_t  adpcmBuf[MIC_CHUNK / 2];
static int32_t  dcOffset = 0;
static unsigned long streamStartMs = 0;
static bool i2sReady = false;
static bool sdAvailable = false;

static WebsocketsClient wsClient;
static bool wsConnected = false;

static String cfgSsid;
static String cfgPassword;
static String cfgServerUrl;
static String deviceToken;
static String pairingCode;
static String deviceMac;
static String cfgEmail;
static String cfgAcctPass;
static String cfgAction;

static unsigned long lastWsReconnect = 0;
static constexpr unsigned long WS_RECONNECT_INTERVAL = 5000;

static unsigned long lastPairingPoll = 0;
static constexpr unsigned long PAIRING_POLL_INTERVAL = 3000;

static int wifiRetryCount = 0;
static constexpr int MAX_WIFI_RETRIES = 3;

static unsigned long lastWsPing = 0;
static constexpr unsigned long WS_PING_INTERVAL = 15000;

static unsigned long lastOtaCheck = 0;
static constexpr unsigned long OTA_CHECK_INTERVAL = 3600000;

static unsigned long lastUploadCheck = 0;
static constexpr unsigned long UPLOAD_CHECK_INTERVAL = 10000;

static String otaFirmwareUrl;
static String otaSha256;
static String otaVersion;
static int    otaFileSize = 0;

// SD recording state
static File    recFile;
static uint32_t recBytes = 0;

// ── I2S Init ────────────────────────────────────────────
static bool initI2S() {
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    cfg.sample_rate = SAMPLE_RATE;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 8;
    cfg.dma_buf_len = 128;
    cfg.use_apll = false;
    cfg.tx_desc_auto_clear = false;
    cfg.fixed_mclk = 0;

    esp_err_t err = i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[I2S] Driver install failed: %s\n", esp_err_to_name(err));
        return false;
    }

    i2s_pin_config_t pins = {};
    pins.bck_io_num   = PIN_I2S_SCK;
    pins.ws_io_num    = PIN_I2S_WS;
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num  = PIN_I2S_DATA;

    err = i2s_set_pin(I2S_NUM_0, &pins);
    if (err != ESP_OK) {
        Serial.printf("[I2S] Pin config failed: %s\n", esp_err_to_name(err));
        return false;
    }

    Serial.println("[I2S] Initialized: 16kHz 32-bit mono");
    return true;
}

static size_t readMic() {
    size_t bytesRead = 0;
    i2s_read(I2S_NUM_0, i2sRaw, MIC_CHUNK * sizeof(int32_t),
             &bytesRead, pdMS_TO_TICKS(100));
    size_t samples = bytesRead / sizeof(int32_t);
    for (size_t i = 0; i < samples; i++)
        pcmChunk[i] = (int16_t)(i2sRaw[i] >> 11);
    return samples;
}

// ── SD Card Init ────────────────────────────────────────
static bool initSD() {
    SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    if (!SD.begin(PIN_SD_CS, SPI)) {
        Serial.println("[SD] Card not found");
        return false;
    }
    Serial.printf("[SD] Card mounted, size: %lluMB\n", SD.cardSize() / (1024 * 1024));
    if (!SD.exists("/pending")) {
        SD.mkdir("/pending");
        Serial.println("[SD] Created /pending directory");
    }
    return true;
}

// ── DC Offset Removal ───────────────────────────────────
static void removeDC(int16_t* buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        dcOffset += ((int32_t)buf[i] - dcOffset) >> 8;
        int32_t v = (int32_t)buf[i] - dcOffset;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        buf[i] = (int16_t)v;
    }
}

// ── WAV Header ──────────────────────────────────────────
static void writeWavHeader(File& f, uint32_t dataSize) {
    uint32_t fileSize = dataSize + 36;
    uint32_t sr = SAMPLE_RATE;
    uint32_t byteRate = sr * 2;
    uint16_t blockAlign = 2;
    uint16_t bps = 16;
    uint16_t ch = 1;
    uint16_t fmt = 1;
    uint32_t fmtSz = 16;

    f.write((const uint8_t*)"RIFF", 4);
    f.write((const uint8_t*)&fileSize, 4);
    f.write((const uint8_t*)"WAVE", 4);
    f.write((const uint8_t*)"fmt ", 4);
    f.write((const uint8_t*)&fmtSz, 4);
    f.write((const uint8_t*)&fmt, 2);
    f.write((const uint8_t*)&ch, 2);
    f.write((const uint8_t*)&sr, 4);
    f.write((const uint8_t*)&byteRate, 4);
    f.write((const uint8_t*)&blockAlign, 2);
    f.write((const uint8_t*)&bps, 2);
    f.write((const uint8_t*)"data", 4);
    f.write((const uint8_t*)&dataSize, 4);
}

// ── Pairing Code ────────────────────────────────────────
static const char PAIR_CHARS[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";

static String genPairCode() {
    String code;
    for (int i = 0; i < 6; i++)
        code += PAIR_CHARS[esp_random() % (sizeof(PAIR_CHARS) - 1)];
    return code;
}

// ── WebSocket Callbacks ─────────────────────────────────
static void onWsMessage(WebsocketsMessage msg) {
    String data = msg.data();
    Serial.printf("[WS] recv: %s\n", data.c_str());

    if (data.indexOf("\"auth_ok\"") >= 0) {
        Serial.println("[WS] Authenticated OK");
    } else if (data.indexOf("\"wifi_update\"") >= 0) {
        int ss = data.indexOf("\"ssid\":\"") + 8;
        int se = data.indexOf("\"", ss);
        int ps = data.indexOf("\"password\":\"") + 12;
        int pe = data.indexOf("\"", ps);
        if (ss > 7 && se > ss && ps > 11 && pe > ps) {
            String newSsid = data.substring(ss, se);
            String newPass = data.substring(ps, pe);
            Serial.printf("[WS] WiFi update: ssid='%s'\n", newSsid.c_str());
            prefs.begin("audio", false);
            prefs.putString("ssid", newSsid);
            prefs.putString("pass", newPass);
            prefs.end();
            setLed(COLOR_GREEN, PAT_SOLID);
            delay(2000);
            ESP.restart();
        }
    } else if (data.indexOf("\"paired\"") >= 0) {
        int ts = data.indexOf("\"token\":\"") + 9;
        int te = data.indexOf("\"", ts);
        if (ts > 8 && te > ts) {
            deviceToken = data.substring(ts, te);
            prefs.begin("audio", false);
            prefs.putString("token", deviceToken);
            prefs.end();
            Serial.printf("[WS] Paired! Token: %s...\n", deviceToken.substring(0, 8).c_str());
            setLed(COLOR_GREEN, PAT_SOLID);
            delay(1500);
            setState(STATE_WS_CONNECTING);
        }
    }
}

static void onWsEvent(WebsocketsEvent event, String data) {
    switch (event) {
    case WebsocketsEvent::ConnectionOpened:
        Serial.println("[WS] Connected");
        wsConnected = true;
        break;
    case WebsocketsEvent::ConnectionClosed:
        Serial.printf("[WS] Disconnected (state=%s, heap=%u, RSSI=%d)\n",
            stateNames[(int)appState], (unsigned)ESP.getFreeHeap(), WiFi.RSSI());
        wsConnected = false;
        break;
    case WebsocketsEvent::GotPing:
    case WebsocketsEvent::GotPong:
        break;
    }
}

// ── HTML Escape ─────────────────────────────────────────
static String htmlEsc(const String& s) {
    String o;
    o.reserve(s.length() + 8);
    for (unsigned int i = 0; i < s.length(); i++) {
        char c = s.charAt(i);
        switch (c) {
            case '&':  o += "&amp;";  break;
            case '<':  o += "&lt;";   break;
            case '>':  o += "&gt;";   break;
            case '"':  o += "&quot;"; break;
            case '\'': o += "&#39;";  break;
            default:   o += c;
        }
    }
    return o;
}

// ── Captive Portal ──────────────────────────────────────
static WebServer*  portalServer = nullptr;
static DNSServer*  portalDns    = nullptr;
static String      portalHtml;

static String buildSetupPage(const String& nets) {
    String h = R"rawliteral(<!DOCTYPE html><html><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>XIAO Audio Setup</title><style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,system-ui,sans-serif;background:#0f1117;color:#e4e6ef;
min-height:100vh;display:flex;justify-content:center;padding:20px}
.card{background:#1a1d27;border:1px solid #2e3144;border-radius:12px;padding:28px 24px;
width:100%;max-width:400px;margin-top:10px}
h2{text-align:center;margin-bottom:6px;font-size:22px}
.sub{text-align:center;color:#8b8fa3;font-size:14px;margin-bottom:20px}
label{display:block;font-size:13px;color:#8b8fa3;margin:14px 0 6px;font-weight:500}
input,select{width:100%;padding:10px 14px;background:#242736;border:1px solid #2e3144;
border-radius:8px;color:#e4e6ef;font-size:15px;outline:none}
input:focus{border-color:#6c63ff}
.nets{display:flex;flex-wrap:wrap;gap:6px;margin-top:8px}
.net{padding:6px 12px;background:#242736;border:1px solid #2e3144;border-radius:6px;
font-size:13px;cursor:pointer;transition:border-color .2s}
.net:hover{border-color:#6c63ff;color:#6c63ff}
.rssi{color:#8b8fa3;font-size:11px;margin-left:4px}
details{margin-top:14px}
summary{color:#8b8fa3;font-size:13px;cursor:pointer}
.btn{width:100%;padding:12px;margin-top:20px;background:#6c63ff;color:#fff;border:none;
border-radius:8px;font-size:16px;font-weight:500;cursor:pointer}
.btn:hover{background:#5a52e0}
.step{display:none}.step.active{display:block}
.steps{display:flex;gap:8px;margin-bottom:20px}
.dot{flex:1;height:4px;border-radius:2px;background:#2e3144}
.dot.done{background:#6c63ff}
.toggle{text-align:center;margin-top:12px;font-size:13px;color:#8b8fa3}
.toggle a{color:#6c63ff;cursor:pointer;text-decoration:none}
</style></head><body>
<div class="card">
<h2>XIAO Audio Setup</h2>
<div class="steps"><div class="dot done" id="d1"></div><div class="dot" id="d2"></div></div>
<form method="POST" action="/save">
<div class="step active" id="s1">
<p class="sub">Step 1: Connect to WiFi</p>
<label>WiFi Network</label>
<input type="text" name="ssid" id="ssid" placeholder="Type or tap below" required>
<div class="nets">)rawliteral";
    h += nets;
    h += R"rawliteral(</div>
<label>Password</label>
<input type="password" name="password" placeholder="WiFi password">
<details><summary>Advanced Settings</summary>
<label>Server URL</label>
<input name="server" value=")rawliteral";
    h += cfgServerUrl.length() > 0 ? cfgServerUrl : String(DEFAULT_SERVER);
    h += R"rawliteral(">
</details>
<div class="btn" onclick="next()">Next &rarr;</div>
</div>
<div class="step" id="s2">
<p class="sub">Step 2: Link your account</p>
<label>Email</label>
<input type="email" name="email" placeholder="you@example.com" required>
<label>Password</label>
<input type="password" name="acct_pass" placeholder="Account password">
<input type="hidden" name="action" id="actField" value="login">
<div class="toggle" id="togWrap">
<span id="togText">No account?</span>
<a onclick="togMode();" id="togLink">Sign up</a>
</div>
<button type="submit" class="btn">Save &amp; Connect</button>
</div>
</form></div>
<script>
var isSignup=false;
function next(){
 var s=document.getElementById('ssid').value;
 if(!s){alert('Pick a WiFi network');return;}
 document.getElementById('s1').classList.remove('active');
 document.getElementById('s2').classList.add('active');
 document.getElementById('d2').classList.add('done');
}
function togMode(){
 isSignup=!isSignup;
 document.getElementById('actField').value=isSignup?'signup':'login';
 document.getElementById('togText').textContent=isSignup?'Have an account?':'No account?';
 document.getElementById('togLink').textContent=isSignup?'Sign in':'Sign up';
}
document.querySelectorAll('.net').forEach(function(el){
 el.onclick=function(){document.getElementById('ssid').value=this.dataset.ssid;};
});
</script></body></html>)rawliteral";
    return h;
}

static void handlePortalRoot() {
    portalServer->send(200, "text/html", portalHtml);
}

static void handlePortalSave() {
    String ssid     = portalServer->arg("ssid");
    String pass     = portalServer->arg("password");
    String server   = portalServer->arg("server");
    String email    = portalServer->arg("email");
    String acctPass = portalServer->arg("acct_pass");
    String action   = portalServer->arg("action");
    if (action.length() == 0) action = "login";
    Serial.printf("[PORTAL] Save: ssid='%s' server='%s' email='%s' action='%s'\n",
        ssid.c_str(), server.c_str(), email.c_str(), action.c_str());

    prefs.begin("audio", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    if (server.length() > 0) prefs.putString("server", server);
    prefs.putString("email", email);
    prefs.putString("apass", acctPass);
    prefs.putString("action", action);
    prefs.end();

    portalServer->send(200, "text/html",
        "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<style>body{font-family:sans-serif;background:#0f1117;color:#e4e6ef;"
        "display:flex;justify-content:center;align-items:center;min-height:100vh;"
        "text-align:center}.ok{font-size:48px;margin-bottom:16px}</style></head>"
        "<body><div><div class='ok'>&#10003;</div><h2>Saved!</h2>"
        "<p style='color:#8b8fa3;margin-top:8px'>Connecting &amp; pairing...</p>"
        "</div></body></html>");

    setLed(COLOR_GREEN, PAT_SOLID);
    Serial.println("[PORTAL] Saved, rebooting...");
    delay(1500);
    ESP.restart();
}

static void startCaptivePortal() {
    Serial.println("[PORTAL] Starting captive portal...");
    WiFi.mode(WIFI_AP_STA);
    delay(200);

    Serial.println("[PORTAL] Scanning WiFi networks...");
    int n = WiFi.scanNetworks();
    String netItems;
    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;
        String esc = htmlEsc(ssid);
        netItems += "<div class=\"net\" data-ssid=\"" + esc + "\">"
                  + esc + "<span class=\"rssi\">" + String(WiFi.RSSI(i)) + "dB</span></div>";
    }
    WiFi.scanDelete();
    if (n == 0) netItems = "<div style='color:#8b8fa3;font-size:13px'>No networks found.</div>";

    portalHtml = buildSetupPage(netItems);

    WiFi.softAP(AP_SSID);
    delay(500);
    Serial.printf("[PORTAL] AP: %s  IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());

    portalDns = new DNSServer();
    portalDns->start(53, "*", WiFi.softAPIP());

    portalServer = new WebServer(80);
    portalServer->on("/", HTTP_GET, handlePortalRoot);
    portalServer->on("/save", HTTP_POST, handlePortalSave);
    portalServer->onNotFound(handlePortalRoot);
    portalServer->begin();

    setLed(COLOR_BLUE, PAT_SLOW_BLINK);
}

// ── WiFi Connection ─────────────────────────────────────
static bool connectWiFi() {
    Serial.printf("[WIFI] Connecting to '%s'...\n", cfgSsid.c_str());
    setLed(COLOR_YELLOW, PAT_FAST_BLINK);

    WiFi.mode(WIFI_STA);
    delay(500);
    esp_wifi_set_protocol(WIFI_IF_STA,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);

    wifi_config_t wcfg = {};
    strncpy((char*)wcfg.sta.ssid, cfgSsid.c_str(), sizeof(wcfg.sta.ssid));
    strncpy((char*)wcfg.sta.password, cfgPassword.c_str(), sizeof(wcfg.sta.password));
    wcfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wcfg.sta.pmf_cfg.capable = true;
    wcfg.sta.pmf_cfg.required = false;
    esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    esp_wifi_connect();

    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 40) {
        delay(500);
        Serial.print(".");
        tries++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        deviceMac = WiFi.macAddress();
        deviceMac.replace(":", "");
        Serial.printf("[WIFI] Connected! IP: %s  MAC: %s  RSSI: %d\n",
            WiFi.localIP().toString().c_str(), deviceMac.c_str(), WiFi.RSSI());
        return true;
    }
    Serial.println("[WIFI] Connection failed");
    return false;
}

// ── WebSocket ───────────────────────────────────────────
static bool connectWebSocket() {
    String url = cfgServerUrl + "?token=" + deviceToken
               + "&mac=" + deviceMac + "&fw=" + FW_VERSION;
    Serial.printf("[WS] Connecting: %s\n", url.c_str());
    return wsClient.connect(url);
}

// ── URL Parser ──────────────────────────────────────────
static void parseServerUrl(String& host, int& port) {
    String url = cfgServerUrl;
    int hs = url.indexOf("://") + 3;
    int ps = url.indexOf(":", hs);
    int pp = url.indexOf("/", hs);
    host = url.substring(hs, ps > 0 ? ps : pp);
    port = 8888;
    if (ps > 0 && pp > ps) port = url.substring(ps + 1, pp).toInt();
}

// ── HTTP: Pairing ───────────────────────────────────────
static bool registerForPairing() {
    WiFiClient http;
    String host; int port;
    parseServerUrl(host, port);
    if (!http.connect(host.c_str(), port)) {
        Serial.println("[PAIR] HTTP connect failed");
        return false;
    }
    String body = "{\"mac\":\"" + deviceMac + "\",\"code\":\"" + pairingCode + "\"}";
    http.printf("POST /api/devices/register HTTP/1.1\r\n"
                "Host: %s:%d\r\nContent-Type: application/json\r\n"
                "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
                host.c_str(), port, body.length(), body.c_str());
    unsigned long t = millis();
    while (!http.available() && millis() - t < 5000) delay(10);
    String resp;
    while (http.available()) resp += (char)http.read();
    http.stop();
    Serial.printf("[PAIR] Register response: %s\n", resp.substring(0, 100).c_str());
    return resp.indexOf("200") >= 0 || resp.indexOf("201") >= 0;
}

static String pollPairingStatus() {
    WiFiClient http;
    String host; int port;
    parseServerUrl(host, port);
    if (!http.connect(host.c_str(), port)) return "";
    String path = "/api/devices/status?mac=" + deviceMac;
    http.printf("GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n\r\n",
                path.c_str(), host.c_str(), port);
    unsigned long t = millis();
    while (!http.available() && millis() - t < 5000) delay(10);
    String resp;
    while (http.available()) resp += (char)http.read();
    http.stop();
    int ts = resp.indexOf("\"token\":\"") + 9;
    int te = resp.indexOf("\"", ts);
    if (ts > 8 && te > ts) return resp.substring(ts, te);
    return "";
}

// ── Register With User (2-step AP) ──────────────────────
static bool registerWithUser() {
    if (cfgEmail.length() == 0) return false;
    WiFiClient http;
    String host; int port;
    parseServerUrl(host, port);
    if (!http.connect(host.c_str(), port)) {
        Serial.println("[PAIR] register-with-user: connect failed");
        return false;
    }
    String body = "{\"mac\":\"" + deviceMac
                + "\",\"email\":\"" + cfgEmail
                + "\",\"password\":\"" + cfgAcctPass
                + "\",\"action\":\"" + cfgAction + "\"}";
    http.printf("POST /api/devices/register-with-user HTTP/1.1\r\n"
                "Host: %s:%d\r\nContent-Type: application/json\r\n"
                "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
                host.c_str(), port, body.length(), body.c_str());
    unsigned long t = millis();
    while (!http.available() && millis() - t < 8000) delay(10);
    String resp;
    while (http.available()) resp += (char)http.read();
    http.stop();
    Serial.printf("[PAIR] register-with-user resp: %s\n", resp.substring(0, 150).c_str());

    int ts = resp.indexOf("\"token\":\"") + 9;
    int te = resp.indexOf("\"", ts);
    if (ts > 8 && te > ts) {
        deviceToken = resp.substring(ts, te);
        prefs.begin("audio", false);
        prefs.putString("token", deviceToken);
        prefs.remove("email");
        prefs.remove("apass");
        prefs.remove("action");
        prefs.end();
        cfgEmail = "";
        cfgAcctPass = "";
        Serial.printf("[PAIR] Auto-paired! Token: %s...\n", deviceToken.substring(0, 8).c_str());
        return true;
    }
    Serial.println("[PAIR] register-with-user: no token in response");
    return false;
}

// ── OTA Check ───────────────────────────────────────────
static bool checkForOta() {
    WiFiClient http;
    String host; int port;
    parseServerUrl(host, port);
    if (!http.connect(host.c_str(), port)) {
        Serial.println("[OTA] Check connect failed");
        return false;
    }
    String path = "/api/ota/check?mac=" + deviceMac
                + "&version=" + String(FW_VERSION) + "&token=" + deviceToken;
    http.printf("GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n\r\n",
                path.c_str(), host.c_str(), port);
    unsigned long t = millis();
    while (!http.available() && millis() - t < 5000) delay(10);
    String resp;
    while (http.available()) resp += (char)http.read();
    http.stop();
    Serial.printf("[OTA] Check: %s\n", resp.substring(0, 120).c_str());

    if (resp.indexOf("\"update\":true") < 0) return false;

    int fs = resp.indexOf("\"firmware_id\":") + 14;
    int fe = resp.indexOf(",", fs); if (fe < 0) fe = resp.indexOf("}", fs);
    String fid = resp.substring(fs, fe); fid.trim();

    int vs = resp.indexOf("\"version\":\"") + 11;
    int ve = resp.indexOf("\"", vs);
    otaVersion = resp.substring(vs, ve);

    int ss = resp.indexOf("\"sha256\":\"") + 10;
    int se = resp.indexOf("\"", ss);
    otaSha256 = resp.substring(ss, se);

    int szs = resp.indexOf("\"size\":") + 7;
    int sze = resp.indexOf("}", szs);
    String szStr = resp.substring(szs, sze); szStr.trim();
    otaFileSize = szStr.toInt();

    otaFirmwareUrl = "/api/ota/firmware/" + fid + "?token=" + deviceToken + "&mac=" + deviceMac;
    Serial.printf("[OTA] Available: v%s, %d bytes\n", otaVersion.c_str(), otaFileSize);
    return true;
}

// ── OTA Perform ─────────────────────────────────────────
static bool performOta() {
    if (WiFi.RSSI() < -80) {
        Serial.println("[OTA] Aborted: weak WiFi");
        return false;
    }
    if (ESP.getFreeHeap() < 50000) {
        Serial.println("[OTA] Aborted: low heap");
        return false;
    }

    String host; int port;
    parseServerUrl(host, port);
    String url = "http://" + host + ":" + String(port) + otaFirmwareUrl;
    Serial.printf("[OTA] Downloading: %s\n", url.c_str());
    setLed(COLOR_CYAN, PAT_BREATHE);

    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = 60000;
    cfg.buffer_size = 4096;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { Serial.println("[OTA] Client init failed"); return false; }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        Serial.printf("[OTA] Open failed: %s\n", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    int contentLen = esp_http_client_fetch_headers(client);
    if (contentLen <= 0) {
        Serial.printf("[OTA] Bad content length: %d\n", contentLen);
        esp_http_client_cleanup(client);
        return false;
    }

    const esp_partition_t* part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        Serial.println("[OTA] No update partition");
        esp_http_client_cleanup(client);
        return false;
    }

    esp_ota_handle_t handle;
    err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        Serial.printf("[OTA] Begin failed: %s\n", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    uint8_t buf[4096];
    int total = 0, lastPct = -1;
    while (true) {
        int rd = esp_http_client_read(client, (char*)buf, sizeof(buf));
        if (rd < 0) {
            Serial.println("[OTA] Read error");
            esp_ota_abort(handle);
            esp_http_client_cleanup(client);
            return false;
        }
        if (rd == 0) break;
        err = esp_ota_write(handle, buf, rd);
        if (err != ESP_OK) {
            Serial.printf("[OTA] Write failed: %s\n", esp_err_to_name(err));
            esp_ota_abort(handle);
            esp_http_client_cleanup(client);
            return false;
        }
        total += rd;
        int pct = (int)((int64_t)total * 100 / contentLen);
        if (pct != lastPct) {
            Serial.printf("[OTA] Progress: %d%%\n", pct);
            lastPct = pct;
        }
    }
    esp_http_client_cleanup(client);

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        Serial.printf("[OTA] End failed: %s\n", esp_err_to_name(err));
        return false;
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        Serial.printf("[OTA] Set boot partition failed: %s\n", esp_err_to_name(err));
        return false;
    }

    Serial.printf("[OTA] Success! %d bytes. Rebooting into v%s...\n", total, otaVersion.c_str());
    setLed(COLOR_GREEN, PAT_SOLID);
    delay(1500);
    ESP.restart();
    return true;
}

// ── SD Upload ───────────────────────────────────────────
static bool uploadWavFile(const char* path) {
    File f = SD.open(path);
    if (!f) { Serial.printf("[UPLOAD] Can't open %s\n", path); return false; }
    size_t fsize = f.size();
    if (fsize < 44) {
        f.close(); SD.remove(path);
        Serial.printf("[UPLOAD] Deleted corrupt file %s (%u bytes)\n", path, (unsigned)fsize);
        return true;
    }

    String host; int port;
    parseServerUrl(host, port);
    WiFiClient http;
    if (!http.connect(host.c_str(), port)) {
        f.close();
        Serial.println("[UPLOAD] Connect failed");
        return false;
    }

    http.printf("POST /api/upload HTTP/1.1\r\n"
                "Host: %s:%d\r\n"
                "Content-Type: application/octet-stream\r\n"
                "X-Device-Token: %s\r\n"
                "X-Device-MAC: %s\r\n"
                "Content-Length: %u\r\n"
                "Connection: close\r\n\r\n",
                host.c_str(), port,
                deviceToken.c_str(), deviceMac.c_str(), (unsigned)fsize);

    uint8_t ubuf[1024];
    size_t remaining = fsize;
    while (remaining > 0) {
        size_t chunk = remaining < sizeof(ubuf) ? remaining : sizeof(ubuf);
        size_t rd = f.read(ubuf, chunk);
        if (rd == 0) break;
        http.write(ubuf, rd);
        remaining -= rd;
        wsClient.poll();
    }
    f.close();

    unsigned long t = millis();
    while (!http.available() && millis() - t < 10000) delay(10);
    String resp;
    while (http.available()) resp += (char)http.read();
    http.stop();

    if (resp.indexOf("201") >= 0) {
        SD.remove(path);
        Serial.printf("[UPLOAD] %s uploaded OK, deleted\n", path);
        return true;
    }
    Serial.printf("[UPLOAD] %s failed: %s\n", path, resp.substring(0, 80).c_str());
    return false;
}

// ═══════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println("\n\n========================================");
    Serial.printf("XIAO ESP32-C3 Audio Device  v%s\n", FW_VERSION);
    Serial.println("========================================");

    // LED init + startup test
    pixel.begin();
    pixel.setPixelColor(0, COLOR_RED); pixel.show(); delay(300);
    pixel.setPixelColor(0, COLOR_GREEN); pixel.show(); delay(300);
    pixel.setPixelColor(0, COLOR_BLUE); pixel.show(); delay(300);
    pixel.setPixelColor(0, COLOR_OFF); pixel.show();
    Serial.println("[LED] Startup test: RED -> GREEN -> BLUE");

    // Button
    pinMode(PIN_BUTTON, INPUT_PULLUP);

    // OTA rollback safety
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t otaSt;
    if (esp_ota_get_state_partition(running, &otaSt) == ESP_OK) {
        if (otaSt == ESP_OTA_IMG_PENDING_VERIFY) {
            Serial.println("[OTA] New firmware booted, marking valid");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }
    Serial.printf("[BOOT] Partition: %s\n", running ? running->label : "?");

    // I2S microphone
    i2sReady = initI2S();

    // SD card
    sdAvailable = initSD();

    // WebSocket callbacks
    wsClient.onMessage(onWsMessage);
    wsClient.onEvent(onWsEvent);

    // Load saved config
    prefs.begin("audio", true);
    cfgSsid      = prefs.getString("ssid", "");
    cfgPassword   = prefs.getString("pass", "");
    cfgServerUrl  = prefs.getString("server", DEFAULT_SERVER);
    deviceToken   = prefs.getString("token", "");
    cfgEmail      = prefs.getString("email", "");
    cfgAcctPass   = prefs.getString("apass", "");
    cfgAction     = prefs.getString("action", "login");
    prefs.end();

    Serial.printf("[CONFIG] SSID='%s'  Server='%s'  Token=%s  Email=%s\n",
        cfgSsid.c_str(), cfgServerUrl.c_str(),
        deviceToken.length() > 0 ? "present" : "none",
        cfgEmail.length() > 0 ? cfgEmail.c_str() : "none");
    Serial.printf("[CONFIG] I2S=%s  SD=%s\n",
        i2sReady ? "OK" : "FAIL", sdAvailable ? "OK" : "NONE");
    Serial.printf("[HEAP] Free: %u bytes\n", (unsigned)ESP.getFreeHeap());

    if (cfgSsid.length() == 0) {
        setState(STATE_SETUP);
        startCaptivePortal();
    } else {
        setState(STATE_WIFI_CONNECTING);
    }
}

// ═══════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════
void loop() {
    updateLed();
    checkLongPressReset();

    switch (appState) {

    // ── Captive Portal ──────────────────────────────────
    case STATE_SETUP:
        portalDns->processNextRequest();
        portalServer->handleClient();
        break;

    // ── WiFi Connecting ─────────────────────────────────
    case STATE_WIFI_CONNECTING: {
        // Long-press button for WiFi reset (2s)
        if (digitalRead(PIN_BUTTON) == LOW) {
            unsigned long hs = millis();
            while (digitalRead(PIN_BUTTON) == LOW) {
                if ((millis() - hs) / 1000 >= 2) {
                    Serial.println("[BTN] WiFi reset -- clearing SSID, rebooting");
                    prefs.begin("audio", false);
                    prefs.remove("ssid");
                    prefs.remove("pass");
                    prefs.end();
                    setLed(COLOR_RED, PAT_SOLID);
                    delay(1000);
                    ESP.restart();
                }
                delay(100);
            }
        }

        if (connectWiFi()) {
            wifiRetryCount = 0;
            setState(STATE_OTA_CHECK);
        } else {
            wifiRetryCount++;
            if (wifiRetryCount >= MAX_WIFI_RETRIES) {
                Serial.println("[WIFI] Max retries, entering setup portal");
                wifiRetryCount = 0;
                setState(STATE_SETUP);
                startCaptivePortal();
            } else {
                Serial.printf("[WIFI] Retry %d/%d in 5s\n", wifiRetryCount, MAX_WIFI_RETRIES);
                setLed(COLOR_RED, PAT_FAST_BLINK);
                delay(5000);
            }
        }
        break;
    }

    // ── OTA Check ───────────────────────────────────────
    case STATE_OTA_CHECK: {
        if (deviceToken.length() == 0) {
            setState(STATE_CHECK_PAIRED);
            break;
        }
        Serial.println("[OTA] Checking for updates...");
        setLed(COLOR_CYAN, PAT_BREATHE);
        if (checkForOta()) {
            Serial.printf("[OTA] Update available: v%s\n", otaVersion.c_str());
            setState(STATE_OTA_UPDATING);
        } else {
            Serial.println("[OTA] No update");
            lastOtaCheck = millis();
            setState(STATE_CHECK_PAIRED);
        }
        break;
    }

    // ── OTA Update ──────────────────────────────────────
    case STATE_OTA_UPDATING:
        if (!performOta()) {
            Serial.println("[OTA] Update failed, continuing");
            setLed(COLOR_RED, PAT_SOLID);
            delay(2000);
            setState(STATE_CHECK_PAIRED);
        }
        break;

    // ── Check Pairing ───────────────────────────────────
    case STATE_CHECK_PAIRED:
        if (deviceToken.length() > 0) {
            Serial.println("[PAIR] Token exists, connecting to cloud");
            setState(STATE_WS_CONNECTING);
        } else if (cfgEmail.length() > 0) {
            Serial.printf("[PAIR] Account credentials found, auto-pairing with %s...\n", cfgEmail.c_str());
            setLed(COLOR_CYAN, PAT_BREATHE);
            if (registerWithUser()) {
                setLed(COLOR_GREEN, PAT_SOLID);
                delay(1000);
                setState(STATE_WS_CONNECTING);
            } else {
                Serial.println("[PAIR] Auto-pair failed, falling back to pairing code");
                pairingCode = genPairCode();
                Serial.println("╔══════════════════════════════════╗");
                Serial.printf( "║     PAIRING CODE: %s          ║\n", pairingCode.c_str());
                Serial.println("╚══════════════════════════════════╝");
                setLed(COLOR_CYAN, PAT_DOUBLE_BLINK);
                if (registerForPairing())
                    Serial.println("[PAIR] Registered, waiting for user to pair via web");
                else
                    Serial.println("[PAIR] Register failed, will retry");
                lastPairingPoll = millis();
                setState(STATE_PAIRING);
            }
        } else {
            pairingCode = genPairCode();
            Serial.println("╔══════════════════════════════════╗");
            Serial.printf( "║     PAIRING CODE: %s          ║\n", pairingCode.c_str());
            Serial.println("╚══════════════════════════════════╝");
            setLed(COLOR_CYAN, PAT_DOUBLE_BLINK);
            if (registerForPairing())
                Serial.println("[PAIR] Registered, waiting for user to pair via web");
            else
                Serial.println("[PAIR] Register failed, will retry");
            lastPairingPoll = millis();
            setState(STATE_PAIRING);
        }
        break;

    // ── Pairing Poll ────────────────────────────────────
    case STATE_PAIRING:
        if (millis() - lastPairingPoll >= PAIRING_POLL_INTERVAL) {
            lastPairingPoll = millis();
            String token = pollPairingStatus();
            if (token.length() > 0) {
                deviceToken = token;
                prefs.begin("audio", false);
                prefs.putString("token", deviceToken);
                prefs.end();
                Serial.printf("[PAIR] Paired! Token: %s...\n", deviceToken.substring(0, 8).c_str());
                setLed(COLOR_GREEN, PAT_SOLID);
                delay(1500);
                setState(STATE_WS_CONNECTING);
            }
        }
        break;

    // ── WebSocket Connecting ────────────────────────────
    case STATE_WS_CONNECTING: {
        if (WiFi.status() != WL_CONNECTED) {
            setState(STATE_WIFI_CONNECTING);
            break;
        }

        // Button press while connecting → offline recording
        if (checkButton() && sdAvailable && i2sReady) {
            Serial.println("[BTN] No cloud yet, starting offline recording");
            setState(STATE_RECORDING_OFFLINE);
            break;
        }

        if (millis() - lastWsReconnect < WS_RECONNECT_INTERVAL) break;
        lastWsReconnect = millis();

        setLed(COLOR_PURPLE, PAT_BREATHE);
        if (connectWebSocket()) {
            setState(STATE_READY);
            setLed(COLOR_GREEN, PAT_SOLID);
            Serial.printf("[READY] Connected to cloud. Heap: %u\n", (unsigned)ESP.getFreeHeap());
        } else {
            Serial.println("[WS] Connection failed, retrying...");
        }
        break;
    }

    // ── Ready / Idle ────────────────────────────────────
    case STATE_READY: {
        wsClient.poll();

        if (!wsConnected) {
            setState(STATE_WS_CONNECTING);
            break;
        }

        // Periodic OTA check
        if (millis() - lastOtaCheck >= OTA_CHECK_INTERVAL) {
            lastOtaCheck = millis();
            Serial.println("[OTA] Periodic check...");
            if (checkForOta()) {
                Serial.printf("[OTA] Update available: v%s -- auto-updating\n", otaVersion.c_str());
                setState(STATE_OTA_UPDATING);
                break;
            }
        }

        // Periodic upload of pending files
        if (sdAvailable && millis() - lastUploadCheck >= UPLOAD_CHECK_INTERVAL) {
            lastUploadCheck = millis();
            File dir = SD.open("/pending");
            if (dir && dir.isDirectory()) {
                File f = dir.openNextFile();
                if (f) {
                    String name = String("/pending/") + f.name();
                    f.close();
                    dir.close();
                    Serial.printf("[UPLOAD] Found pending: %s\n", name.c_str());
                    setState(STATE_UPLOADING_PENDING);
                    setLed(COLOR_WHITE, PAT_BREATHE);
                    uploadWavFile(name.c_str());
                    setState(STATE_READY);
                    setLed(COLOR_GREEN, PAT_SOLID);
                    break;
                }
                dir.close();
            }
        }

        // WS keepalive ping
        if (millis() - lastWsPing >= WS_PING_INTERVAL) {
            wsClient.ping();
            lastWsPing = millis();
        }

        // Button → start streaming
        if (checkButton()) {
            if (wsConnected && i2sReady) {
                Serial.println("[STREAM] Starting live stream...");
                adpcm.pred = 0;
                adpcm.idx = 0;
                dcOffset = 0;

                wsClient.poll();
                delay(20);

                // Send start as JSON text (avoids binary framing issues on C3)
                char startMsg[64];
                snprintf(startMsg, sizeof(startMsg),
                    "{\"type\":\"stream_start\",\"sr\":%d}", SAMPLE_RATE);
                bool sent = wsClient.send(startMsg);
                Serial.printf("[STREAM] Start sent=%d, heap=%u\n",
                    sent, (unsigned)ESP.getFreeHeap());

                if (!sent || !wsConnected) {
                    Serial.println("[STREAM] Send failed, aborting stream start");
                    break;
                }

                delay(50);
                wsClient.poll();

                if (!wsConnected) {
                    Serial.println("[STREAM] Connection lost after start");
                    break;
                }

                streamStartMs = millis();
                lastWsPing = millis();
                setState(STATE_STREAMING);
                setLed(COLOR_RED, PAT_RAPID_PULSE);
                Serial.printf("[STREAM] Started. Heap: %u\n", (unsigned)ESP.getFreeHeap());
            } else if (sdAvailable && i2sReady) {
                Serial.println("[BTN] WS not connected, starting offline recording");
                setState(STATE_RECORDING_OFFLINE);
            } else {
                Serial.println("[BTN] Cannot record: no mic or no WS/SD");
            }
        }
        break;
    }

    // ── Live Streaming ──────────────────────────────────
    case STATE_STREAMING: {
        wsClient.poll();

        if (wsConnected && millis() - lastWsPing >= WS_PING_INTERVAL) {
            wsClient.ping();
            lastWsPing = millis();
        }

        if (!wsConnected) {
            i2s_stop(I2S_NUM_0);
            unsigned long elapsed = (millis() - streamStartMs) / 1000;
            Serial.printf("[STREAM] Lost connection after %lus\n", elapsed);
            setLed(COLOR_RED, PAT_SOLID);
            delay(1000);
            setState(STATE_WS_CONNECTING);
            break;
        }

        // Button → stop streaming
        if (checkButton()) {
            i2s_stop(I2S_NUM_0);
            delay(100);
            wsClient.send("{\"type\":\"stream_stop\"}");
            unsigned long elapsed = (millis() - streamStartMs) / 1000;
            Serial.printf("[STREAM] Stopped after %lus\n", elapsed);
            i2s_start(I2S_NUM_0);
            setState(STATE_READY);
            setLed(COLOR_GREEN, PAT_SOLID);
            break;
        }

        // Read mic + encode + send
        size_t samples = readMic();
        if (samples > 0) {
            removeDC(pcmChunk, samples);
            adpcm_block(pcmChunk, adpcmBuf, samples);
            if (wsConnected)
                wsClient.sendBinary((const char*)adpcmBuf, samples / 2);
        }

        // Print level every second
        {
            static int lastSec = -1;
            int sec = (millis() - streamStartMs) / 1000;
            if (sec != lastSec) {
                int32_t sum = 0;
                for (size_t i = 0; i < samples; i++) sum += abs(pcmChunk[i]);
                int avg = samples > 0 ? sum / samples : 0;
                Serial.printf("[STREAM] %ds  level=%d  heap=%u\n",
                    sec, avg, (unsigned)ESP.getFreeHeap());
                lastSec = sec;
            }
        }
        break;
    }

    // ── Offline Recording ───────────────────────────────
    case STATE_RECORDING_OFFLINE: {
        // Start recording if file not open
        if (!recFile) {
            char fname[40];
            snprintf(fname, sizeof(fname), "/pending/rec_%08lX.wav", (unsigned long)millis());
            recFile = SD.open(fname, FILE_WRITE);
            if (!recFile) {
                Serial.printf("[REC] Failed to create %s\n", fname);
                setState(STATE_READY);
                setLed(COLOR_GREEN, PAT_SOLID);
                break;
            }
            // Write placeholder header
            uint8_t zeros[44] = {};
            recFile.write(zeros, 44);
            recBytes = 0;
            dcOffset = 0;
            setLed(COLOR_ORANGE, PAT_SLOW_BLINK);
            Serial.printf("[REC] Recording to %s\n", fname);
        }

        // Button → stop recording
        if (checkButton()) {
            // Write real WAV header
            recFile.seek(0);
            writeWavHeader(recFile, recBytes);
            String name = recFile.name();
            recFile.close();
            Serial.printf("[REC] Stopped. %u bytes -> %s\n", (unsigned)recBytes, name.c_str());

            // Try to reconnect
            if (WiFi.status() == WL_CONNECTED) {
                setState(STATE_WS_CONNECTING);
                setLed(COLOR_PURPLE, PAT_BREATHE);
            } else {
                setState(STATE_WIFI_CONNECTING);
            }
            break;
        }

        // Read mic + write PCM to file
        size_t samples = readMic();
        if (samples > 0) {
            removeDC(pcmChunk, samples);
            size_t written = recFile.write((const uint8_t*)pcmChunk, samples * 2);
            recBytes += written;
        }

        // Print progress every second
        {
            static unsigned long lastPrint = 0;
            if (millis() - lastPrint >= 1000) {
                lastPrint = millis();
                Serial.printf("[REC] %u bytes written (%.1f sec)\n",
                    (unsigned)recBytes, (float)recBytes / (SAMPLE_RATE * 2));
            }
        }
        break;
    }

    // ── Uploading Pending ───────────────────────────────
    case STATE_UPLOADING_PENDING:
        // Handled inline in STATE_READY for simplicity
        break;

    } // switch
}
