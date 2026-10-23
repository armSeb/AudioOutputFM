# Changelog

## 0.9.2

- **FMTransmitterWeb shows its firmware version.** The System section of the page, the boot log and `/api/state` (`status.fw`) give the library version and build time. `AUDIOOUTPUTFM_VERSION` is now defined in `AudioOutputFM.h`.
- **Default stations.** The mono AM test stream (KVB) is no longer among them. An existing station list in NVS is kept: delete it from the page if needed.

## 0.9.1

- **Accurate pre-emphasis.** A first-order IIR is fitted at run time to the analog 50/75 µs curve, by weighted least squares over 0–15 kHz. Error at 44.1/48 kHz is now 0.1 dB or less; the previous first difference was 1.7 dB short at 15 kHz, which gave slightly dull treble. The extra cost is one multiply-add per sample and channel. Not yet validated on hardware.

## 0.9.0: first public release

- **New `ToneTest` example.** Stereo test tones (left, right, in phase, out of phase) and RDS, with no Wi-Fi. It is meant for hardware bring-up and separation tuning.
- **Streams opened in HTTP/1.0.** The examples now call ESP8266Audio's own `useHTTP10()`, so ESP8266Audio no longer needs patching.
- **FMTransmitterWeb, PSRAM boards.** It puts a 256 KB arena in PSRAM when available. Heap health figures now count internal RAM only. Untested on hardware.
- **Compiler warnings fixed.** All examples build clean with `--warnings all`.
- **Repository packaging.** README rewrite, CLAUDE.md, LICENSE, `library.json`, `keywords.txt`, and a CI compile workflow. The simulations are in `extras/sim` and updated to Q15 taps and to the current RadioText format.

## 0.8.x: memory

- **0.8.3.** The heap guard only reboots after 15 s of real shortage: less than 10 KB free, or a largest block under 2 KB. The page shows the largest block, current and lowest.
- **0.8.2.** Resampler table moved back to DRAM, in Q15 (8.3 KB instead of 16.5 KB, same response). In IRAM, it slowed playback.
- **0.8.1.** IRAM accesses limited to 32-bit integer instructions: FPU loads/stores raised `LoadStoreError`.
- **0.8.0.** Stereo ring moved to the spare IRAM heap, packed two samples per word. The sketch now measures byte-addressable RAM, because `ESP.getMaxAllocHeap()` counts IRAM.

## 0.7.x: AAC

- **0.7.3.** Decoder memory and stream buffer share one arena, carved per station: MP3 gets a ~79 KB buffer, AAC ~19 KB.
- **0.7.1–0.7.2.** Allocation order and fallbacks: AAC is dropped cleanly, keeping MP3, when RAM is short.
- **0.7.0.** AAC-LC and HE-AAC streams, detected automatically from the ADTS frames. The sniffed bytes are replayed to the decoder.

## 0.6.x: 24/7 operation

- **0.6.3.** Joins the strongest access point when several share the SSID, and roams when the signal is weak. The page shows the BSSID and channel. The web page moved to `web_page.h` to work around the Arduino IDE preprocessor.
- **0.6.2.** The setup access point retries the saved Wi-Fi every 60 s while nobody is connected to it.
- **0.6.1.** No Wi-Fi credentials in the code: the setup access point starts on first boot.
- **0.6.0.** On air / off air from the page and `POST /api/onair`. Also: task watchdog, Wi-Fi supervision, stalled-stream restart, heap guard, OTA, uptime and reset cause on the page, and a 32 KB stream buffer plus MP3 decoder pre-allocated.

## 0.5.x: web transmitter and RDS fixes

- **`FMTransmitterWeb`.** Stations, transmitter settings and Wi-Fi from a browser, stored in NVS. The audio and web tasks are separate.
- **RDS character sets.** RDS table, ASCII, or Latin-1. UTF-8 and Latin-1 input are both accepted.
- **RadioText format.** Always 64 characters, without 0x0D; the A/B toggle is optional and off by default; duplicate titles are ignored. These fix the display on a Yamaha RX-V673.

## 0.3–0.4: stereo and RDS

- **Stereo modulator.** A busy loop owns core 1 at 133.333 kHz, with posted regi2c writes (format recovered from the ESP32 ROM).
- **Resampler and multiplex.** Polyphase resampler (256 × 32, Kaiser sinc), pilot NCO, and 38 kHz = 2 × pilot phase plus trim (−22°, measured).
- **RDS encoder.** Groups 0A and 2A, locked to the pilot. The ICY title is sent as RadioText.

## 0.1–0.2: mono

- **Mono FM from the APLL on GPIO0.** Timer ISR at the source rate, with low-pass, AGC, pre-emphasis, limiter and noise-shaped quantization.
- **Band and boundaries.** Restricted to 88–108 MHz, with the DSDM2 boundary windows rejected.
- **Packaged as an ESP8266Audio output**, with the `WebRadioFM` example.
