/*
    AudioOutputFM
    ESP8266Audio output: mono or stereo FM transmitter using the ESP32 APLL on GPIO0.
    See AudioOutputFM.h and README.md for the principle of operation.

    Copyright (C) 2026  Seb
    Code written by Claude (Anthropic), tested on hardware by Seb.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "AudioOutputFM.h"

#if defined(ESP32) && defined(CONFIG_IDF_TARGET_ESP32)

#include <math.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "soc/rtc.h"
#include "soc/io_mux_reg.h"
#include "driver/gpio.h"
#include "clk_ctrl_os.h"
#include "esp_private/regi2c_ctrl.h"   // internal IDF API: may change in future IDF releases
#include "soc/regi2c_apll.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

[[maybe_unused]] static const char *TAG = "AudioOutputFM";

// DSDM0 and DSDM1 are full 8-bit registers: plain writes, no read-modify-write needed.
static_assert(I2C_APLL_DSDM0_MSB == 7 && I2C_APLL_DSDM0_LSB == 0, "DSDM0 is not a full register");
static_assert(I2C_APLL_DSDM1_MSB == 7 && I2C_APLL_DSDM1_LSB == 0, "DSDM1 is not a full register");

// Internal I2C master, as used by the ROM (rom_chip_i2c_writeReg, ESP32 classic):
// one command register per host at 0x6000E000 + 4 * host_id.
// Word: bits 0-7 block id, 8-15 register, 16-23 data, bit 24 = write, bit 25 = busy.
#define FM_RI2C_REG   (0x6000E000 + 4 * I2C_APLL_HOSTID)
#define FM_RI2C_WR    (1u << 24)
#define FM_RI2C_BUSY  (1u << 25)

struct FmTimerCtx {
    AudioOutputFM    *self;
    SemaphoreHandle_t done;
    bool              ok;
    int               prio;
};

static float s_sin[1024];
static bool  s_sinReady = false;

// ---------------------------------------------------------------------------------------------
// Interrupt: integer only (no FPU in ISRs on Xtensa), everything in IRAM.
// ---------------------------------------------------------------------------------------------

static inline IRAM_ATTR uint32_t ccount()
{
    uint32_t c;
    __asm__ __volatile__("rsr %0, ccount" : "=a"(c));
    return c;
}

static inline IRAM_ATTR void ri2c_wait_idle()
{
    volatile uint32_t *r = (volatile uint32_t *)FM_RI2C_REG;
    for (int n = 0; n < 4000 && (*r & FM_RI2C_BUSY); n++) { }
}

// Start a write and return without waiting for the bus transaction to complete.
static inline IRAM_ATTR void ri2c_post(uint8_t reg, uint8_t val)
{
    ri2c_wait_idle();   // previous transaction (normally long finished)
    *(volatile uint32_t *)FM_RI2C_REG = I2C_APLL | ((uint32_t)reg << 8) | ((uint32_t)val << 16) | FM_RI2C_WR;
}

static bool IRAM_ATTR fm_on_alarm(gptimer_handle_t, const gptimer_alarm_event_data_t *, void *ctx)
{
    return static_cast<AudioOutputFM *>(ctx)->_isr();
}

// The ring can live in the ESP32's spare IRAM (~37 KB free, usually unused): that saves 16 KB of
// byte-addressable DRAM, which an AAC decoder needs. (Not the resampler table: it is read 32 times
// per output sample, and IRAM data reads compete with the instruction fetches of both cores.) IRAM only allows 32-bit
// loads and stores, so the int16 ring is packed two samples per word. A put rewrites the word's
// other half with the value it already holds (the previous sample, or an unread slot), so the
// reader on the other core never sees a wrong sample.
// volatile: stops the compiler from narrowing these into 16-bit loads/stores, which IRAM rejects.
static inline IRAM_ATTR int16_t ringGet(const uint32_t *r, uint32_t i)
{
    uint32_t w = ((const volatile uint32_t *)r)[i >> 1];
    return (int16_t)((i & 1) ? (w >> 16) : (w & 0xFFFF));
}

static inline IRAM_ATTR void ringPut(uint32_t *r, uint32_t i, int16_t v)
{
    volatile uint32_t *p = &r[i >> 1];
    uint32_t w = *p;
    w = (i & 1) ? ((w & 0x0000FFFFu) | ((uint32_t)(uint16_t)v << 16)) : ((w & 0xFFFF0000u) | (uint16_t)v);
    *p = w;
}

// 32-bit-only memory first (IRAM), byte-addressable internal RAM as a fallback
static void *allocWords(size_t bytes)
{
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_EXEC | MALLOC_CAP_32BIT);
    if (!p) p = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p;
}

bool IRAM_ATTR AudioOutputFM::_isr()
{
    uint32_t t0 = ccount();

    int32_t sdm = center_;
    uint32_t r = rd_.load(std::memory_order_relaxed);
    if (r != wr_.load(std::memory_order_acquire)) {
        sdm += ringGet(ring_, r & mask_);
        rd_.store(r + 1, std::memory_order_release);
        playing_ = true;
    } else if (playing_) {
        playing_ = false;                       // ring empty: park the carrier at center (silence)
        underruns_ = underruns_ + 1;
    }

    int32_t diff = sdm ^ last_;
    if (diff) {
        // DSDM2 only changes on a boundary crossing, which IsFrequencyUsable() rules out.
        if (diff & 0x3F0000) {
            ri2c_wait_idle();
            REGI2C_WRITE_MASK(I2C_APLL, I2C_APLL_DSDM2, (sdm >> 16) & 0x3F);
        }
        if (isrPosted_) {
            // Under the IDF regi2c lock, but without waiting for the bus: nobody else
            // writes the APLL host, and the lock keeps IDF users serialized with us.
            regi2c_enter_critical();
            if (diff & 0x00FF00) {
                ri2c_post(I2C_APLL_DSDM1, (sdm >> 8) & 0xFF);
                twoWrites_ = twoWrites_ + 1;
            }
            if (diff & 0x0000FF) ri2c_post(I2C_APLL_DSDM0, sdm & 0xFF);
            regi2c_exit_critical();
        } else {
            if (diff & 0x00FF00) {
                REGI2C_WRITE(I2C_APLL, I2C_APLL_DSDM1, (sdm >> 8) & 0xFF);
                twoWrites_ = twoWrites_ + 1;
            }
            if (diff & 0x0000FF) REGI2C_WRITE(I2C_APLL, I2C_APLL_DSDM0, sdm & 0xFF);
        }
        last_ = sdm;
    }

    isrCount_ = isrCount_ + 1;
    uint32_t dt = ccount() - t0;
    if (dt > isrMaxCyc_) isrMaxCyc_ = dt;
    if (dt > lateCyc_) isrLate_ = isrLate_ + 1;
    return false;                               // no task woken
}

// ---------------------------------------------------------------------------------------------
// Construction / lifecycle
// ---------------------------------------------------------------------------------------------

AudioOutputFM::AudioOutputFM(float mhz, int cpu)
    : reqMhz_(mhz), cpu_(cpu ? 1 : 0)
{
    hertz = 44100;
    channels = 2;
    gainF2P6 = 1 << 6;
}

AudioOutputFM::~AudioOutputFM()
{
    end();
    if (ring_) {
        heap_caps_free(ring_);
        ring_ = nullptr;
    }
    if (taps_) {
        heap_caps_free(taps_);
        taps_ = nullptr;
    }
}

bool AudioOutputFM::SetBufferSize(int samples)
{
    if (running_) return false;
    uint32_t n = 256;
    while (n < (uint32_t)samples && n < 32768) n <<= 1;
    if (ring_ && n != size_) {
        heap_caps_free(ring_);
        ring_ = nullptr;
    }
    size_ = n;
    mask_ = n - 1;
    sizeSet_ = true;
    return true;
}

bool AudioOutputFM::SetStereo(bool enable)
{
    if (running_) return false;
    stereo_ = enable;
    dspDirty_ = true;
    return true;
}

void AudioOutputFM::SetStereoTrim(float diffGain, float phaseDeg)
{
    kDiff_ = diffGain;
    phaseDeg_ = phaseDeg;
    dspDirty_ = true;
}

void AudioOutputFM::SetPilotLevel(float fraction)
{
    pilot_ = fraction < 0.0f ? 0.0f : (fraction > 0.12f ? 0.12f : fraction);
    dspDirty_ = true;
}

void AudioOutputFM::SetPostedWrites(bool on)
{
    posted_ = on;
    isrPosted_ = on || stereo_;
}

bool AudioOutputFM::begin()
{
    if (running_) return true;

    if (!IsFrequencyUsable(reqMhz_)) {
        ESP_LOGE(TAG, "%.3f MHz is not usable (range or DSDM2 boundary)", reqMhz_);
        return false;
    }
    if (ESP.getChipRevision() == 0) {
        ESP_LOGE(TAG, "ESP32 rev0 ignores sdm0/sdm1: modulation impossible");
        return false;
    }

    if (!s_sinReady) {
        for (int i = 0; i < 1024; i++) s_sin[i] = sinf(2.0f * (float)M_PI * i / 1024.0f);
        s_sinReady = true;
    }

    if (stereo_ && !sizeSet_ && size_ < 8192) {      // ~61 ms at 133 kHz
        if (ring_) { heap_caps_free(ring_); ring_ = nullptr; }
        size_ = 8192;
        mask_ = 8191;
    }
    if (!ring_) {
        // Must be internal RAM: the ISR reads it
        ring_ = (uint32_t *)allocWords(size_ * sizeof(int16_t));    // size_ is a power of 2 >= 256
        if (!ring_) {
            ESP_LOGE(TAG, "cannot allocate the ring buffer (%u samples)", (unsigned)size_);
            return false;
        }
    }
    if (stereo_ && !taps_) {
        taps_ = (int16_t *)heap_caps_malloc((RS_PHASES / 2 + 1) * RS_TAPS * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!taps_) {
            ESP_LOGE(TAG, "cannot allocate the resampler table");
            return false;
        }
        designedFor_ = 0;
    }
    wr_.store(0);
    rd_.store(0);
    playing_ = false;
    isrPosted_ = posted_ || stereo_;

    if (!apllHeld_) {
        periph_rtc_apll_acquire();
        apllHeld_ = true;
    }
    if (!applyFrequency()) {
        periph_rtc_apll_release();
        apllHeld_ = false;
        return false;
    }
    SetCarrier(true);

    if (stereo_) {
        // Dedicated loop owning cpu_: no interrupt overhead, exact 133.3 kHz from the cycle counter.
        // One priority level below the top so the IPC task can still stall this core for flash
        // writes (NVS, OTA): the carrier then freezes for a few ms instead of the write blocking.
        ticks_ = STEREO_TICKS;
        lateCyc_ = (uint32_t)((uint64_t)getCpuFrequencyMhz() * 1000000ULL * ticks_ / TIMER_HZ);
        modRun_ = true;
        modDone_ = false;
        isrPrio_ = -1;
        statsSinceUs_ = esp_timer_get_time();
        dspDirty_ = true;
        running_ = true;
        if (xTaskCreatePinnedToCore(modTaskEntry, "fm_mod", 3072, this, configMAX_PRIORITIES - 2, &modTask_, cpu_) != pdPASS) {
            ESP_LOGE(TAG, "cannot start the modulator task");
            modRun_ = false;
            modDone_ = true;
            running_ = false;
            end();
            return false;
        }
        return true;
    }

    // The interrupt is allocated on the core that installs the timer: use a short-lived
    // task pinned to cpu_.
    const int prios[1] = { 0 };
    for (int prio : prios) {
        FmTimerCtx ctx = { this, xSemaphoreCreateBinary(), false, prio };
        if (!ctx.done) return false;
        if (xTaskCreatePinnedToCore(timerSetupTask, "fm_tmr", 3072, &ctx, configMAX_PRIORITIES - 1, nullptr, cpu_) != pdPASS) {
            vSemaphoreDelete(ctx.done);
            return false;
        }
        xSemaphoreTake(ctx.done, portMAX_DELAY);
        vSemaphoreDelete(ctx.done);
        if (ctx.ok) {
            isrPrio_ = prio;
            break;
        }
        if (prio == 0) {
            ESP_LOGE(TAG, "cannot start the timer");
            end();
            return false;
        }
        ESP_LOGW(TAG, "level-%d interrupt unavailable, falling back to default", prio);
    }

    statsSinceUs_ = esp_timer_get_time();
    dspDirty_ = true;
    running_ = true;
    return true;
}

void AudioOutputFM::timerSetupTask(void *arg)
{
    FmTimerCtx *c = static_cast<FmTimerCtx *>(arg);
    c->ok = c->self->startTimer(c->prio);
    xSemaphoreGive(c->done);
    vTaskDelete(nullptr);
}

bool AudioOutputFM::startTimer(int prio)
{
    gptimer_config_t cfg = {};
    cfg.clk_src = GPTIMER_CLK_SRC_DEFAULT;
    cfg.direction = GPTIMER_COUNT_UP;
    cfg.resolution_hz = TIMER_HZ;
    cfg.intr_priority = prio;
    if (gptimer_new_timer(&cfg, &timer_) != ESP_OK) {
        timer_ = nullptr;
        return false;
    }
    gptimer_event_callbacks_t cbs = {};
    cbs.on_alarm = fm_on_alarm;
    if (gptimer_register_event_callbacks(timer_, &cbs, this) != ESP_OK ||
        gptimer_enable(timer_) != ESP_OK) {
        gptimer_del_timer(timer_);
        timer_ = nullptr;
        return false;
    }
    programAlarm();
    return gptimer_start(timer_) == ESP_OK;
}

void AudioOutputFM::programAlarm()
{
    // Reset the counter: an alarm below the current count would never fire.
    if (stereo_) {
        ticks_ = STEREO_TICKS;
    } else {
        int hz = hertz ? hertz : 44100;
        ticks_ = (TIMER_HZ + hz / 2) / hz;
    }
    lateCyc_ = (uint32_t)((uint64_t)getCpuFrequencyMhz() * 1000000ULL * ticks_ / TIMER_HZ);
    gptimer_set_raw_count(timer_, 0);
    gptimer_alarm_config_t a = {};
    a.alarm_count = ticks_;
    a.reload_count = 0;
    a.flags.auto_reload_on_alarm = true;
    gptimer_set_alarm_action(timer_, &a);
}

void AudioOutputFM::stopTimer()
{
    if (!timer_) return;
    gptimer_stop(timer_);
    gptimer_disable(timer_);
    gptimer_del_timer(timer_);
    timer_ = nullptr;
    ri2c_wait_idle();
}

void AudioOutputFM::modTaskEntry(void *arg)
{
    static_cast<AudioOutputFM *>(arg)->_modLoop();
    vTaskDelete(nullptr);
}

// Stereo modulator: paced by the CPU cycle counter (240 MHz / 133.33 kHz = 1800 cycles).
// Reuses the ISR body for one sample. If it falls behind (an interrupt on this core, a
// long wait for the regi2c lock), it drops the overdue samples instead of slowing down.
void IRAM_ATTR AudioOutputFM::_modLoop()
{
    const uint32_t period = lateCyc_;
    uint32_t next = ccount();
    while (modRun_) {
        if (modHold_) {                          // frequency change in progress
            modHeld_ = true;
            while (modHold_ && modRun_) vTaskDelay(1);
            modHeld_ = false;
            next = ccount();
            continue;
        }
        while ((int32_t)(ccount() - next) < 0) { }
        _isr();
        next += period;
        int32_t lag = (int32_t)(ccount() - next);
        if (lag >= (int32_t)period) {
            uint32_t skip = (uint32_t)lag / period;
            uint32_t r = rd_.load(std::memory_order_relaxed);
            uint32_t avail = wr_.load(std::memory_order_acquire) - r;
            rd_.store(r + (skip < avail ? skip : avail), std::memory_order_release);
            next += skip * period;
            isrLate_ = isrLate_ + skip;
        }
    }
    ri2c_wait_idle();
    modDone_ = true;
}

void AudioOutputFM::end()
{
    if (modTask_) {
        modRun_ = false;
        modHold_ = false;
        while (!modDone_) vTaskDelay(1);
        modTask_ = nullptr;
    }
    stopTimer();
    if (apllHeld_) {
        SetCarrier(false);
        periph_rtc_apll_release();
        apllHeld_ = false;
    }
    running_ = false;
}

bool AudioOutputFM::stop()
{
    // Behave like a transmitter: the carrier stays, the buffer drains, then silence.
    return true;
}

// ---------------------------------------------------------------------------------------------
// Frequency / RF
// ---------------------------------------------------------------------------------------------

bool AudioOutputFM::IsFrequencyUsable(float mhz)
{
    if (mhz < MIN_MHZ || mhz > MAX_MHZ) return false;
    // Keep far enough from a DSDM2 boundary that full deviation never crosses it:
    // DSDM2 and DSDM1/0 are written separately, so a crossing produces a ~10 MHz glitch.
    float xtal = (float)rtc_clk_xtal_freq_get();
    float lsb = xtal * 1e6f / 4.0f / 65536.0f;
    int32_t sdm = lroundf(65536.0f * (mhz * 4.0f / xtal - 4.0f));
    int32_t low = sdm & 0xFFFF;
    int32_t margin = (int32_t)ceilf(1.05f * 75000.0f / lsb) + 2;
    return low >= margin && low <= 0xFFFF - margin;
}

bool AudioOutputFM::SetFrequency(float mhz)
{
    if (!IsFrequencyUsable(mhz)) {
        ESP_LOGW(TAG, "%.3f MHz rejected (outside %.0f-%.0f MHz or too close to a multiple of %.0f MHz)",
                 mhz, MIN_MHZ, MAX_MHZ, (float)rtc_clk_xtal_freq_get() / 4.0f);
        return false;
    }
    reqMhz_ = mhz;
    if (!apllHeld_) return true;               // applied at begin()
    return applyFrequency();
}

bool AudioOutputFM::applyFrequency()
{
    bool hadTimer = (timer_ != nullptr);
    bool hadLoop = (modTask_ != nullptr && modRun_);
    if (hadTimer) {
        gptimer_stop(timer_);
        vTaskDelay(2);                         // let a running ISR finish
    }
    if (hadLoop) {
        modHold_ = true;
        while (!modHeld_ && modRun_) vTaskDelay(1);
    }
    ri2c_wait_idle();                          // a posted write must not overlap the IDF's own

    uint32_t real = 0;
    if (periph_rtc_apll_freq_set((uint32_t)(reqMhz_ * 1e6f), &real) != ESP_OK) {
        ESP_LOGE(TAG, "periph_rtc_apll_freq_set failed (APLL in use elsewhere?)");
        if (hadTimer) gptimer_start(timer_);
        modHold_ = false;
        return false;
    }

    // Overwrite the IDF coefficients with our own exact value (o_div = 0 in this range)
    float xtal = (float)rtc_clk_xtal_freq_get();
    lsbHz_ = xtal * 1e6f / 4.0f / 65536.0f;
    int32_t c = lroundf(65536.0f * (reqMhz_ * 4.0f / xtal - 4.0f));
    REGI2C_WRITE_MASK(I2C_APLL, I2C_APLL_DSDM2, (c >> 16) & 0x3F);
    REGI2C_WRITE(I2C_APLL, I2C_APLL_DSDM1, (c >> 8) & 0xFF);
    REGI2C_WRITE(I2C_APLL, I2C_APLL_DSDM0, c & 0xFF);
    center_ = c;
    last_ = c;
    dspDirty_ = true;                          // qScale depends on the step size

    if (hadTimer) gptimer_start(timer_);
    if (hadLoop) modHold_ = false;
    return true;
}

float AudioOutputFM::GetFrequency() const
{
    if (!apllHeld_) return reqMhz_;
    float xtal = (float)rtc_clk_xtal_freq_get();
    return xtal * (4.0f + center_ / 65536.0f) / 4.0f;
}

void AudioOutputFM::SetCarrier(bool on)
{
    if (on && apllHeld_) {
        PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO0_U, FUNC_GPIO0_CLK_OUT1);
        REG_SET_FIELD(PIN_CTRL, CLK_OUT1, 6);  // 6 = APLL clock
        gpio_set_drive_capability(GPIO_NUM_0, (gpio_drive_cap_t)drive_);
    } else {
        PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO0_U, PIN_FUNC_GPIO);
        gpio_set_direction(GPIO_NUM_0, GPIO_MODE_INPUT);
    }
}

void AudioOutputFM::SetDriveStrength(int level)
{
    drive_ = level < 0 ? 0 : (level > 3 ? 3 : level);
    if (apllHeld_) gpio_set_drive_capability(GPIO_NUM_0, (gpio_drive_cap_t)drive_);
}

// ---------------------------------------------------------------------------------------------
// Audio settings
// ---------------------------------------------------------------------------------------------

bool AudioOutputFM::SetRate(int hz)
{
    if (hz < 8000 || hz > 65535) return false;   // hertz is a uint16_t in AudioOutput
    hertz = hz;
    if (timer_ && !stereo_) {                    // stereo: fixed multiplex rate, the resampler adapts
        gptimer_stop(timer_);
        programAlarm();
        gptimer_start(timer_);
    }
    dspDirty_ = true;
    return true;
}

bool AudioOutputFM::SetChannels(int ch)
{
    channels = (ch == 1) ? 1 : 2;
    return true;
}

bool AudioOutputFM::SetGain(float f)
{
    AudioOutput::SetGain(f);
    gain_ = (f < 0.0f) ? 0.0f : (f > 4.0f ? 4.0f : f);
    return true;
}

void AudioOutputFM::SetDeviation(float hz)
{
    devHz_ = (hz < 1000.0f) ? 1000.0f : (hz > 75000.0f ? 75000.0f : hz);
    dspDirty_ = true;
}

void AudioOutputFM::SetPreemphasis(float us)
{
    tauUs_ = (us < 0.0f) ? 0.0f : us;
    dspDirty_ = true;
}

void AudioOutputFM::SetAGC(bool enable, float targetRms)
{
    agcTarget_ = targetRms;
    agcOn_ = enable;
}

void AudioOutputFM::Biquad::lowpass(float fs, float fc, float q)
{
    // RBJ cookbook low-pass
    float w = 2.0f * (float)M_PI * fc / fs, c = cosf(w), al = sinf(w) / (2.0f * q);
    float a0 = 1.0f + al;
    b0 = (1.0f - c) / 2.0f / a0;
    b1 = (1.0f - c) / a0;
    b2 = b0;
    a1 = -2.0f * c / a0;
    a2 = (1.0f - al) / a0;
}

static float bessel_i0(float x)
{
    float sum = 1.0f, term = 1.0f, q = x * x / 4.0f;
    for (int k = 1; k < 25; k++) {
        term *= q / ((float)k * k);
        sum += term;
    }
    return sum;
}

// Polyphase low-pass prototype (Kaiser-windowed sinc) sampled at fsIn * RS_PHASES.
// taps_[p * T + j] = proto((T - 1 - j) * P + p): phase p, taps reversed so that the
// dot product runs over the history from oldest to newest sample.
// The prototype is symmetric, so phase p is phase P - p read backwards: only phases
// 0..P/2 are stored. Taps are Q15 (peak tap = 2 fc / fs < 0.85): 8.3 KB instead of 33 KB in float.
void AudioOutputFM::designResampler(float fsIn)
{
    const int T = RS_TAPS, P = RS_PHASES, N = T * P;
    // float only: double is software-emulated on the ESP32 (design takes a few tens of ms)
    const float fc = fminf(17000.0f, 0.42f * fsIn);      // -6 dB: flat to ~14.5 kHz, -36 dB at 19 kHz, < -65 dB above 23 kHz
    const float wc = fc / (fsIn * P);                    // cycles per prototype sample
    const float beta = 5.65f, i0b = bessel_i0(beta);
    const float half = N / 2.0f;
    auto proto = [&](int i) {
        float t = i - half;
        float s = (t == 0.0f) ? 2.0f * wc : sinf(2.0f * (float)M_PI * wc * t) / ((float)M_PI * t);
        float r = t / half;
        float w = (fabsf(r) <= 1.0f) ? bessel_i0(beta * sqrtf(1.0f - r * r)) / i0b : 0.0f;
        return s * w;
    };
    // Pass 1: DC gain, pass 2: normalized Q15 taps (unity DC gain for every phase)
    double sum = 0.0;
    for (int i = 0; i < N; i++) sum += proto(i);
    const float g = (float)(P / sum) * 32768.0f;
    for (int p = 0; p <= P / 2; p++) {
        for (int j = 0; j < T; j++) {
            int32_t q = lroundf(proto((T - 1 - j) * P + p) * g);
            taps_[p * T + j] = (int16_t)(q > 32767 ? 32767 : (q < -32768 ? -32768 : q));
        }
    }
    designedFor_ = (int)fsIn;
}

// Pre-emphasis as a first-order IIR, H(z) = g (1 + b z^-1) / (1 + a z^-1), fitted to the analog
// curve |H(f)|^2 = 1 + (2 pi f tau)^2 from 0 to 15 kHz. The plain first difference used before
// fell 1.7 dB short at 15 kHz (44.1 kHz input); this stays within ~0.1 dB at 44.1/48 kHz.
// Method: with c = cos(w), |H|^2 = K (1 + b^2 + 2 b c) / (1 + a^2 + 2 a c), so fitting
// T (1 + A c) = N0 + N1 c is linear in (A, N0, N1): weighted least squares (weight 1/T, i.e.
// relative error), then a and b come back from A = 2a / (1 + a^2) and N1 / N0 = 2b / (1 + b^2),
// and g sets unity gain at DC. Runs once per rate or setting change (float: double is software-emulated).
static void designPreemphasis(float fs, float tau, float &g, float &a, float &b)
{
    g = 1.0f; a = 0.0f; b = 0.0f;
    if (tau <= 0.0f) return;
    const int n = 32;
    const float fmax = fminf(15000.0f, 0.45f * fs);
    float m[3][3] = {{0}}, v[3] = {0};                     // normal equations M^T M x = M^T y
    for (int i = 1; i <= n; i++) {
        float f = fmax * i / n, c = cosf(2.0f * (float)M_PI * f / fs);
        float wt = 2.0f * (float)M_PI * f * tau, T = 1.0f + wt * wt;
        float row[3] = { -c, 1.0f / T, c / T };          // (T (1 + A c) = N0 + N1 c) / T
        for (int r = 0; r < 3; r++) {
            v[r] += row[r];                              // y = 1
            for (int k = 0; k < 3; k++) m[r][k] += row[r] * row[k];
        }
    }
    auto det3 = [](float q[3][3]) {
        return q[0][0] * (q[1][1] * q[2][2] - q[1][2] * q[2][1])
             - q[0][1] * (q[1][0] * q[2][2] - q[1][2] * q[2][0])
             + q[0][2] * (q[1][0] * q[2][1] - q[1][1] * q[2][0]);
    };
    float d = det3(m), x[3];
    if (fabsf(d) < 1e-30f) { a = 0.0f; b = -expf(-1.0f / (tau * fs)); g = 1.0f / (1.0f + b); return; }
    for (int k = 0; k < 3; k++) {                        // Cramer's rule
        float q[3][3];
        memcpy(q, m, sizeof(q));
        for (int r = 0; r < 3; r++) q[r][k] = v[r];
        x[k] = det3(q) / d;
    }
    auto root = [](float r) {                            // x with 2x / (1 + x^2) = r, |x| < 1
        if (r > 0.999999f) r = 0.999999f; else if (r < -0.999999f) r = -0.999999f;
        return fabsf(r) < 1e-12f ? 0.0f : (1.0f - sqrtf(1.0f - r * r)) / r;
    };
    float aa = root(x[0]), bb = root(x[2] / x[1]);
    a = aa;
    b = bb;
    g = (1.0f + aa) / (1.0f + bb);                       // unity gain at DC
}

void AudioOutputFM::setupDsp()
{
    float fs = (float)(hertz ? hertz : 44100);
    float fc = fminf(15000.0f, 0.45f * fs);
    lp1_.lowpass(fs, fc, 0.5412f);             // 4th-order Butterworth (mono path)
    lp2_.lowpass(fs, fc, 1.3066f);
    designPreemphasis(fs, tauUs_ * 1e-6f, peG_, peA_, peB_);
    rel_  = expf(-1.0f / (0.150f * fs));       // limiter release 150 ms
    agcK_ = 1.0f - expf(-1.0f / (3.0f * fs));  // power averaged over 3 s
    agcS_ = 1.0f - expf(-1.0f / (0.5f * fs));  // gain smoothing 0.5 s
    qScale_ = devHz_ / lsbHz_;

    if (stereo_ && taps_) {
        const double fsOut = (double)TIMER_HZ / STEREO_TICKS;
        if (designedFor_ != (int)fs) designResampler(fs);
        step_   = (float)(fs / fsOut);
        phInc_  = (uint32_t)llround(19000.0 / fsOut * 4294967296.0);
        phOff_  = (uint32_t)(int32_t)llround(phaseDeg_ / 360.0 * 4294967296.0);
        aRds_   = rdsOn_ ? rdsLevel_ : 0.0f;
        aAudio_ = 1.0f - pilot_ - aRds_ - 0.01f;   // audio + pilot + RDS stay within full deviation
    }
    dspDirty_ = false;
}

// ---------------------------------------------------------------------------------------------
// Audio path (generator context, FPU allowed)
// ---------------------------------------------------------------------------------------------

bool AudioOutputFM::ConsumeSample(int16_t sample[2])
{
    if (!running_) return false;
    return stereo_ ? consumeStereo(sample) : consumeMono(sample);
}

// Slow AGC on the mid signal, frozen during silence. Returns the gain to apply.
void AudioOutputFM::levelControl(float mid, float &g)
{
    float p = mid * mid;
    if (p > 1e-6f || pw_ > 1e-5f) pw_ += (p - pw_) * agcK_;
    if (agcOn_) {
        if (pw_ > 1e-5f) {
            float gt = agcTarget_ / sqrtf(pw_);
            gt = gt > 6.0f ? 6.0f : (gt < 0.5f ? 0.5f : gt);
            agc_ += (gt - agc_) * agcS_;
        }
    } else {
        agc_ = 1.0f;
    }
    g = agc_ * gain_;
}

bool AudioOutputFM::consumeMono(int16_t sample[2])
{
    uint32_t w = wr_.load(std::memory_order_relaxed);
    if (w - rd_.load(std::memory_order_acquire) >= size_) return false;   // full: the generator retries later
    if (dspDirty_) setupDsp();

    float x = (channels == 1) ? (float)sample[LEFTCHANNEL]
                              : 0.5f * ((float)sample[LEFTCHANNEL] + (float)sample[RIGHTCHANNEL]);
    x *= (1.0f / 32768.0f);

    // 15 kHz low-pass: nothing above, keeps the 19 kHz pilot region clean
    x = lp2_.run(lp1_.run(x));

    float g;
    levelControl(x, g);
    x *= g;

    // Pre-emphasis (see designPreemphasis)
    float y = peG_ * (x + peB_ * x1_) - peA_ * y1_;
    x1_ = x;
    y1_ = y;

    // Peak limiter (instant attack) + safety hard clip
    float a = fabsf(y);
    if (a > peak_) peak_ = a;
    env_ = (a > env_) ? a : env_ * rel_;
    if (env_ > 1.0f) y /= env_;
    if (y > 1.0f) { y = 1.0f; clips_++; } else if (y < -1.0f) { y = -1.0f; clips_++; }

    // Quantize to the SDM step; first-order noise shaping pushes the error up in
    // frequency, where the receiver's de-emphasis attenuates it
    float d = y * qScale_ + err_;
    int32_t q = lroundf(d);
    err_ = d - (float)q;
    if (q > 32767) q = 32767; else if (q < -32767) q = -32767;

    ringPut(ring_, w & mask_, (int16_t)q);
    wr_.store(w + 1, std::memory_order_release);
    return true;
}

bool AudioOutputFM::consumeStereo(int16_t sample[2])
{
    const int T = RS_TAPS, P = RS_PHASES;
    uint32_t w = wr_.load(std::memory_order_relaxed);
    if (size_ - (w - rd_.load(std::memory_order_acquire)) < 16) return false;   // room for one input's outputs
    if (dspDirty_) setupDsp();

    float l = (float)sample[LEFTCHANNEL] * (1.0f / 32768.0f);
    float r = (channels == 1) ? l : (float)sample[RIGHTCHANNEL] * (1.0f / 32768.0f);

    float g;
    levelControl(0.5f * (l + r), g);
    l *= g;
    r *= g;

    // Per-channel pre-emphasis (before matrixing, as broadcast processors do)
    float yl = peG_ * (l + peB_ * x1_) - peA_ * y1_;
    float yr = peG_ * (r + peB_ * x1r_) - peA_ * y1r_;
    x1_ = l;
    x1r_ = r;
    y1_ = yl;
    y1r_ = yr;

    // Linked peak limiter: |L|, |R| <= 1 keeps (L+R)/2 + (L-R)/2*sub within +/-1
    float a = fmaxf(fabsf(yl), fabsf(yr));
    if (a > peak_) peak_ = a;
    env_ = (a > env_) ? a : env_ * rel_;
    if (env_ > 1.0f) {
        float k = 1.0f / env_;
        yl *= k;
        yr *= k;
    }

    // History (duplicated so the window is always contiguous: oldest .. newest)
    histL_[hw_] = histL_[hw_ + T] = yl;
    histR_[hw_] = histR_[hw_ + T] = yr;
    const float *wl = &histL_[hw_ + 1];
    const float *wrh = &histR_[hw_ + 1];
    if (++hw_ == T) hw_ = 0;

    // Emit every multiplex sample falling between the previous input and this one.
    // frac in [0, 1): position of the output between the previous input and the newest one;
    // the matching polyphase branch is p = frac * P (checked against a numerical simulation:
    // images of the source rate end up below -79 dB, crosstalk at -110 dB before the APLL).
    const float kd = kDiff_, ap = pilot_, aa = aAudio_, qs = qScale_, st = step_, ar = aRds_;
    while (frac_ < 1.0f) {
        int p = (int)(frac_ * P + 0.5f);                // nearest phase, 0..P
        float L = 0.0f, R = 0.0f;
        if (p <= P / 2) {
            const int16_t *c = &taps_[p * T];
            for (int j = 0; j < T; j++) {
                float k = (float)c[j];
                L += wl[j] * k;
                R += wrh[j] * k;
            }
        } else {                                         // mirrored phase: taps of P - p, reversed
            const int16_t *c = &taps_[(P - p) * T + (T - 1)];
            for (int j = 0; j < T; j++) {
                float k = (float)c[-j];
                L += wl[j] * k;
                R += wrh[j] * k;
            }
        }
        L *= (1.0f / 32768.0f);                          // Q15 taps
        R *= (1.0f / 32768.0f);
        float mid = 0.5f * (L + R), side = 0.5f * (L - R);
        float pil = s_sin[ph_ >> 22];
        float sub = s_sin[((ph_ << 1) + phOff_) >> 22];  // 38 kHz locked to 2x pilot, + trim

        float mpx = aa * (mid + kd * side * sub) + ap * pil;

        if (ar > 0.0f) {
            // RDS: 57 kHz = 3x pilot phase; one bit = 16 pilot cycles (19000 / 16 = 1187.5 bit/s).
            // Biphase symbol: one sine period per bit, sign from the differential encoder.
            uint32_t u = ((uint32_t)rdsCyc_ << 28) | (ph_ >> 4);      // position within the bit
            mpx += ar * rdsSign_ * s_sin[u >> 22] * s_sin[(ph_ * 3u) >> 22];
        }
        uint32_t prev = ph_;
        ph_ += phInc_;
        if (ph_ < prev && ++rdsCyc_ == 16) {             // one pilot cycle done; 16 = next RDS bit
            rdsCyc_ = 0;
            if (ar > 0.0f) {
                if (rdsBit_ >= 104) rdsNextGroup();
                rdsDiff_ ^= rdsBits_[rdsBit_++];
                rdsSign_ = rdsDiff_ ? 1.0f : -1.0f;
            }
        }
        if (mpx > 1.0f) { mpx = 1.0f; clips_++; } else if (mpx < -1.0f) { mpx = -1.0f; clips_++; }

        // Plain rounding: noise shaping would push the error into the 23-53 kHz L-R band
        int32_t q = lroundf(mpx * qs);
        ringPut(ring_, w & mask_, (int16_t)q);
        w++;
        frac_ += st;
    }
    frac_ -= 1.0f;
    wr_.store(w, std::memory_order_release);
    return true;
}

// ---------------------------------------------------------------------------------------------
// RDS encoder: groups 0A (PS) and 2A (RadioText), IEC 62106
// ---------------------------------------------------------------------------------------------

void AudioOutputFM::SetRDS(bool enable)
{
    rdsOn_ = enable;
    dspDirty_ = true;
}

void AudioOutputFM::SetRdsPI(uint16_t pi)       { rdsPI_ = pi; }
void AudioOutputFM::SetRdsPTY(uint8_t pty)      { rdsPTY_ = pty & 0x1F; }
void AudioOutputFM::SetRdsAscii(bool on)        { rdsCharset_ = on ? 1 : 0; }
void AudioOutputFM::SetRdsABToggle(bool on)    { rdsAbToggle_ = on; }
void AudioOutputFM::SetRdsCharset(int cs)       { rdsCharset_ = (cs < 0 || cs > 2) ? 0 : cs; }

void AudioOutputFM::SetRdsLevel(float fraction)
{
    rdsLevel_ = fraction < 0.0f ? 0.0f : (fraction > 0.10f ? 0.10f : fraction);
    dspDirty_ = true;
}

void AudioOutputFM::SetRdsPS(const char *ps)
{
    char tmp[8];
    int n = rdsConvert(ps ? ps : "", tmp, 8, rdsCharset_);
    for (int i = 0; i < 8; i++) rdsPS_[i] = i < n ? tmp[i] : ' ';
}

void AudioOutputFM::SetRdsText(const char *text)
{
    // Picked up by the encoder at the next group boundary
    strncpy(rtPend_, text ? text : "", sizeof(rtPend_) - 1);
    rtPend_[sizeof(rtPend_) - 1] = 0;
    rtPending_.store(true, std::memory_order_release);
}

void AudioOutputFM::rdsLoadText()
{
    char tmp[64];
    int n = rdsConvert(rtPend_, tmp, 64, rdsCharset_);
    rtPending_.store(false, std::memory_order_release);
    // Always send all 64 characters, space-padded, with no 0x0D terminator: several receivers
    // (e.g. Yamaha RX-V673) ignore the terminator, show it as '_', and keep displaying the
    // segments of the previous text that are no longer transmitted.
    char rt[64];
    memset(rt, ' ', sizeof(rt));
    memcpy(rt, tmp, n);
    // Many servers (Shoutcast, Centova...) resend the same title in every metadata block:
    // an unchanged text must not restart the cycle nor toggle A/B.
    if (rdsRTSegs_ == 16 && memcmp(rt, rdsRT_, sizeof(rt)) == 0) return;
    memcpy(rdsRT_, rt, sizeof(rt));
    rdsRTSegs_ = 16;
    if (rdsAbToggle_) rdsAB_ = !rdsAB_;        // per the standard: receivers should clear their display
    rdsRtSeg_ = 0;
}

static bool is_valid_utf8(const uint8_t *p)
{
    while (*p) {
        int n = (*p < 0x80) ? 0 : ((*p & 0xE0) == 0xC0) ? 1 : ((*p & 0xF0) == 0xE0) ? 2 : ((*p & 0xF8) == 0xF0) ? 3 : -1;
        if (n < 0) return false;
        p++;
        while (n--) {
            if ((*p & 0xC0) != 0x80) return false;
            p++;
        }
    }
    return true;
}

// Text -> RDS characters. Input is UTF-8, or Latin-1 when it is not valid UTF-8 (some
// Icecast servers send ICY titles in ISO-8859-1). Output charset: 0 = RDS table
// (IEC 62106 annex E, common Latin accents), 1 = plain ASCII, 2 = raw Latin-1 bytes.
int AudioOutputFM::rdsConvert(const char *in, char *out, int max, int charset)
{
    const bool ascii = (charset == 1);
    // Latin-1 U+00C0..U+00FF: RDS code, and ASCII fallback letter
    static const uint8_t ebu[64] = {
        0xC1, 0xC0, 0xD0, 'A',  0xD1, 'A',  'A',  0x8B, 0xC3, 0xC2, 0xD2, 0xD3, 0xC5, 0xC4, 0xD4, 0xD5,   // C0-CF
        'D',  0x8A, 0xC7, 0xC6, 0xD6, 'O',  0xD7, 'x',  'O',  0xC9, 0xC8, 0xD8, 0xD9, 'Y',  'T',  0x8D,   // D0-DF
        0x81, 0x80, 0x90, 'a',  0x91, 'a',  'a',  0x9B, 0x83, 0x82, 0x92, 0x93, 0x85, 0x84, 0x94, 0x95,   // E0-EF
        'd',  0x9A, 0x87, 0x86, 0x96, 'o',  0x97, '/',  'o',  0x89, 0x88, 0x98, 0x99, 'y',  't',  'y'     // F0-FF
    };
    static const char base[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    int n = 0;
    const uint8_t *p = (const uint8_t *)in;
    const bool utf8 = is_valid_utf8(p);
    while (*p && n < max) {
        uint32_t cp;
        if (!utf8)                                         { cp = p[0]; p += 1; }          // Latin-1 input
        else if (p[0] < 0x80)                              { cp = p[0]; p += 1; }
        else if ((p[0] & 0xE0) == 0xC0 && p[1])            { cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
        else if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2])    { cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
        else { p++; while ((*p & 0xC0) == 0x80) p++; cp = '?'; }
        char ch;
        if (cp >= 0x20 && cp < 0x7F)            ch = (char)cp;
        else if (cp >= 0xA0 && cp <= 0xFF && charset == 2) ch = (char)cp;
        else if (cp >= 0xC0 && cp <= 0xFF)      ch = ascii ? base[cp - 0xC0] : (char)ebu[cp - 0xC0];
        else if (cp == 0x2018 || cp == 0x2019)  ch = '\'';
        else if (cp == 0x201C || cp == 0x201D)  ch = '"';
        else if (cp == 0x2013 || cp == 0x2014)  ch = '-';
        else if (cp == 0x2026)                  ch = '.';
        else if (cp < 0x20)                     continue;
        else                                    ch = '?';
        out[n++] = ch;
    }
    return n;
}

static uint16_t rds_crc(uint16_t data)
{
    // g(x) = x^10 + x^8 + x^7 + x^5 + x^4 + x^3 + 1
    uint32_t reg = (uint32_t)data << 10;
    for (int i = 25; i >= 10; i--)
        if (reg & (1u << i)) reg ^= (0x5B9u << (i - 10));
    return reg & 0x3FF;
}

void AudioOutputFM::rdsNextGroup()
{
    static const uint16_t offset[4] = { 0x0FC, 0x198, 0x168, 0x1B4 };   // A, B, C, D
    if (rtPending_.load(std::memory_order_acquire)) rdsLoadText();

    uint16_t blk[4];
    blk[0] = rdsPI_;
    const uint16_t common = (uint16_t)((rdsPTY_ & 0x1F) << 5);         // TP = 0, B0 = 0 (version A)
    if (rdsRTSegs_ > 0 && (rdsGroups_ & 1)) {
        // 2A: RadioText, 4 characters per group
        int seg = rdsRtSeg_;
        blk[1] = (uint16_t)((2 << 12) | common | (rdsAB_ ? 0x10 : 0) | seg);
        blk[2] = (uint16_t)(((uint8_t)rdsRT_[seg * 4] << 8) | (uint8_t)rdsRT_[seg * 4 + 1]);
        blk[3] = (uint16_t)(((uint8_t)rdsRT_[seg * 4 + 2] << 8) | (uint8_t)rdsRT_[seg * 4 + 3]);
        if (++rdsRtSeg_ >= rdsRTSegs_) rdsRtSeg_ = 0;
    } else {
        // 0A: PS, 2 characters per group; DI bit d0 (stereo) is sent in segment 3; M/S = music
        int seg = rdsPsSeg_;
        uint16_t di = (seg == 3 && stereo_) ? 1 : 0;
        blk[1] = (uint16_t)((0 << 12) | common | (1 << 3) | (di << 2) | seg);
        blk[2] = 0xE0CD;                                                // no alternative frequencies
        blk[3] = (uint16_t)(((uint8_t)rdsPS_[seg * 2] << 8) | (uint8_t)rdsPS_[seg * 2 + 1]);
        rdsPsSeg_ = (rdsPsSeg_ + 1) & 3;
    }
    rdsGroups_++;

    int p = 0;
    for (int b = 0; b < 4; b++) {
        uint16_t cw = rds_crc(blk[b]) ^ offset[b];
        for (int i = 15; i >= 0; i--) rdsBits_[p++] = (blk[b] >> i) & 1;
        for (int i = 9; i >= 0; i--)  rdsBits_[p++] = (cw >> i) & 1;
    }
    rdsBit_ = 0;
}

AudioOutputFM::Stats AudioOutputFM::GetStats(bool reset)
{
    Stats s;
    int64_t now = esp_timer_get_time();
    uint32_t n = isrCount_;
    float elapsed = (now - statsSinceUs_) * 1e-6f;
    s.rateHz       = ticks_ ? (float)TIMER_HZ / ticks_ : 0.0f;
    s.measuredHz   = elapsed > 0.0f ? n / elapsed : 0.0f;
    s.fill         = wr_.load() - rd_.load();
    s.size         = size_;
    s.underruns    = underruns_;
    s.isrMaxUs     = isrMaxCyc_ / (float)getCpuFrequencyMhz();
    s.isrLate      = isrLate_;
    s.twoWritesPct = n ? 100.0f * twoWrites_ / n : 0.0f;
    s.agcGain      = agc_;
    s.peak         = peak_;
    s.clips        = clips_;
    s.stereo       = stereo_;
    s.isrPriority  = isrPrio_;
    if (reset) {
        isrMaxCyc_ = 0;
        isrLate_ = 0;
        twoWrites_ = 0;
        isrCount_ = 0;
        peak_ = 0.0f;
        statsSinceUs_ = now;
    }
    return s;
}

#endif // ESP32 classic
