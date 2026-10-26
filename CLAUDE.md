# CLAUDE.md

Guidance for AI assistants (and humans) working on this repository.

## What this is

An ESP8266Audio output (`AudioOutputFM`) that turns a classic ESP32 into an FM broadcast transmitter. It works by rewriting the APLL's fractional divider in real time, and the clock leaves on GPIO0 via `CLK_OUT1`. Mono runs at the source rate from a timer ISR. Stereo and RDS run as a multiplex at 133.333 kHz from a busy loop that owns core 1. `examples/FMTransmitterWeb` is the full appliance: web UI, NVS, MP3/AAC, OTA, 24/7 self-healing.

The README is the user-facing reference, and its "How it works" and "Memory" sections explain the design. Read them before changing the modulator or the memory layout.

## Layout

```
src/AudioOutputFM.{h,cpp}        the library (one class)
examples/ToneTest                no Wi-Fi: stereo tones + RDS, separation tuning
examples/WebRadioFM              minimal stream → FM, serial console
examples/FMTransmitterWeb        full app: .ino + web_page.h (HTML/JS) + stream_sniff.h (MP3/AAC detection)
extras/sim/                      numpy ports of the resampler/multiplex and of the RDS encoder
extras/*.png                     output filter schematic and response
```

## Building

The library targets **Arduino-ESP32 3.3.x** (ESP-IDF 5.5) and **ESP8266Audio ≥ 1.9.0**. With arduino-cli:

```sh
arduino-cli core install esp32:esp32@3.3.5 \
  --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli lib install ESP8266Audio
arduino-cli compile -b esp32:esp32:esp32:PartitionScheme=min_spiffs \
  --library . examples/FMTransmitterWeb
```

- PlatformIO: `platformio.ini` at the root builds `FMTransmitterWeb` (pioarduino). `esp32dev-lowram` rebuilds the framework with `custom_sdkconfig` (L2-to-L3 copy, smaller Wi-Fi pools) and defines `FMTX_TUNED_SDKCONFIG`, which makes the sketch call `WiFi.useStaticBuffers(true)` so Arduino doesn't override those pools. Keep the default dependency finder: `deep+` evaluates `#if CONFIG_IDF_TARGET_ESP32` without the sdkconfig and drops WiFi.h.
- `FMTransmitterWeb` is about 1.29 MB. It needs `PartitionScheme=min_spiffs`: the default partition is 98 % full, and with PSRAM enabled the sketch overflows it.
- On other targets (S2/S3/C3…), everything compiles to an empty `setup()`/`loop()`. CI checks that path on the ESP32-S3.
- Build with **no warnings** (`--warnings all`), apart from those inside ESP8266Audio and the core.

## Testing

There is no unit-test harness, and real validation needs hardware: an ESP32, a receiver, and ideally a NanoVNA or SDR. The maintainer (Seb) runs the hardware tests. Before handing over a change:

1. **Compile all examples** for `esp32:esp32:esp32`, with `--warnings all`.
2. **Simulate DSP changes** with `extras/sim/simstereo.py` or `simrds.py`, after porting the change there. These scripts mirror the C code line by line, so keep them in sync.
3. **Code going to IRAM** (ISR, `_modLoop`, anything touching `ring_`): check the generated assembly with `xtensa-esp32-elf-objdump -d`. See the invariants below.
4. **Say what was and wasn't verified.** "Compiles" is not "works on hardware".

## Invariants: break these and things fail in non-obvious ways

**APLL / regi2c**
- `sdm` is written as DSDM1 then DSDM0, high byte first. DSDM2 must never change while modulating. `IsFrequencyUsable()` rejects the ±80 kHz windows around multiples of 10 MHz (89.92–90.08, 99.92–100.08 MHz). Don't relax it: a DSDM2 carry causes a ~10 MHz glitch.
- Every regi2c access holds the IDF lock (`regi2c_enter_critical`). The stereo loop's "posted" writes use the ROM register format (`0x6000E000 + 4·host`, bit 24 = write, bit 25 = busy): post a write, then wait for busy before the next one.
- Rev0 silicon ignores `sdm0`/`sdm1`, and `begin()` refuses it.

**Stereo loop and cores**
- In stereo, `_modLoop` owns core `cpu_` (default 1) at `configMAX_PRIORITIES − 2`. That is one level below the IPC task, so flash writes (NVS, OTA) can still stall the core. Never raise it to the maximum.
- Nothing else may run on that core: decoders, web server and console go on core 0. `setup()` runs on core 1, so tasks that must start after the modulator are created from a core-0 task.
- The loop and the ISR must not call anything that executes from flash. Keep them in IRAM.
- The stereo timing is 1800 CPU cycles at 240 MHz. The CPU must be at 240 MHz before `begin()`.

**Memory (classic ESP32, no PSRAM)**
- **IRAM accepts 32-bit integer loads and stores only.** FPU `lsi`/`ssi` raise `LoadStoreError` even when aligned, and so do 8- or 16-bit accesses.
  - `ring_` may live in IRAM. It is `uint32_t*` with int16 samples packed two per word, and `ringGet`/`ringPut` access it through `volatile` pointers so GCC can't narrow the access.
  - Don't put float data or byte buffers in IRAM.
- `taps_` is Q15 in DRAM. In IRAM, the 4M reads/s competed with instruction fetch and slowed playback.
- `ESP.getMaxAllocHeap()` includes IRAM. For "can a buffer fit", use `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`.
- **FMTransmitterWeb allocation order matters:**
  1. The arena (decoder + stream buffer) first, from the ~110 KB boot block.
  2. Then the modulator.
  3. Then the web task.

  The arena is carved per station: MP3 gets 30 KB of decoder plus ~79 KB of buffer; HE-AAC gets 88 KB (`AAC_MEM`) plus ~19 KB. Don't add separate large allocations at runtime: the heap is fragmented.
- The modulator keeps `ring_`/`taps_` across `end()`/`begin()` so going off and on air never re-allocates.

**Arduino IDE quirks**
- The IDE inserts auto-generated prototypes before the first function. Types used in function signatures (`PlayCmd`…) must be declared above it.
- The IDE's ctags preprocessor mangles long raw string literals containing JavaScript. The web page lives in `web_page.h`, not in the `.ino`.

**Streams**
- Open ICY/HTTP sources with the default constructor, then `useHTTP10()`, then `open(url)`. HTTP/1.1 chunked encoding otherwise corrupts the audio.
- Codec detection (`stream_sniff.h`) needs two consecutive ADTS headers to call a stream AAC. The sniffed bytes are replayed through `PrefixSource`.

**RDS**
- RadioText is always 64 characters, space-padded, with no 0x0D, and the A/B toggle is off by default. Both choices fix real receivers (Yamaha RX-V673). An identical text must not restart the cycle.

## Conventions

- **Language and style.** English everywhere (code, comments, docs). Comments explain *why*, especially for hardware facts found by measurement. Keep the GPL-3.0-or-later header on source files.
- **Style.** 4-space indent, `camelCase_` members, no exceptions, no RTTI, no dynamic allocation in the ISR or the loop.
- **Versioning.** Bump `version` in `library.properties`, `library.json` and `AUDIOOUTPUTFM_VERSION` in `AudioOutputFM.h` and add a `CHANGELOG.md` entry for every user-visible change.
- **README.** Keep the README's validation table honest: mark what was tested on hardware and what wasn't.
- **Safety.** This is RF. Don't add features that raise output power or widen the band, and keep the warnings in the docs.
