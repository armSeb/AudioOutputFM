# Changelog

## 0.9.8

- **PlatformIO: flash in QIO at 80 MHz**, like the Arduino IDE's "ESP32 Dev Module". PlatformIO's `esp32dev` board defaults to DIO at 40 MHz, up to 4× slower for everything run from flash. In the `esp32dev-lowram` build, the Wi-Fi/PHY code then held the regi2c lock for up to 33 µs, and the stereo loop skipped ~10 samples per second at rest, against ~0.3 with the Arduino IDE build.
- **PlatformIO `esp32dev-lowram`: only the components that break the rebuild are removed** (RainMaker and Insights with its dependencies). pioarduino applies `custom_component_remove` by editing the shared framework packages in `~/.platformio`, and deletes the removed components' include folders. Removing `network_provisioning`, whose headers Arduino's Network/WiFi libraries include, therefore broke every later build. Recovery: delete `~/.platformio/packages/framework-arduinoespressif32` and `framework-arduinoespressif32-libs`, and let PlatformIO download them again.
- **Audio task stack: 10 KB → 6 KB.** The measured peak is about 3.4 KB, MP3 and AAC streams included. Saves 4 KB of internal RAM.
- **Measured with `esp32dev-lowram` (0.9.8, QIO 80 MHz):**
  - Internal heap free after start: 42.9 KB, with the lowest point at 19–30 KB through several stream starts (MP3, AAC 320 kb/s).
  - No failed allocations.
  - At rest: regi2c lock wait 0.9 µs, 0–5 skipped samples per measurement, producer ~500 cycles per sample (24–28 % of core 0).
  - Audio task stack peak 3.3 KB out of 6 KB.

  `esp32dev-lowram` is now the recommended build for 24/7 use.
- **Correction to 0.9.6:** the IRAM options in `sdkconfig.lowram` change nothing; Arduino already keeps Wi-Fi and lwIP code in flash.
- **Earlier measurement with `esp32dev-lowram` (0.9.7):** no failed allocations over 5 station changes, AAC 320 kb/s included; the lowest free heap stayed at 5.1 KB, against 1.1–4 KB with ~20 failed Wi-Fi allocations per stream start before.

## 0.9.7

- **PlatformIO fixes for `esp32dev-lowram`**, found on the first real build:
  - **Partition table in the project.** The hybrid ESP-IDF build looks for the partition file in the project directory, not in the Arduino framework, so the project now has its own `partitions_ota.csv` (same layout as Arduino's `min_spiffs.csv`). All environments use it.
  - **ESP-IDF options moved to `sdkconfig.lowram`**, referenced with `custom_sdkconfig = file://sdkconfig.lowram`. PlatformIO silently drops lines starting with `#` from `platformio.ini`, so the `# CONFIG_… is not set` options (Bluetooth off, Wi-Fi/lwIP code out of IRAM) were never applied before.
  - **Unused managed components removed** (`custom_component_remove`): Insights/Diagnostics, RainMaker, Zigbee, ESP-SR, Modbus, modem, ESP-Hosted, LAN867x, libsodium, QR code, ESP-DSP, libhelix-mp3 (ESP8266Audio has its own). `esp_insights` broke the rebuild (`https_server.crt.S not found`), and the others only cost build time. `mdns` and `littlefs` are kept.
  - **Use pioarduino's PlatformIO core** (`~/.platformio/penv/bin/pio`, or the pioarduino VS Code extension). With the upstream core, SCons 4.11 breaks the ESP-IDF builder (`No module named 'SCons.Tool.FortranCommon'`). This is now documented in `platformio.ini` and the README.
  - Checked: the Bluetooth and buffer options now appear among the replaced sdkconfig values. `esp32dev`, `wrover` and `esp32cam` build. `esp32dev-lowram` gets as far as the ESP component registry, which is unreachable from here.

## 0.9.6

- **PlatformIO project** (`platformio.ini`, pioarduino with Arduino-ESP32 3.3.5) for `FMTransmitterWeb`, with four environments:
  - `esp32dev`: same as the Arduino IDE;
  - `esp32dev-lowram`: framework rebuilt with `CONFIG_LWIP_L2_TO_L3_COPY` (each received frame copied out of its 2.3 KB Wi-Fi buffer at once), smaller Wi-Fi RX/TX buffer pools, Wi-Fi/lwIP code moved out of IRAM, and Bluetooth disabled. The sketch then calls `WiFi.useStaticBuffers(true)` so Arduino keeps these pool sizes;
  - `wrover` and `esp32cam`: PSRAM boards.

  `esp32dev` and `wrover` were built successfully. `esp32dev-lowram` could not be built here, because the ESP component registry was unreachable; its options were checked against the ESP-IDF 5.5 Kconfig.

## 0.9.5

- **FMTransmitterWeb diagnostics.**
  - A failed allocation now logs the requesting task and the internal free/largest figures at that moment, to tell exhaustion from fragmentation.
  - `s` shows the unused stack of the audio and web tasks.

## 0.9.4

- **Faster stereo producer.** It measured ~800 cycles per multiplex sample, 30–41 % of core 0. Changes:
  - the 32-tap dot products are unrolled by 4 with constant offsets: 6.75 instead of ~11 instructions per tap;
  - no more `lroundf()` or `fmaxf()` library calls, and the ring write is inlined (an IRAM function can't be inlined into flash code);
  - the limiter's division runs only on attack, with one Newton step otherwise;
  - the function is built at `-O2`.

  Same output to float rounding (checked against the old code: 7·10⁻⁸ of full scale). The cycle gain still needs measuring on hardware.
- **Station selection writes only its index to NVS**, not the whole configuration. Every flash write freezes core 1 and the carrier; changing stations used to skip about 500 samples.
- **Heap diagnostics in FMTransmitterWeb:**
  - each new low of the internal heap is logged, with the time since the last stream start;
  - failed allocations are logged (the first 20) and counted on the page.

## 0.9.3

- **Timing instrumentation**, a first step before moving the multiplex computation to core 1:
  - writer: average and maximum time, and idle share of each sample period;
  - longest wait for the regi2c lock;
  - longest delay past a stereo deadline;
  - samples skipped to catch up, and writes longer than one period;
  - producer: cycles per ring sample, and its share of the core.

  The counters are cumulative until `ResetTiming()`, so the web page's polling doesn't erase them. The examples print them with `t` (`T` in ToneTest), and `FMTransmitterWeb` also prints them with `s`. No behaviour change.

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
