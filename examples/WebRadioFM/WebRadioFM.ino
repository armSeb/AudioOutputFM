// WebRadioFM.ino - MP3 web radio -> FM transmitter (mono or stereo), using AudioOutputFM
// Same structure as ESP8266Audio's StreamMP3FromHTTP example: only the output changes.
//
// Hardware: RF out on GPIO0, through a DC-blocking capacitor (GPIO0 is a boot strapping
// pin) and a low-pass filter (the output is a square wave, rich in harmonics).
// A few centimeters of wire as antenna. Check your local regulations.
//
// Serial commands (115200 baud):
//   f 100.1   frequency (MHz)          g 1.0   gain after AGC      a 1|0   AGC on/off
//   d 75000   deviation (Hz)           p 50    pre-emphasis (us, 0 = none)
//   x 1|0     carrier on/off           s       statistics       t   timing (then restart it)
//   stereo only:  k 1.11  L-R gain      h -22   38 kHz phase trim (deg)     P 0.09  pilot level
//                 R 1|0   RDS on/off    T text  RadioText (the ICY StreamTitle is sent automatically)
//                 C 0|1|2 RDS charset: 0 RDS table, 1 ASCII (no accents), 2 Latin-1 (resends the title)

#include <Arduino.h>

#if !defined(ESP32) || !defined(CONFIG_IDF_TARGET_ESP32)
// AudioOutputFM needs a classic ESP32 (APLL + CLK_OUT1 on GPIO0)
void setup() {}
void loop() {}

#else
#include <WiFi.h>
#include "esp_task_wdt.h"
#include "AudioFileSourceICYStream.h"
#include "AudioFileSourceBuffer.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputFM.h"

// Enter your WiFi setup here:
#ifndef STASSID
#define STASSID "your-ssid"
#define STAPSK  "your-password"
#endif

const char *ssid = STASSID;
const char *password = STAPSK;

// Same stream as the StreamMP3FromHTTP example (a mono AM simulcast: use a stereo
// music stream to hear the stereo mode, e.g. the commented one below)
const char *URL = "http://kvbstreams.dyndns.org:8000/wkvi-am";
// const char *URL = "http://frequence3.net-radio.fr/frequence3-256.mp3";

// Stereo multiplex (experimental): 19 kHz pilot + L-R on 38 kHz, 133.3 kHz modulation rate.
// The stereo modulator owns core 1 entirely, so this sketch then runs the stream, the
// decoder and the serial console from a task on core 0 instead of loop().
#define FM_STEREO 1

AudioFileSourceICYStream *file = nullptr;
AudioFileSourceBuffer    *buff = nullptr;
AudioGeneratorMP3        *mp3  = nullptr;
AudioOutputFM            *out  = nullptr;
uint32_t retryAt = 0;
float kTrim = 1.11f, hTrim = -22.0f;
String lastTitle;

void MDCallback(void *, const char *type, bool, const char *str)
{
    Serial.printf("[ICY] %s = %s\n", type, str);
    if (out && !strcmp(type, "StreamTitle") && str[0]) {    // song title -> RDS RadioText
        lastTitle = str;
        out->SetRdsText(str);
    }
}

void stopStream()
{
    if (mp3) { mp3->stop(); delete mp3; mp3 = nullptr; }
    delete buff; buff = nullptr;
    delete file; file = nullptr;
}

void startStream()
{
    Serial.printf("Connecting to %s\n", URL);
    // HTTP/1.0: a server may otherwise answer HTTP/1.1 with "Transfer-Encoding: chunked", and
    // the chunk headers would end up in the audio data (sync losses, choppy "accelerated" sound).
    // The flag must be set before open(), hence the default constructor.
    file = new AudioFileSourceICYStream();
    file->useHTTP10();
    file->open(URL);
    file->RegisterMetadataCB(MDCallback, nullptr);
    buff = new AudioFileSourceBuffer(file, 64 * 1024);
    mp3  = new AudioGeneratorMP3();
    if (!mp3->begin(buff, out)) {
        Serial.println("Failed, retrying in 2 s");
        stopStream();
        retryAt = millis() + 2000;
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

void handleSerial()
{
    static String line;
    while (Serial.available()) {
        char c = Serial.read();
        if (c != '\n' && c != '\r') { line += c; continue; }
        line.trim();
        if (!line.length()) continue;
        float v = line.substring(1).toFloat();
        switch (line[0]) {
            case 'f':
                if (out->SetFrequency(v)) Serial.printf("Carrier %.4f MHz\n", out->GetFrequency());
                else Serial.println("Frequency rejected (outside 88-108 MHz, or within ~80 kHz of 90/100 MHz)");
                break;
            case 'g': out->SetGain(v); break;
            case 'a': out->SetAGC(v != 0); break;
            case 'd': out->SetDeviation(v); break;
            case 'p': out->SetPreemphasis(v); break;
            case 'x': out->SetCarrier(v != 0); break;
            case 'k': kTrim = v; out->SetStereoTrim(kTrim, hTrim); break;
            case 'h': hTrim = v; out->SetStereoTrim(kTrim, hTrim); break;
            case 'P': out->SetPilotLevel(v); break;
            case 'R': out->SetRDS(v != 0); break;
            case 'C': {
                static const char *names[] = { "RDS table (IEC 62106)", "ASCII, accents stripped", "Latin-1 bytes" };
                int cs = (int)v;
                if (cs < 0 || cs > 2) cs = 0;
                out->SetRdsCharset(cs);
                out->SetRdsText(lastTitle.length() ? lastTitle.c_str() : "Accents: e\xC3\xA9 e\xC3\xA8 a\xC3\xA0 c\xC3\xA7 u\xC3\xB9 E\xC3\x89");
                Serial.printf("RDS charset: %s\n", names[cs]);
                break;
            }
            case 'T': {
                String t = line.substring(1);
                t.trim();
                out->SetRdsText(t.c_str());
                break;
            }
            case 's': {
                AudioOutputFM::Stats s = out->GetStats();
                Serial.printf("%s %.4f MHz | rate %.0f/%.0f Hz | buffer %lu/%lu | underruns %lu | prio %d max %.2f us, late %lu"
                              " | 2 writes %.0f%% | AGC x%.2f | peak %.2f | clips %lu | RSSI %d\n",
                              s.stereo ? "ST" : "MO", out->GetFrequency(), s.measuredHz, s.rateHz,
                              (unsigned long)s.fill, (unsigned long)s.size, (unsigned long)s.underruns,
                              s.isrPriority, s.isrMaxUs, (unsigned long)s.isrLate, s.twoWritesPct,
                              s.agcGain, s.peak, (unsigned long)s.clips, WiFi.RSSI());
                break;
            }
            case 't': printTiming(true); break;
            default: Serial.println("f <MHz> | g <gain> | a <0|1> | d <Hz> | p <us> | x <0|1> | k <gain> | h <deg> | P <level> | R <0|1> | T <text> | C <0..2> | s | t");
        }
        line = "";
    }
}

// Stream + decoder + console: called from loop() in mono, from a core-0 task in stereo
void service()
{
    if (mp3 && mp3->isRunning()) {
        if (!mp3->loop()) {
            Serial.println("Stream lost, reconnecting in 2 s");
            stopStream();
            retryAt = millis() + 2000;
        }
    } else if (!mp3 && (int32_t)(millis() - retryAt) >= 0) {
        startStream();
    }
    handleSerial();
}

void audioTask(void *)
{
    startStream();                             // mp3->begin() calls out->begin(): core 1 is taken from here on
    Serial.printf("Carrier %.4f MHz\n", out->GetFrequency());
    for (;;) {
        service();
        vTaskDelay(1);
    }
}

void setup()
{
    Serial.begin(115200);
    delay(300);

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(ssid, password);
    Serial.print("Wi-Fi");
    while (WiFi.status() != WL_CONNECTED) { delay(250); Serial.print('.'); }
    Serial.printf(" OK %s\n", WiFi.localIP().toString().c_str());

    out = new AudioOutputFM(100.1f);           // frequency, ISR core (1 by default)
    out->SetPreemphasis(50);                   // 50 us Europe / 75 us Americas: match your receivers
    out->SetAGC(true, 0.25f);
    out->SetStereo(FM_STEREO);                 // before begin()
    out->SetStereoTrim(kTrim, hTrim);          // separation tuning, measured on a car radio
    out->SetRDS(FM_STEREO);                    // RDS needs the multiplex (stereo) mode
    out->SetRdsPS("ESP32 FM");                 // station name, 8 characters
    out->SetRdsPTY(10);                        // pop music (RDS table)

#if FM_STEREO
    // The decoder task spins inside the HTTP client while waiting for data: keep the task
    // watchdog from tripping on a starved IDLE0 (same fix as the original mono sketch).
    esp_task_wdt_config_t wdt = {};
    wdt.timeout_ms = 5000;
    wdt.idle_core_mask = 0;
    wdt.trigger_panic = false;
    esp_task_wdt_reconfigure(&wdt);
    // setup() runs on core 1, which the modulator is about to take: start everything from core 0
    xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 3, nullptr, 0);
#else
    if (!out->begin()) {                       // optional: the generator calls it too
        Serial.println("AudioOutputFM: start failed");
        while (true) delay(1000);
    }
    Serial.printf("Carrier %.4f MHz\n", out->GetFrequency());
    startStream();
#endif
}

void loop()
{
#if FM_STEREO
    vTaskDelete(nullptr);                      // everything runs in audioTask
#else
    service();
#endif
}

#endif
