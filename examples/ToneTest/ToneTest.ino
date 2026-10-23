// ToneTest.ino - test tones for AudioOutputFM, no Wi-Fi and no stream needed
//
// Useful to bring up the hardware (filter, antenna, receiver) and to tune the stereo
// separation: send a tone on the left channel only, then adjust k and h until the right
// channel is as quiet as possible on your receiver.
//
// Serial commands (115200 baud):
//   l / r / b / m   tone on left only, right only, both in phase, both out of phase (L-R only)
//   t 1000          tone frequency (Hz)        v 0.5   tone level (0..1)
//   f 100.1         carrier (MHz)              x 1|0   carrier on/off
//   k 1.11          L-R gain trim              h -22   38 kHz phase trim (degrees)
//   s               statistics
//
// Hardware: RF out on GPIO0 through a DC-blocking capacitor and a low-pass filter.
// Check your local regulations.

#include <Arduino.h>

#if !defined(ESP32) || !defined(CONFIG_IDF_TARGET_ESP32)
void setup() {}
void loop() {}
#else

#include "AudioOutputFM.h"

static const int RATE = 44100;

AudioOutputFM *out = nullptr;
volatile float toneHz = 1000.0f, level = 0.5f, kTrim = 1.11f, hTrim = -22.0f;
volatile char mode = 'l';                     // l, r, b, m

// Feeds the modulator like a decoder would: as fast as it accepts samples
void toneTask(void *)
{
    out->begin();                              // in stereo, core 1 belongs to the modulator from here on
    Serial.printf("Carrier %.4f MHz, tone on the left channel\n", out->GetFrequency());
    float ph = 0.0f;
    for (;;) {
        int16_t s[2];
        float v = level * sinf(ph) * 32767.0f;
        int16_t a = (int16_t)v;
        switch (mode) {
            case 'l': s[0] = a; s[1] = 0; break;
            case 'r': s[0] = 0; s[1] = a; break;
            case 'b': s[0] = a; s[1] = a; break;
            default:  s[0] = a; s[1] = -a; break;
        }
        while (!out->ConsumeSample(s)) vTaskDelay(1);   // ring full: let the modulator drain it
        ph += 2.0f * (float)M_PI * toneHz / RATE;
        if (ph > 2.0f * (float)M_PI) ph -= 2.0f * (float)M_PI;
    }
}

void consoleTask(void *)
{
    String line;
    for (;;) {
        while (Serial.available()) {
            char c = Serial.read();
            if (c != '\n' && c != '\r') { line += c; continue; }
            line.trim();
            if (line.length()) {
                float v = line.substring(1).toFloat();
                switch (line[0]) {
                    case 'l': case 'r': case 'b': case 'm':
                        mode = line[0];
                        Serial.printf("Tone: %s\n", mode == 'l' ? "left" : mode == 'r' ? "right" : mode == 'b' ? "both, in phase" : "both, out of phase");
                        break;
                    case 't': toneHz = constrain(v, 20.0f, 15000.0f); break;
                    case 'v': level = constrain(v, 0.0f, 1.0f); break;
                    case 'f':
                        if (out->SetFrequency(v)) Serial.printf("Carrier %.4f MHz\n", out->GetFrequency());
                        else Serial.println("Frequency rejected (outside 88-108 MHz, or within ~80 kHz of 90/100 MHz)");
                        break;
                    case 'x': out->SetCarrier(v != 0); break;
                    case 'k': kTrim = v; out->SetStereoTrim(kTrim, hTrim); Serial.printf("k %.3f h %.1f\n", kTrim, hTrim); break;
                    case 'h': hTrim = v; out->SetStereoTrim(kTrim, hTrim); Serial.printf("k %.3f h %.1f\n", kTrim, hTrim); break;
                    case 's': {
                        AudioOutputFM::Stats st = out->GetStats();
                        Serial.printf("%.4f MHz | rate %.0f Hz | buffer %lu/%lu | underruns %lu | peak %.2f\n",
                                      out->GetFrequency(), st.measuredHz, (unsigned long)st.fill,
                                      (unsigned long)st.size, (unsigned long)st.underruns, st.peak);
                        break;
                    }
                    default: Serial.println("l|r|b|m | t <Hz> | v <0..1> | f <MHz> | x <0|1> | k <gain> | h <deg> | s");
                }
            }
            line = "";
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void setup()
{
    Serial.begin(115200);
    delay(300);
    out = new AudioOutputFM(100.1f);
    out->SetRate(RATE);
    out->SetChannels(2);
    out->SetStereo(true);                      // before begin()
    out->SetStereoTrim(kTrim, hTrim);
    out->SetAGC(false);                        // a test tone must keep its level
    out->SetPreemphasis(50);
    out->SetRDS(true);
    out->SetRdsPS("TONETEST");
    out->SetRdsText("AudioOutputFM test tone");
    // The stereo modulator takes core 1 for good: everything else runs on core 0
    xTaskCreatePinnedToCore(consoleTask, "console", 4096, nullptr, 2, nullptr, 0);
    xTaskCreatePinnedToCore(toneTask, "tone", 4096, nullptr, 3, nullptr, 0);
}

void loop()
{
    vTaskDelete(nullptr);
}

#endif
