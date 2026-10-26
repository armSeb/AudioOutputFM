// FMTransmitterWeb.ino - web radio -> FM transmitter with a web configuration page
//
// Everything is set from a browser (http://fmtx.local or the IP printed on the serial port)
// and stored in NVS, so it survives a reboot:
//   - carrier frequency, stereo on/off, RDS on/off, station name (PS), programme type,
//     pre-emphasis, gain, AGC, stereo separation trims, RDS character set
//   - a list of up to 8 stream URLs, and which one is on air
//   - Wi-Fi credentials. No credentials are needed in this file: on first boot (or if the
//     connection fails) the ESP32 opens its own access point "ESP32-FM-Setup" (password
//     "fmsetup123"); join it, open http://192.168.4.1 and enter your Wi-Fi on the page.
//
// Architecture, all on core 0 (in stereo the modulator owns core 1):
//   - audio task (priority 3): stream + MP3/AAC decoder only, so a slow web client can't starve it
//   - web task (priority 2): web page + serial console (x 0|1 = RF off/on, s = status); it asks
//     the audio task to change station through a queue (the URL is copied, nothing shared)
// Switching mono/stereo saves and reboots; every other setting applies live.
//
// Running 24/7:
//   - "On air / Off air" button (and POST /api/onair on=0|1 for home automation). Off air stops
//     the stream and the modulator, frees core 1, powers the APLL down, drops the CPU to 80 MHz
//     and lets Wi-Fi sleep. The state survives a power cut.
//   - Self-healing: task watchdog (reboot if the audio or web task hangs for 30 s), Wi-Fi
//     reconnection (reboot after 10 minutes without Wi-Fi), restart of a stalled stream,
//     heap guard. Uptime, last reset cause and lowest free heap are shown on the page.
//   - Firmware update from the page (OTA), no USB cable needed once installed.
//
// Hardware: RF out on GPIO0 through a DC-blocking capacitor and a low-pass filter.
// Check your local regulations.

#include <Arduino.h>

#if !defined(ESP32) || !defined(CONFIG_IDF_TARGET_ESP32)
void setup() {}
void loop() {}
#else

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "AudioFileSourceICYStream.h"
#include "AudioFileSourceBuffer.h"
#include "AudioGeneratorMP3.h"
#include "AudioGeneratorAAC.h"
#include "stream_sniff.h"
#include "AudioOutputFM.h"

// Optional defaults, used only until something is saved from the web page. Leave them empty
// to keep credentials out of the code: the first boot then starts the setup access point.
// (They can also be passed as build flags: -DSTASSID=\"...\" -DSTAPSK=\"...\")
#ifndef STASSID
#define STASSID ""
#define STAPSK  ""
#endif
static const char *HOSTNAME = "fmtx";
static const char *AP_SSID  = "ESP32-FM-Setup";
static const char *AP_PASS  = "fmsetup123";
static const int   MAX_STATIONS = 8;
// Shown on the page and at boot: library version + build time of this binary
static const char *FW_VERSION = AUDIOOUTPUTFM_VERSION " (built " __DATE__ " " __TIME__ ")";
// Decoder memory + stream buffer share ONE block, the biggest the heap has at boot (~110 KB), carved
// per station once the codec is known: an MP3 decoder takes ~30 KB and leaves ~80 KB of stream
// buffer; the HE-AAC decoder takes 87.4 KB (input 1.6 + output 8 + Helix core 28.1 + SBR 49.6) and
// leaves ~20 KB. Separate allocations don't fit: the leftover heap is too fragmented.
static const int   AAC_MEM = 90112;            // measured 89460 B for the Helix structures, plus a margin
static const int   NET_BUF_MIN = 16 * 1024;    // smallest acceptable stream buffer
static const int   ARENA_PSRAM = 256 * 1024;   // with PSRAM (WROVER modules): roomy, and off the internal heap
static const int   SNIFF_LEN = 2048;           // bytes read up front to tell MP3 from AAC

// ---------------------------------------------------------------------------------------------
// Configuration (NVS)
// ---------------------------------------------------------------------------------------------

struct Config {
    String ssid, pass;
    float  freq = 100.1f;
    bool   stereo = true, rds = true, agc = true, rtab = false, onair = true;
    String ps = "ESP32 FM";
    int    pty = 10, preemph = 50, charset = 0;
    float  gain = 1.0f, k = 1.11f, h = -22.0f;
    int    count = 0, sel = 0;
    String name[MAX_STATIONS], url[MAX_STATIONS];
} cfg;

Preferences prefs;

// Station change request, web task -> audio task. Declared up here, before any function:
// the Arduino IDE inserts its auto-generated prototypes ahead of the first function.
struct PlayCmd { char name[64]; char url[256]; };
void webTask(void *);

void loadConfig()
{
    prefs.begin("fmtx", true);
    cfg.ssid    = prefs.getString("ssid", STASSID);
    cfg.pass    = prefs.getString("pass", STAPSK);
    cfg.freq    = prefs.getFloat("freq", 100.1f);
    cfg.stereo  = prefs.getBool("stereo", true);
    cfg.rds     = prefs.getBool("rds", true);
    cfg.agc     = prefs.getBool("agc", true);
    cfg.rtab    = prefs.getBool("rtab", false);
    cfg.onair   = prefs.getBool("onair", true);
    cfg.ps      = prefs.getString("ps", "ESP32 FM");
    cfg.pty     = prefs.getInt("pty", 10);
    cfg.preemph = prefs.getInt("preemph", 50);
    cfg.charset = prefs.getInt("charset", 0);
    cfg.gain    = prefs.getFloat("gain", 1.0f);
    cfg.k       = prefs.getFloat("k", 1.11f);
    cfg.h       = prefs.getFloat("h", -22.0f);
    cfg.count   = prefs.getInt("count", -1);
    cfg.sel     = prefs.getInt("sel", 0);
    if (cfg.count < 0) {                       // first boot: two example stations
        cfg.count = 2;
        cfg.name[0] = "Frequence3";
        cfg.url[0]  = "http://frequence3.net-radio.fr/frequence3-256.mp3";
        cfg.name[1] = "Irish Pub Radio";
        cfg.url[1]  = "http://solid24.streamupsolutions.com/proxy/vbkiacud?mp=/live";
    } else {
        cfg.count = constrain(cfg.count, 0, MAX_STATIONS);
        for (int i = 0; i < cfg.count; i++) {
            cfg.name[i] = prefs.getString(("n" + String(i)).c_str(), "");
            cfg.url[i]  = prefs.getString(("u" + String(i)).c_str(), "");
        }
    }
    if (cfg.sel >= cfg.count) cfg.sel = 0;
    if (!AudioOutputFM::IsFrequencyUsable(cfg.freq)) cfg.freq = 100.1f;
    prefs.end();
}

// Station change: write only the index. Every flash write stalls core 1, so the carrier freezes
// (the stereo loop skips samples) for as long as the write lasts: keep it to one key.
void saveSel()
{
    prefs.begin("fmtx", false);
    prefs.putInt("sel", cfg.sel);
    prefs.end();
}

void saveOnAir()
{
    prefs.begin("fmtx", false);
    prefs.putBool("onair", cfg.onair);
    prefs.end();
}

// Each write is a flash operation (a few ms, the modulator core is briefly stalled): only on Save.
void saveConfig()
{
    prefs.begin("fmtx", false);
    prefs.putString("ssid", cfg.ssid);
    prefs.putString("pass", cfg.pass);
    prefs.putFloat("freq", cfg.freq);
    prefs.putBool("stereo", cfg.stereo);
    prefs.putBool("rds", cfg.rds);
    prefs.putBool("agc", cfg.agc);
    prefs.putBool("rtab", cfg.rtab);
    prefs.putString("ps", cfg.ps);
    prefs.putInt("pty", cfg.pty);
    prefs.putInt("preemph", cfg.preemph);
    prefs.putInt("charset", cfg.charset);
    prefs.putFloat("gain", cfg.gain);
    prefs.putFloat("k", cfg.k);
    prefs.putFloat("h", cfg.h);
    prefs.putInt("count", cfg.count);
    prefs.putInt("sel", cfg.sel);
    for (int i = 0; i < cfg.count; i++) {
        prefs.putString(("n" + String(i)).c_str(), cfg.name[i]);
        prefs.putString(("u" + String(i)).c_str(), cfg.url[i]);
    }
    prefs.end();
}

// ---------------------------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------------------------

AudioFileSourceICYStream *file = nullptr;
AudioFileSourceBuffer    *buff = nullptr;
PrefixSource             *pre  = nullptr;
AudioGenerator           *gen  = nullptr;     // AudioGeneratorMP3 or AudioGeneratorAAC
AudioOutputFM            *out  = nullptr;
volatile bool apMode = false;               // setup access point active (no usable Wi-Fi)
volatile uint32_t restartAt = 0;

// Allocated once at startup, before the modulator's own tables, and reused for every station:
// no 64 KB-block hunt on a fragmented heap, no fragmentation from station changes.
uint8_t *arena = nullptr;                     // decoder memory, then the stream buffer
int      arenaSize = 0;
static constexpr int align8(int n) { return (n + 7) & ~7; }
// Internal byte-addressable RAM. ESP.getMaxAllocHeap() also counts IRAM, which only takes 32-bit
// accesses (the modulator puts its ring there) and can't hold buffers or decoders; PSRAM, if any,
// is left out too: the health figures are about the RAM Wi-Fi, TCP and the web server need.
static const uint32_t RAM8 = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
static uint32_t largest8() { return heap_caps_get_largest_free_block(RAM8); }
static uint32_t free8() { return heap_caps_get_free_size(RAM8); }
static uint8_t sniffBuf[SNIFF_LEN];
volatile uint32_t minLargest = UINT32_MAX;
volatile uint32_t streamStartMs = 0;          // last stream start, to date heap lows
volatile uint32_t allocFails = 0;             // failed heap allocations since boot
TaskHandle_t audioTaskH = nullptr, webTaskH = nullptr;   // smallest "largest byte-addressable block" seen
volatile bool aacOk = false;                  // the AAC decoder fits in the arena with a usable buffer
volatile bool codecAac = false;               // codec of the stream on air

// Shared between the web task and the audio task
QueueHandle_t playQ;                          // web -> audio: "play this station" (copied, no shared Strings)
portMUX_TYPE  titleMux = portMUX_INITIALIZER_UNLOCKED;
char          titleBuf[160] = "";             // current ICY title, written by the audio task
volatile bool streamPlaying = false;
volatile bool wantOnAir = true;               // requested by the web task / serial console
volatile bool isOnAir = false;                // actual state, owned by the audio task

// Web/serial side: request on or off air, and remember it across power cuts
void setOnAir(bool on)
{
    wantOnAir = on;
    if (cfg.onair != on) {
        cfg.onair = on;
        saveOnAir();
    }
    Serial.printf("%s requested\n", on ? "On air" : "Off air");
}

void setTitle(const char *t)
{
    portENTER_CRITICAL(&titleMux);
    strlcpy(titleBuf, t, sizeof(titleBuf));
    portEXIT_CRITICAL(&titleMux);
}

String getTitle()
{
    char tmp[sizeof(titleBuf)];
    portENTER_CRITICAL(&titleMux);
    memcpy(tmp, titleBuf, sizeof(tmp));
    portEXIT_CRITICAL(&titleMux);
    return String(tmp);
}

// Ask the audio task to (re)start the selected station. Called from the web task / setup().
void requestPlay()
{
    PlayCmd c = {};
    if (cfg.count > 0) {
        strlcpy(c.name, cfg.name[cfg.sel].c_str(), sizeof(c.name));
        strlcpy(c.url, cfg.url[cfg.sel].c_str(), sizeof(c.url));
    }
    xQueueOverwrite(playQ, &c);               // only the latest request matters
}

void applyRadio()
{
    static float appliedFreq = 0.0f;
    if (cfg.freq != appliedFreq) {             // retuning the APLL causes a short glitch: only when needed
        out->SetFrequency(cfg.freq);
        appliedFreq = cfg.freq;
    }
    out->SetPreemphasis(cfg.preemph);
    out->SetGain(cfg.gain);
    out->SetAGC(cfg.agc, 0.25f);
    out->SetStereoTrim(cfg.k, cfg.h);
    out->SetRDS(cfg.stereo && cfg.rds);
    out->SetRdsPS(cfg.ps.c_str());
    out->SetRdsPTY(cfg.pty);
    out->SetRdsCharset(cfg.charset);
    out->SetRdsABToggle(cfg.rtab);
}

// ---- everything below runs in the audio task only ----

void MDCallback(void *, const char *type, bool, const char *str)
{
    if (!strcmp(type, "StreamTitle")) {
        setTitle(str);
        Serial.printf("[ICY] %s\n", str);
        if (str[0]) out->SetRdsText(str);
    }
}

void stopStream()
{
    streamPlaying = false;
    if (gen) { gen->stop(); delete gen; gen = nullptr; }
    delete buff; buff = nullptr;
    delete pre;  pre  = nullptr;
    delete file; file = nullptr;
}

bool startStream(const PlayCmd &c)
{
    if (apMode || !c.url[0]) return true;      // nothing to play: not an error
    Serial.printf("Playing %s (%s)\n", c.name, c.url);
    streamStartMs = millis();
    setTitle("");
    out->SetRdsText(c.name);                   // until the stream sends a title
    // HTTP/1.0: a server may otherwise answer HTTP/1.1 with "Transfer-Encoding: chunked", and
    // the chunk headers would end up in the audio data (sync losses, choppy "accelerated" sound).
    // The flag must be set before open(), hence the default constructor.
    file = new AudioFileSourceICYStream();
    file->useHTTP10();
    file->open(c.url);
    if (!file->isOpen()) {
        Serial.println("Stream: HTTP connection failed");
        stopStream();
        return false;
    }
    file->RegisterMetadataCB(MDCallback, nullptr);

    // Read the first 2 KB to find out what the station sends, then hand them back to the decoder
    int n = 0;
    uint32_t t0 = millis();
    while (n < SNIFF_LEN && file->isOpen() && millis() - t0 < 5000) {
        n += file->read(sniffBuf + n, SNIFF_LEN - n);  // blocks up to 0.5 s per call
        esp_task_wdt_reset();
    }
    if (n < 64) {
        Serial.println("Stream: no data");
        stopStream();
        return false;
    }
    bool aac = sniffCodec(sniffBuf, n) == CODEC_AAC;
    codecAac = aac;
    Serial.printf("Stream: %s\n", aac ? "AAC (ADTS)" : "MP3");
    if (aac && !aacOk) {
        Serial.println("Stream: AAC, but there was not enough RAM for the AAC decoder at boot");
        setTitle("AAC stream: not enough RAM for the AAC decoder");
        stopStream();
        return true;                           // retrying would not help
    }
    pre  = new PrefixSource(file, sniffBuf, n);
    const int decSize = aac ? AAC_MEM : AudioGeneratorMP3::preAllocSize();
    const int bufSize = arenaSize - align8(decSize);
    Serial.printf("Stream buffer %d KB\n", bufSize / 1024);
    buff = new AudioFileSourceBuffer(pre, arena + align8(decSize), bufSize);
    if (aac) gen = new AudioGeneratorAAC(arena, decSize);
    else     gen = new AudioGeneratorMP3(arena, decSize);
    if (!gen->begin(buff, out)) {
        Serial.printf("Stream: decoder start failed (free heap %u, largest block %u)\n",
                      (unsigned)free8(), (unsigned)largest8());
        stopStream();
        return false;
    }
    streamPlaying = true;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Web
// ---------------------------------------------------------------------------------------------

WebServer server(80);

static String jstr(const String &s)
{
    String o = "\"";
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((uint8_t)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += c;
    }
    return o + "\"";
}

static const char *resetReason()
{
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  return "power on";
        case ESP_RST_SW:       return "software restart";
        case ESP_RST_PANIC:    return "crash / watchdog";
        case ESP_RST_INT_WDT:  return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT:      return "watchdog";
        case ESP_RST_BROWNOUT: return "brownout (power supply)";
        case ESP_RST_EXT:      return "reset pin";
        default:               return "other";
    }
}

void handleState()
{
    AudioOutputFM::Stats st = out->GetStats(true);   // rate measured since the previous poll
    String j;
    j.reserve(1600);                           // one allocation per poll: no heap fragmentation over days
    j = "{";
    j += "\"freq\":" + String(cfg.freq, 2);
    j += ",\"stereo\":" + String(cfg.stereo ? "true" : "false");
    j += ",\"rds\":" + String(cfg.rds ? "true" : "false");
    j += ",\"agc\":" + String(cfg.agc ? "true" : "false");
    j += ",\"rtab\":" + String(cfg.rtab ? "true" : "false");
    j += ",\"ps\":" + jstr(cfg.ps);
    j += ",\"pty\":" + String(cfg.pty);
    j += ",\"preemph\":" + String(cfg.preemph);
    j += ",\"charset\":" + String(cfg.charset);
    j += ",\"gain\":" + String(cfg.gain, 2);
    j += ",\"k\":" + String(cfg.k, 3);
    j += ",\"h\":" + String(cfg.h, 1);
    j += ",\"ssid\":" + jstr(cfg.ssid);
    j += ",\"sel\":" + String(cfg.sel);
    j += ",\"stations\":[";
    for (int i = 0; i < cfg.count; i++) {
        if (i) j += ",";
        j += "{\"name\":" + jstr(cfg.name[i]) + ",\"url\":" + jstr(cfg.url[i]) + "}";
    }
    j += "]";
    j += ",\"status\":{\"title\":" + jstr(getTitle());
    j += ",\"playing\":" + String(streamPlaying ? "true" : "false");
    j += ",\"codec\":" + String(codecAac ? "\"AAC\"" : "\"MP3\"");
    j += ",\"aac\":" + String(aacOk ? "true" : "false");
    j += ",\"ap\":" + String(apMode ? "true" : "false");
    j += ",\"ip\":" + jstr(apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString());
    j += ",\"rssi\":" + String(apMode ? 0 : WiFi.RSSI());
    j += ",\"bssid\":" + jstr(apMode ? String() : WiFi.BSSIDstr());
    j += ",\"ch\":" + String(apMode ? 0 : WiFi.channel());
    j += ",\"actual\":" + String(out->GetFrequency(), 4);
    j += ",\"rate\":" + String(st.measuredHz, 0);
    j += ",\"fill\":" + String(st.fill) + ",\"size\":" + String(st.size);
    j += ",\"underruns\":" + String(st.underruns);
    j += ",\"peak\":" + String(st.peak, 2);
    j += ",\"heap\":" + String(free8());
    j += ",\"minheap\":" + String(heap_caps_get_minimum_free_size(RAM8));
    j += ",\"largest\":" + String(largest8());
    j += ",\"minlargest\":" + String(minLargest == UINT32_MAX ? largest8() : minLargest);
    j += ",\"onair\":" + String(isOnAir ? "true" : "false");
    j += ",\"want\":" + String(wantOnAir ? "true" : "false");
    j += ",\"uptime\":" + String(millis() / 1000);
    j += ",\"reset\":" + jstr(resetReason());
    j += ",\"fw\":" + jstr(FW_VERSION);
    j += ",\"allocfail\":" + String(allocFails);
    j += "}}";
    server.send(200, "application/json", j);
}

static bool argBool(const char *n) { return server.arg(n) == "1" || server.arg(n) == "true" || server.arg(n) == "on"; }

void handleRadio()
{
    float f = server.arg("freq").toFloat();
    if (!AudioOutputFM::IsFrequencyUsable(f)) {
        server.send(400, "text/plain", "Frequency rejected: 88-108 MHz, and not within ~80 kHz of 90 or 100 MHz");
        return;
    }
    bool newStereo = argBool("stereo");
    cfg.freq    = f;
    cfg.rds     = argBool("rds");
    cfg.agc     = argBool("agc");
    cfg.ps      = server.arg("ps").substring(0, 24);   // UTF-8 bytes; the RDS encoder keeps 8 characters
    cfg.pty     = constrain(server.arg("pty").toInt(), 0, 31);
    cfg.preemph = server.arg("preemph").toInt() == 75 ? 75 : (server.arg("preemph").toInt() == 0 ? 0 : 50);
    cfg.gain    = constrain(server.arg("gain").toFloat(), 0.0f, 4.0f);
    if (server.hasArg("k"))       cfg.k = constrain(server.arg("k").toFloat(), 0.5f, 2.0f);
    if (server.hasArg("h"))       cfg.h = constrain(server.arg("h").toFloat(), -90.0f, 90.0f);
    if (server.hasArg("charset")) cfg.charset = constrain(server.arg("charset").toInt(), 0, 2);
    if (server.hasArg("rtab"))    cfg.rtab = argBool("rtab");
    bool reboot = (newStereo != cfg.stereo);
    cfg.stereo = newStereo;
    saveConfig();
    if (reboot) {
        server.send(200, "application/json", "{\"restart\":true}");
        restartAt = millis() + 800;
        return;
    }
    applyRadio();
    server.send(200, "application/json", "{\"restart\":false}");
}

void handleStations()
{
    int n = constrain(server.arg("count").toInt(), 0, MAX_STATIONS);
    String oldUrl = cfg.count ? cfg.url[cfg.sel] : "";
    for (int i = 0; i < n; i++) {
        cfg.name[i] = server.arg("n" + String(i));
        cfg.url[i]  = server.arg("u" + String(i));
        cfg.url[i].trim();
    }
    cfg.count = n;
    if (cfg.sel >= cfg.count) cfg.sel = 0;
    saveConfig();
    if (!cfg.count || cfg.url[cfg.sel] != oldUrl) requestPlay();   // the station on air was edited or removed
    server.send(200, "application/json", "{}");
}

void handleSelect()
{
    int i = server.arg("i").toInt();
    if (i < 0 || i >= cfg.count) { server.send(400, "text/plain", "Bad station index"); return; }
    cfg.sel = i;
    saveSel();
    requestPlay();
    server.send(200, "application/json", "{}");
}

void handleOnAir()
{
    setOnAir(argBool("on"));
    server.send(200, "application/json", String("{\"onair\":") + (wantOnAir ? "true}" : "false}"));
}

// Firmware update. The transmitter goes off air first: the modulator stops, so the flash
// writes don't keep stalling it, and the update gets the whole CPU.
static bool otaOk = false;

void handleUpdateUpload()
{
    HTTPUpload &up = server.upload();
    esp_task_wdt_reset();
    if (up.status == UPLOAD_FILE_START) {
        Serial.printf("OTA: receiving %s\n", up.filename.c_str());
        wantOnAir = false;                     // not persisted: the previous state comes back after reboot
        for (int i = 0; i < 150 && isOnAir; i++) {
            vTaskDelay(pdMS_TO_TICKS(20));
            esp_task_wdt_reset();
        }
        otaOk = Update.begin(UPDATE_SIZE_UNKNOWN);
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (otaOk && Update.write(up.buf, up.currentSize) != up.currentSize) otaOk = false;
    } else if (up.status == UPLOAD_FILE_END) {
        otaOk = otaOk && Update.end(true);
        Serial.printf("OTA %s (%u bytes)\n", otaOk ? "done" : "FAILED", (unsigned)up.totalSize);
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
        otaOk = false;
    }
}

void handleUpdateDone()
{
    if (otaOk) {
        server.send(200, "application/json", "{\"ok\":true}");
        restartAt = millis() + 1000;
    } else {
        server.send(500, "text/plain", String("Update failed: ") + Update.errorString());
        wantOnAir = cfg.onair;
    }
}

void handleWifi()
{
    cfg.ssid = server.arg("ssid");
    if (server.arg("pass").length()) cfg.pass = server.arg("pass");   // empty = keep the current one
    saveConfig();
    server.send(200, "application/json", "{\"restart\":true}");
    restartAt = millis() + 800;
}

#include "web_page.h"                          // PAGE[]: the HTML/JS of the configuration page

void setupWeb()
{
    server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", PAGE); });
    server.on("/api/state", HTTP_GET, handleState);
    server.on("/api/radio", HTTP_POST, handleRadio);
    server.on("/api/stations", HTTP_POST, handleStations);
    server.on("/api/select", HTTP_POST, handleSelect);
    server.on("/api/wifi", HTTP_POST, handleWifi);
    server.on("/api/onair", HTTP_POST, handleOnAir);
    server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
    server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
    server.begin();
    MDNS.begin(HOSTNAME);
    MDNS.addService("http", "tcp", 80);
}

// ---------------------------------------------------------------------------------------------
// Tasks
// ---------------------------------------------------------------------------------------------

// On air / off air, called from the audio task only.
// Off air releases everything the transmitter holds: stream stopped, modulator stopped (core 1
// freed, APLL powered down, GPIO0 high-impedance), CPU at 80 MHz, Wi-Fi modem sleep.
void goOffAir()
{
    stopStream();
    out->end();
    setTitle("");
    isOnAir = false;
    setCpuFrequencyMhz(80);
    if (!apMode) WiFi.setSleep(true);
    Serial.println("Off air");
}

bool goOnAir()
{
    setCpuFrequencyMhz(240);                   // before begin(): the stereo loop is timed in CPU cycles
    if (!apMode) WiFi.setSleep(false);
    if (!out->begin()) {                       // carrier up (in stereo, core 1 is taken from here on)
        Serial.printf("AudioOutputFM: start failed (free heap %u, largest block %u)\n",
                      (unsigned)free8(), (unsigned)largest8());
        return false;
    }
    isOnAir = true;
    Serial.printf("On air: %.4f MHz, %s\n", out->GetFrequency(), cfg.stereo ? "stereo" : "mono");
    return true;
}

// Stream + decoder only: nothing here waits on the web (core 0, priority 3)
void audioTask(void *)
{
    // Largest blocks first, before anything else fragments the heap:
    //   1. decoder memory: 90 KB with AAC (only the ~110 KB boot block can hold it), else ~30 KB
    //   2. modulator: ring 16 KB (in IRAM when possible) + resampler 8.3 KB
    //   3. stream buffer: whatever remains, 32 KB down to 16 KB
    // If the stream buffer can't get 16 KB next to the AAC decoder, AAC is given up: MP3 and the
    // transmitter matter more. The modulator keeps its buffers across off/on cycles.
    Serial.printf("Heap before buffers: free %u, largest block %u\n", (unsigned)free8(), (unsigned)largest8());
    const int mp3Size = AudioGeneratorMP3::preAllocSize();
    // 1. The arena: as big as the heap allows (the reported largest block is not always
    //    allocatable in full, so step down until it works)
    // With PSRAM (a WROVER module, or "PSRAM: Enabled"), the arena goes there: no squeeze at all.
    // Not validated on hardware yet.
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > ARENA_PSRAM) {
        arena = (uint8_t *)heap_caps_malloc(ARENA_PSRAM, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (arena) arenaSize = ARENA_PSRAM;
    }
    for (int sz = largest8(); sz >= mp3Size + NET_BUF_MIN && !arena; sz -= 1024)
        if ((arena = (uint8_t *)heap_caps_malloc(sz, RAM8))) arenaSize = sz;
    if (!arena) {
        Serial.printf("Out of memory for the stream buffers (largest block %u)\n", (unsigned)largest8());
        for (;;) vTaskDelay(1000);
    }
    // 2. The modulator (ring 16 KB, usually in IRAM, resampler 8.3 KB in stereo). If they don't fit in what's
    //    left, give back the end of the arena, 17 KB at a time (realloc shrinks in place).
    bool started = goOnAir();
    for (int i = 0; i < 3 && !started && arenaSize - 17408 >= mp3Size + NET_BUF_MIN; i++) {
        uint8_t *p = (uint8_t *)realloc(arena, arenaSize - 17408);
        if (!p) break;
        arena = p;
        arenaSize -= 17408;
        started = goOnAir();
    }
    aacOk = arenaSize >= align8(AAC_MEM) + NET_BUF_MIN;
    Serial.printf("Decoders: MP3%s, arena %d KB (stream buffer %d KB for MP3%s)\n",
                  aacOk ? " + AAC/HE-AAC" : " only (not enough RAM for AAC)", arenaSize / 1024,
                  (arenaSize - align8(mp3Size)) / 1024,
                  aacOk ? (String(", ") + (arenaSize - align8(AAC_MEM)) / 1024 + " KB for AAC").c_str() : "");
    if (started && !wantOnAir) goOffAir();
    // Web task last: its stack must not fragment the heap before the big buffers. Created from
    // here because setup() (core 1) may already be preempted by the stereo modulator.
    xTaskCreatePinnedToCore(webTask, "web", 6144, nullptr, 2, &webTaskH, 0);
    Serial.printf("Heap after start: free %u, largest block %u\n", (unsigned)free8(), (unsigned)largest8());

    esp_task_wdt_add(nullptr);                 // reboot if this task ever hangs for 30 s
    PlayCmd cur = {};
    uint32_t retryAt = 0, lastHealth = millis(), lastUnderruns = 0;
    bool retry = false;
    int starving = 0, heapLow = 0;
    for (;;) {
        esp_task_wdt_reset();

        PlayCmd c;
        if (xQueueReceive(playQ, &c, 0) == pdTRUE) {          // new station requested by the web task
            cur = c;
            if (isOnAir) {
                stopStream();
                retry = !startStream(cur);
                retryAt = millis() + 5000;
            }
        }

        bool want = wantOnAir;
        if (want && !isOnAir) {
            if (goOnAir()) {
                retry = !startStream(cur);
                retryAt = millis() + 5000;
            } else {
                wantOnAir = false;
            }
        } else if (!want && isOnAir) {
            goOffAir();
            retry = false;
        }

        if (!isOnAir) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (gen && gen->isRunning()) {
            if (!gen->loop()) {
                Serial.println("Stream lost, reconnecting in 2 s");
                stopStream();
                retry = true;
                retryAt = millis() + 2000;
            }
        } else if (retry && (int32_t)(millis() - retryAt) >= 0) {
            retry = !startStream(cur);
            retryAt = millis() + 5000;
        }

        // Health check, once a second
        if (millis() - lastHealth >= 1000) {
            lastHealth = millis();
            uint32_t u = out->GetStats(false).underruns;
            // The modulator kept running dry for 15 s in a row: the stream is stalled without
            // the HTTP client noticing. Start it again from scratch.
            starving = (streamPlaying && u != lastUnderruns) ? starving + 1 : 0;
            lastUnderruns = u;
            if (starving >= 15) {
                Serial.println("Stream starving for 15 s, restarting it");
                stopStream();
                starving = 0;
                retry = true;
                retryAt = millis();
            }
            // Leak / fragmentation guard: reboot cleanly rather than fail somewhere random.
            // Once running, nothing big is allocated any more (the decoder and stream buffer live
            // in the arena), and the Wi-Fi/TCP buffers come and go in bursts: so only a shortage
            // that lasts 15 s counts, and the bar is what the code still needs (a 1.6 KB web reply,
            // a few KB for HTTP and TCP), not a big block.
            uint32_t lg = largest8(), fr = free8();
            // Heap diagnostics: report each new low of the internal heap, with the time since the
            // last stream start, to tell a dip at station change from a slow leak
            static uint32_t lastLow = UINT32_MAX;
            uint32_t low = heap_caps_get_minimum_free_size(RAM8);
            if (low < lastLow) {
                if (lastLow != UINT32_MAX)
                    Serial.printf("Heap: new low %u bytes free (now free %u, largest %u, %.1f s after the last stream start)\n",
                                  (unsigned)low, (unsigned)fr, (unsigned)lg, (millis() - streamStartMs) / 1000.0f);
                lastLow = low;
            }
            if (lg < minLargest) minLargest = lg;
            bool shortage = lg < 2048 || fr < 10240;
            if (shortage && !heapLow) Serial.printf("Heap low: free %u, largest block %u\n", (unsigned)fr, (unsigned)lg);
            heapLow = shortage ? heapLow + 1 : 0;
            if (heapLow >= 15) {
                Serial.printf("Heap exhausted for 15 s (free %u, largest block %u), restarting\n", (unsigned)fr, (unsigned)lg);
                restartAt = millis();
            }
        }
        vTaskDelay(1);
    }
}

// Wi-Fi came back while in setup mode: drop the access point and resume normal operation
static void leaveSetupAp()
{
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(!isOnAir);                   // modem sleep only while off air
    apMode = false;
    MDNS.end();
    MDNS.begin(HOSTNAME);
    MDNS.addService("http", "tcp", 80);
    Serial.printf("Wi-Fi %s connected, setup access point closed: http://%s.local or http://%s\n",
                  cfg.ssid.c_str(), HOSTNAME, WiFi.localIP().toString().c_str());
    requestPlay();                             // the audio task starts the stream if on air
}

// Roaming: the ESP32 never leaves an access point on its own while the link holds. If the signal
// is weak, look (in the background) for a stronger access point with the same SSID and move to it.
// A scan briefly takes the radio off channel (~1.5 s, covered by the stream buffer), so it only
// runs when the RSSI is poor, and backs off from 5 min to 1 h while nothing better turns up.
static void checkRoaming()
{
    static uint32_t lastScan = 0, interval = 300000UL;
    static bool scanning = false;
    if (!scanning) {
        if (WiFi.RSSI() >= -70 || millis() - lastScan < interval) return;
        lastScan = millis();
        if (WiFi.scanNetworks(true, false, false, 120, 0, cfg.ssid.c_str()) == WIFI_SCAN_FAILED) return;
        scanning = true;
        return;
    }
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;
    scanning = false;
    int best = -1, cur = WiFi.RSSI();
    for (int i = 0; i < n; i++) {
        if (WiFi.SSID(i) != cfg.ssid || !memcmp(WiFi.BSSID(i), WiFi.BSSID(), 6)) continue;
        if (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best)) best = i;
    }
    if (best >= 0 && WiFi.RSSI(best) > cur + 8) {             // clearly better, not just noise
        uint8_t bssid[6];
        memcpy(bssid, WiFi.BSSID(best), 6);
        int ch = WiFi.channel(best);
        Serial.printf("Roaming: %d dBm here, %d dBm on %s (channel %d), switching\n",
                      cur, (int)WiFi.RSSI(best), WiFi.BSSIDstr(best).c_str(), ch);
        WiFi.scanDelete();
        WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str(), ch, bssid);
        interval = 300000UL;
    } else {
        WiFi.scanDelete();
        interval = (interval >= 1800000UL) ? 3600000UL : interval * 2;               // nothing better: look less often
    }
}

// Timing of the modulator (cumulative since the last 't'): see AudioOutputFM::Stats
static void printTiming(bool reset)
{
    AudioOutputFM::Stats st = out->GetStats(false);
    Serial.printf("timing | writer avg %.2f us, max %.2f us, idle %.0f%% | regi2c lock wait max %.1f us"
                  " | deadline late max %.1f us, skipped %lu, overlong %lu"
                  " | producer %.0f cycles/sample, %.1f%% of its core\n",
                  st.isrAvgUs, st.isrMaxUs, st.slackPct, st.lockMaxUs, st.wakeLateMaxUs,
                  (unsigned long)st.skipped, (unsigned long)st.overlong, st.mpxCycles, st.producerPct);
    if (reset) out->ResetTiming();
}

static void printStatus()
{
    AudioOutputFM::Stats st = out->GetStats();
    Serial.printf("%s %s %.4f MHz | rate %.0f Hz | buffer %lu/%lu | underruns %lu | late %lu | peak %.2f | "
                  "RSSI %d dBm (AP %s, channel %d) | heap %u, largest %u, min %u | up %lu s\n",
                  isOnAir ? "ON AIR" : "OFF AIR", st.stereo ? "ST" : "MO", out->GetFrequency(), st.measuredHz,
                  (unsigned long)st.fill, (unsigned long)st.size, (unsigned long)st.underruns,
                  (unsigned long)st.isrLate, st.peak, (int)WiFi.RSSI(), WiFi.BSSIDstr().c_str(), (int)WiFi.channel(),
                  (unsigned)free8(), (unsigned)largest8(),
                  (unsigned)heap_caps_get_minimum_free_size(RAM8), (unsigned long)(millis() / 1000));
    printTiming(false);
    // Unused stack (bytes): candidates for trimming if RAM is short
    Serial.printf("stacks free | audio %u, web %u\n",
                  audioTaskH ? (unsigned)uxTaskGetStackHighWaterMark(audioTaskH) : 0,
                  webTaskH ? (unsigned)uxTaskGetStackHighWaterMark(webTaskH) : 0);
}

// Web page + serial console + Wi-Fi supervision (core 0, priority 2): may wait on a slow
// client without starving the decoder
void webTask(void *)
{
    esp_task_wdt_add(nullptr);                 // reboot if this task ever hangs for 30 s
    String line;
    uint32_t wifiDownSince = 0, wifiLastTry = 0;
    for (;;) {
        esp_task_wdt_reset();
        server.handleClient();

        while (Serial.available()) {
            char ch = Serial.read();
            if (ch != '\n' && ch != '\r') { line += ch; continue; }
            line.trim();
            if (line.length()) {
                float v = line.substring(1).toFloat();
                if (line[0] == 'o') {
                    setOnAir(v != 0);
                } else if (line[0] == 'x') {
                    out->SetCarrier(v != 0);
                    Serial.printf("RF %s\n", v != 0 ? "on" : "off");
                } else if (line[0] == 's') {
                    printStatus();
                } else if (line[0] == 't') {
                    printTiming(true);
                } else {
                    Serial.println("o 0|1 (off/on air) | x 0|1 (RF output only) | s (status) | t (timing, then restart the measurement)");
                }
            }
            line = "";
        }

        // Setup access point active: retry the saved Wi-Fi every 60 s, but only while nobody is
        // connected to the access point (a scan/connect can move the AP channel and drop a client).
        // Once connected, close the access point and go back to normal operation.
        if (apMode && cfg.ssid.length()) {
            if (WiFi.status() == WL_CONNECTED) {
                if (WiFi.softAPgetStationNum() == 0) leaveSetupAp();
            } else if (WiFi.softAPgetStationNum() == 0 && millis() - wifiLastTry > 60000) {
                wifiLastTry = millis();
                Serial.printf("Setup AP idle, retrying Wi-Fi %s\n", cfg.ssid.c_str());
                WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
            }
        }

        // Wi-Fi supervision: reconnect every 30 s while down, reboot after 10 minutes
        if (!apMode) {
            if (WiFi.status() == WL_CONNECTED) {
                wifiDownSince = 0;
                checkRoaming();
            } else {
                if (!wifiDownSince) {
                    wifiDownSince = millis();
                    Serial.println("Wi-Fi lost");
                }
                if (millis() - wifiLastTry > 30000) {
                    wifiLastTry = millis();
                    // full begin(), not reconnect(): drops any access point pinned by roaming
                    // and scans every channel again for the strongest one
                    WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
                }
                if (millis() - wifiDownSince > 600000UL) {
                    Serial.println("Wi-Fi down for 10 minutes, restarting");
                    restartAt = millis();
                }
            }
        }

        if (restartAt && (int32_t)(millis() - restartAt) >= 0) {
            out->end();
            ESP.restart();
        }
        vTaskDelay(2);
    }
}

void setup()
{
    Serial.begin(115200);
    delay(300);
    audioLogger = &Serial;                     // ESP8266Audio's own error messages
    loadConfig();

#if defined(FMTX_TUNED_SDKCONFIG)
    // PlatformIO "lowram" build: the framework was rebuilt with smaller Wi-Fi buffer pools. Arduino
    // otherwise overrides them at Wi-Fi init (32 dynamic RX buffers); static-buffer mode makes it use
    // the sdkconfig values instead.
    WiFi.useStaticBuffers(true);
#endif
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setHostname(HOSTNAME);
    // Several access points with the same SSID (repeaters, mesh, a second box): by default the
    // ESP32 stops at the FIRST one it hears, channel by channel, even a weak one in another room.
    // Scan every channel and join the strongest.
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    if (cfg.ssid.length()) {
        WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
        Serial.printf("Wi-Fi %s", cfg.ssid.c_str());
        uint32_t t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) { delay(250); Serial.print('.'); }
    } else {
        Serial.print("No Wi-Fi configured yet.");
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf(" OK, http://%s.local or http://%s\n", HOSTNAME, WiFi.localIP().toString().c_str());
    } else {
        apMode = true;
        // Access point AND station: the web task keeps retrying the saved Wi-Fi in the background
        // (the router may simply be booting slower than the ESP32 after a power cut)
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP(AP_SSID, AP_PASS);
        Serial.printf(" Setup access point \"%s\" (password %s): http://%s\n",
                      AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str());
    }
    setupWeb();

    out = new AudioOutputFM(cfg.freq);
    out->SetStereo(cfg.stereo);                // before begin()
    applyRadio();

    // Task watchdog: the audio and web tasks subscribe themselves; if either hangs for 30 s the
    // ESP32 reboots (and comes back in its saved on/off-air state). The idle tasks are not
    // watched: the HTTP client legitimately spins while waiting for data.
    esp_task_wdt_config_t wdt = {};
    wdt.timeout_ms = 30000;
    wdt.idle_core_mask = 0;
    wdt.trigger_panic = true;
    esp_task_wdt_reconfigure(&wdt);

    // Log failed allocations (ROM printf: allocates nothing, safe from any task). Only the first
    // 20: a synchronous UART print slows the failing task down; the page keeps the full count.
    heap_caps_register_failed_alloc_callback([](size_t size, uint32_t caps, const char *fn) {
        allocFails = allocFails + 1;
        if (allocFails <= 20) {
            // Free vs largest block tells exhaustion from fragmentation; the task tells who asked
            // (wifi = driver RX/TX buffers, tiT = lwIP, audio/web = this sketch)
            esp_rom_printf("[heap] %s: %u bytes failed (caps 0x%x) in task %s, internal free %u, largest %u\n",
                           fn ? fn : "?", (unsigned)size, (unsigned)caps, pcTaskGetName(nullptr),
                           (unsigned)heap_caps_get_free_size(RAM8), (unsigned)heap_caps_get_largest_free_block(RAM8));
        }
    });
    wantOnAir = cfg.onair;
    Serial.printf("FM transmitter %s\n", FW_VERSION);
    Serial.printf("Last reset: %s. Starting %s\n", resetReason(), cfg.onair ? "on air" : "off air");

    // setup() runs on core 1, which the stereo modulator is about to take: start from core 0
    playQ = xQueueCreate(1, sizeof(PlayCmd));
    requestPlay();                             // first station, picked up by the audio task
    // 6 KB: measured peak ~3.4 KB (MP3 and AAC/HE-AAC streams), see "stacks free" in the s command
    xTaskCreatePinnedToCore(audioTask, "audio", 6144, nullptr, 3, &audioTaskH, 0);   // starts the web task itself
}

void loop()
{
    vTaskDelete(nullptr);                      // everything runs in audioTask
}

#endif
