/*
 * Roast audio recorder (see roast_audio.h). Device-only: the desktop preview links a stand-in.
 *
 * Data path: I2S0 RX master, 32 kHz, 16-bit stereo frames -> capture task keeps the slot that carries the
 * microphone -> 8 KB blocks (128 ms) -> writer task -> write() on a file descriptor (a FILE* would split the blocks).
 * The 512-byte header keeps every block write sector aligned, so FatFs writes the blocks straight from our buffers.
 * The header sizes are rewritten
 * and the file is synced every 10 s, so a crash or power loss costs at most the last 10 s.
 */
#include "roast_audio.h"
#include <M5Unified.hpp>
#include <driver/i2s_std.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace roast {

float PeakDbfs(int peak)
{
    return peak <= 0 ? -INFINITY : 20.0f * log10f((float)peak / 32768.0f);
}

namespace rec {
namespace {

const char* TAG = "RoastRec";

constexpr int kBlockBytes         = 8192;  // one SD write: 16 sectors, 128 ms of audio
constexpr int kBlockSamples       = kBlockBytes / 2;
constexpr int kMaxBlocks          = 8;
constexpr int kMinBlocks          = 3;
constexpr size_t kHeapLeft        = 28 * 1024;  // left for BLE, the UI and the log writer after we allocate
constexpr size_t kStacks          = 3072 + 4096;
constexpr int kDmaDesc            = 4;
constexpr int kDmaFrames          = 511;  // stereo 16-bit frames per DMA descriptor: 2044 B, 16 ms
constexpr int kHeaderBytes        = 512;  // RIFF + fmt + JUNK + data: samples start on a sector boundary
constexpr uint32_t kHeaderEveryMs = 10000;
constexpr int kSlotProbeChunks    = 8;  // ~128 ms of audio decides which I2S slot carries the microphone
constexpr int kMaxGaps            = 64;
constexpr uint8_t kCodecAddr      = 0x18;  // ES8311
constexpr uint32_t kI2cHz         = 400000;
constexpr gpio_num_t kBck = GPIO_NUM_41, kWs = GPIO_NUM_43, kDin = GPIO_NUM_46;

enum class Cmd : uint8_t { Pause, Resume, Stop };

struct Item {
    int8_t block;  // < 0: end of stream
    uint16_t samples;
};

struct Rec {
    int fd                      = -1;
    i2s_chan_handle_t rx        = nullptr;
    int16_t* blocks[kMaxBlocks] = {};
    int nBlocks                 = 0;
    QueueHandle_t freeQ = nullptr, fullQ = nullptr, cmdQ = nullptr;
    SemaphoreHandle_t done = nullptr;  // given once by each task when it has finished
    int64_t t0Us           = 0;
    int gainDb             = 0;
    // capture task only
    int curBlock       = -1;
    int curFill        = 0;
    uint32_t queued    = 0;  // samples handed to the writer, padding included
    uint32_t dropped   = 0;  // samples lost to a full ring and not padded yet
    uint32_t dropAt    = 0;
    uint32_t dropTotal = 0;
    int probeChunks    = 0;
    int64_t probeEnergy[2] = {0, 0};
    int peakHist[32]   = {};
    int peakIdx        = 0;
    // shared with the UI task
    std::atomic<bool> active{false}, paused{false}, failed{false};
    std::atomic<uint32_t> written{0}, clipped{0};
    std::atomic<int> peak{0}, recentPeak{0}, slot{-1};
    uint32_t sent = 0;               // commands sent (UI task)
    std::atomic<uint32_t> acked{0};  // commands carried out (capture task)
    portMUX_TYPE gapLock = portMUX_INITIALIZER_UNLOCKED;
    AudioGap gaps[kMaxGaps];
    uint32_t gapCount = 0;
};
Rec R;

int16_t s_stereo[kDmaFrames * 2];  // capture task scratch: kept off its stack
int16_t s_mono[kDmaFrames];

/* ---------------------------------------------------------------- codec */
void codec_write(const uint8_t (*regs)[2], size_t n)
{
    for (size_t i = 0; i < n; i++) {
        for (int tries = 0; tries < 3; tries++) {
            if (M5.In_I2C.writeRegister8(kCodecAddr, regs[i][0], regs[i][1], kI2cHz)) break;
            M5.delay(1);
        }
    }
}

void codec_mic_on(int gainDb)
{
    // M5Unified's Cardputer ADV microphone sequence, plus 16-bit ADC words (0x0A) and the PGA gain (0x14).
    const uint8_t pga       = 0x10 | (uint8_t)(std::max(0, std::min(kMaxGainDb, gainDb)) / 3);
    const uint8_t regs[][2] = {
        {0x00, 0x80},  // CSM power on
        {0x01, 0xBA},  // clock manager: MCLK taken from BCLK
        {0x02, 0x18},  // MULT_PRE = 3: MCLK = 8 x BCLK = 256 fs
        {0x0A, 0x0C},  // ADC serial port: I2S, 16-bit words
        {0x0D, 0x01},  // power up analog circuitry
        {0x0E, 0x02},  // enable analog PGA and the ADC modulator
        {0x14, pga},   // input Mic1p-Mic1n, PGA gain in 3 dB steps (0..30 dB)
        {0x17, 0xBF},  // ADC volume 0 dB
        {0x1C, 0x6A},  // ADC equalizer bypass, cancel DC offset
    };
    codec_write(regs, sizeof(regs) / sizeof(regs[0]));
}

void codec_mic_off()
{
    const uint8_t regs[][2] = {{0x0D, 0xFC}, {0x0E, 0x6A}, {0x00, 0x00}};
    codec_write(regs, sizeof(regs) / sizeof(regs[0]));
}

/* ------------------------------------------------------------------ i2s */
i2s_std_gpio_config_t gpio_cfg()
{
    i2s_std_gpio_config_t g = {};
    g.mclk                  = I2S_GPIO_UNUSED;
    g.bclk                  = kBck;
    g.ws                    = kWs;
    g.dout                  = I2S_GPIO_UNUSED;
    g.din                   = kDin;
    return g;
}

esp_err_t i2s_create()
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num      = kDmaDesc;
    cc.dma_frame_num     = kDmaFrames;
    esp_err_t e          = i2s_new_channel(&cc, nullptr, &R.rx);
    if (e != ESP_OK) return e;
    i2s_std_config_t sc = {};
    sc.clk_cfg          = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
    sc.slot_cfg         = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    sc.gpio_cfg         = gpio_cfg();
    e                   = i2s_channel_init_std_mode(R.rx, &sc);
    if (e != ESP_OK) {
        i2s_del_channel(R.rx);
        R.rx = nullptr;
    }
    return e;
}

/* --------------------------------------------------------------- header */
void put32(uint8_t* p, uint32_t v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

void put16(uint8_t* p, uint16_t v)
{
    p[0] = v & 0xFF;
    p[1] = v >> 8;
}

bool write_header(uint32_t samples)
{
    static uint8_t h[kHeaderBytes];
    memset(h, 0, sizeof(h));
    uint32_t data = samples * 2;
    memcpy(h, "RIFF", 4);
    put32(h + 4, kHeaderBytes - 8 + data);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    put32(h + 16, 16);
    put16(h + 20, 1);  // PCM
    put16(h + 22, 1);  // mono
    put32(h + 24, kSampleRate);
    put32(h + 28, kSampleRate * 2);
    put16(h + 32, 2);
    put16(h + 34, 16);
    memcpy(h + 36, "JUNK", 4);  // padding so the samples start at byte 512
    put32(h + 40, kHeaderBytes - 8 - 44);
    memcpy(h + kHeaderBytes - 8, "data", 4);
    put32(h + kHeaderBytes - 4, data);
    if (lseek(R.fd, 0, SEEK_SET) != 0) return false;
    bool ok = write(R.fd, h, sizeof(h)) == (ssize_t)sizeof(h);
    return lseek(R.fd, 0, SEEK_END) >= 0 && ok;
}

/* -------------------------------------------------------------- capture */
void add_gap(uint32_t atSamples, uint32_t samples, GapWhy why)
{
    portENTER_CRITICAL(&R.gapLock);
    if (R.gapCount < kMaxGaps)
        R.gaps[R.gapCount] = {(uint32_t)((uint64_t)atSamples * 1000 / kSampleRate),
                              (uint32_t)((uint64_t)samples * 1000 / kSampleRate), why};
    R.gapCount++;
    portEXIT_CRITICAL(&R.gapLock);
}

void emit_block()
{
    Item it{(int8_t)R.curBlock, (uint16_t)R.curFill};
    xQueueSend(R.fullQ, &it, portMAX_DELAY);
    R.curBlock = -1;
    R.curFill  = 0;
}

// Appends samples (zeros when s is null) to the block stream. Returns how many did not fit because every block is
// waiting for the SD card. `wait` blocks for a free block instead (used when stopping).
uint32_t append(const int16_t* s, uint32_t n, TickType_t wait = 0)
{
    uint32_t done = 0;
    while (done < n) {
        if (R.curBlock < 0) {
            int8_t b;
            if (xQueueReceive(R.freeQ, &b, wait) != pdTRUE) break;
            R.curBlock = b;
        }
        uint32_t k   = std::min<uint32_t>(n - done, kBlockSamples - R.curFill);
        int16_t* dst = R.blocks[R.curBlock] + R.curFill;
        if (s)
            memcpy(dst, s + done, k * 2);
        else
            memset(dst, 0, k * 2);
        R.curFill += k;
        R.queued += k;
        done += k;
        if (R.curFill == kBlockSamples) emit_block();
    }
    return n - done;
}

// Every sample position is filled exactly once: when the ring is full, the samples that did not fit are replaced
// by the same number of zeros as soon as a block is free again.
void feed(const int16_t* s, uint32_t n, TickType_t wait = 0)
{
    if (R.dropped) {
        uint32_t left = append(nullptr, R.dropped, wait);
        R.dropped     = left;
        if (left) {
            R.dropped += n;
            R.dropTotal += n;
            return;
        }
        add_gap(R.dropAt, R.dropTotal, GapWhy::Overrun);
        R.dropTotal = 0;
    }
    uint32_t left = append(s, n, wait);
    if (left) {
        R.dropAt    = R.queued;
        R.dropped   = left;
        R.dropTotal = left;
    }
}

// After a pause: silence up to the current time, so the file stays on the roast clock.
void pad_to_now()
{
    uint32_t expected = (uint32_t)((esp_timer_get_time() - R.t0Us) * (int64_t)kSampleRate / 1000000);
    uint32_t have     = R.queued + R.dropped;
    if (expected <= have) return;
    uint32_t n = expected - have;
    if (!R.dropped) add_gap(have, n, GapWhy::Tone);
    feed(nullptr, n);
}

void take_chunk(size_t bytes)
{
    int frames = (int)(bytes / 4);
    if (R.slot.load() < 0) {
        // The ES8311 is mono; find the slot with signal (AC energy, so a DC offset does not count).
        for (int i = 1; i < frames; i++)
            for (int c = 0; c < 2; c++) R.probeEnergy[c] += std::abs(s_stereo[2 * i + c] - s_stereo[2 * i - 2 + c]);
        if (++R.probeChunks >= kSlotProbeChunks) R.slot = R.probeEnergy[0] > R.probeEnergy[1] ? 0 : 1;
    }
    int sl       = R.slot.load() < 0 ? 1 : R.slot.load();
    int chunkMax = 0;
    uint32_t clip = 0;
    for (int i = 0; i < frames; i++) {
        int16_t v  = s_stereo[2 * i + sl];
        s_mono[i]  = v;
        int a      = v < 0 ? -(int)v : v;
        chunkMax   = std::max(chunkMax, a);
        if (a >= 32767) clip++;
    }
    if (clip) R.clipped += clip;
    if (chunkMax > R.peak.load()) R.peak = chunkMax;
    R.peakHist[R.peakIdx] = chunkMax;
    R.peakIdx             = (R.peakIdx + 1) % 32;
    int rp                = 0;
    for (int p : R.peakHist) rp = std::max(rp, p);
    R.recentPeak = rp;
    feed(s_mono, (uint32_t)frames);
}

void capture_task(void*)
{
    bool running = true;
    for (;;) {
        Cmd c;
        // While paused there is nothing to read: wait for the next command.
        while (xQueueReceive(R.cmdQ, &c, running ? 0 : portMAX_DELAY) == pdTRUE) {
            if (c == Cmd::Stop) goto stop;
            if (c == Cmd::Pause && running) {
                i2s_channel_disable(R.rx);
                running = false;
            } else if (c == Cmd::Resume && !running) {
                // The speaker routed G41/G43 to I2S1 while we were paused: take the pins back first.
                i2s_std_gpio_config_t g = gpio_cfg();
                esp_err_t e             = i2s_channel_reconfig_std_gpio(R.rx, &g);
                if (e == ESP_OK) e = i2s_channel_enable(R.rx);
                if (e == ESP_OK)
                    running = true;
                else
                    ESP_LOGE(TAG, "resume failed: %s", esp_err_to_name(e));
                pad_to_now();
            }
            R.paused = !running;
            R.acked++;
            if (!running) continue;
        }
        size_t got = 0;
        i2s_channel_read(R.rx, s_stereo, sizeof(s_stereo), &got, pdMS_TO_TICKS(100));
        if (got) take_chunk(got);  // a timeout can still return part of a buffer: keep every sample
    }
stop:
    if (running) i2s_channel_disable(R.rx);
    if (R.dropped) feed(nullptr, 0, pdMS_TO_TICKS(2000));  // let the writer drain, then fill the hole
    if (R.curBlock >= 0) {
        if (R.curFill > 0) {
            emit_block();
        } else {
            int8_t b = (int8_t)R.curBlock;
            xQueueSend(R.freeQ, &b, 0);
            R.curBlock = -1;
        }
    }
    Item end{-1, 0};
    xQueueSend(R.fullQ, &end, portMAX_DELAY);
    xSemaphoreGive(R.done);
    vTaskDelete(nullptr);
}

/* --------------------------------------------------------------- writer */
void writer_task(void*)
{
    int64_t lastHdr = esp_timer_get_time();
    for (;;) {
        Item it;
        if (xQueueReceive(R.fullQ, &it, portMAX_DELAY) != pdTRUE) continue;
        if (it.block < 0) break;
        if (!R.failed) {
            if (write(R.fd, R.blocks[it.block], it.samples * 2) == (ssize_t)it.samples * 2) {
                R.written += it.samples;
            } else {
                R.failed = true;  // keep draining so capture never blocks; the file keeps what was written
                ESP_LOGE(TAG, "SD write failed after %u samples", (unsigned)R.written.load());
            }
        }
        int8_t b = it.block;
        xQueueSend(R.freeQ, &b, portMAX_DELAY);
        int64_t now = esp_timer_get_time();
        if (!R.failed && now - lastHdr >= (int64_t)kHeaderEveryMs * 1000) {
            if (!write_header(R.written) || fsync(R.fd) != 0) R.failed = true;
            lastHdr = now;
        }
    }
    if (R.fd >= 0) {
        bool ok = write_header(R.written);
        ok      = fsync(R.fd) == 0 && ok;
        ok      = close(R.fd) == 0 && ok;
        if (!ok) R.failed = true;
        R.fd = -1;
    }
    xSemaphoreGive(R.done);
    vTaskDelete(nullptr);
}

void release()
{
    if (R.rx) {
        i2s_del_channel(R.rx);
        R.rx = nullptr;
    }
    for (int i = 0; i < R.nBlocks; i++) {
        heap_caps_free(R.blocks[i]);
        R.blocks[i] = nullptr;
    }
    R.nBlocks = 0;
    if (R.freeQ) vQueueDelete(R.freeQ);
    if (R.fullQ) vQueueDelete(R.fullQ);
    if (R.cmdQ) vQueueDelete(R.cmdQ);
    if (R.done) vSemaphoreDelete(R.done);
    R.freeQ = R.fullQ = R.cmdQ = nullptr;
    R.done                     = nullptr;
    if (R.fd >= 0) {
        close(R.fd);
        R.fd = -1;
    }
}

uint32_t send(Cmd c)
{
    if (!R.active || !R.cmdQ) return 0;
    xQueueSend(R.cmdQ, &c, portMAX_DELAY);
    return ++R.sent;
}

}  // namespace

bool Start(const std::string& path, int gainDb, std::string* err)
{
    auto fail = [&](const char* why) {
        if (err) *err = why;
        ESP_LOGE(TAG, "start failed: %s", why);
        release();
        codec_mic_off();
        M5.Speaker.begin();
        return false;
    };
    if (R.active) {
        if (err) *err = "already recording";
        return false;
    }
    R.curBlock = -1;
    R.curFill  = 0;
    R.queued = R.dropped = R.dropAt = R.dropTotal = 0;
    R.probeChunks                                 = 0;
    R.probeEnergy[0] = R.probeEnergy[1] = 0;
    memset(R.peakHist, 0, sizeof(R.peakHist));
    R.peakIdx = 0;
    R.paused  = false;
    R.failed  = false;
    R.written = 0;
    R.clipped = 0;
    R.peak = R.recentPeak = 0;
    R.slot                = -1;
    R.gapCount            = 0;
    R.sent                = 0;
    R.acked               = 0;
    R.gainDb              = std::max(0, std::min(kMaxGainDb, gainDb)) / 3 * 3;  // the PGA has 3 dB steps

    R.fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (R.fd < 0) return fail("cannot create the WAV file");
    if (!write_header(0)) return fail("SD write failed");

    M5.Speaker.end();  // the speaker task drives the same bit/word clocks on I2S1
    codec_mic_on(R.gainDb);
    if (i2s_create() != ESP_OK) return fail("I2S init failed");
    R.freeQ = xQueueCreate(kMaxBlocks, sizeof(int8_t));
    R.fullQ = xQueueCreate(kMaxBlocks + 1, sizeof(Item));
    R.cmdQ  = xQueueCreate(8, sizeof(Cmd));
    R.done  = xSemaphoreCreateCounting(2, 0);
    if (!R.freeQ || !R.fullQ || !R.cmdQ || !R.done) return fail("out of memory");
    while (R.nBlocks < kMaxBlocks &&
           heap_caps_get_free_size(MALLOC_CAP_8BIT) > kHeapLeft + kStacks + kBlockBytes) {
        auto* p = (int16_t*)heap_caps_malloc(kBlockBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!p) break;
        R.blocks[R.nBlocks] = p;
        int8_t idx          = (int8_t)R.nBlocks++;
        xQueueSend(R.freeQ, &idx, 0);
    }
    if (R.nBlocks < kMinBlocks) return fail("not enough memory to record");

    R.t0Us = esp_timer_get_time();
    if (i2s_channel_enable(R.rx) != ESP_OK) return fail("I2S start failed");
    R.active = true;
    if (xTaskCreatePinnedToCore(writer_task, "roast_wav", 4096, nullptr, 4, nullptr, 1) != pdPASS) {
        R.active = false;
        i2s_channel_disable(R.rx);
        return fail("cannot start the writer");
    }
    if (xTaskCreatePinnedToCore(capture_task, "roast_mic", 3072, nullptr, 6, nullptr, 1) != pdPASS) {
        // the writer is already waiting: end its stream so it closes the file
        Item end{-1, 0};
        xQueueSend(R.fullQ, &end, portMAX_DELAY);
        xSemaphoreTake(R.done, portMAX_DELAY);
        R.active = false;
        i2s_channel_disable(R.rx);
        return fail("cannot start capture");
    }
    ESP_LOGI(TAG, "recording %s: %d blocks (%d ms), gain %d dB, heap left %u", path.c_str(), R.nBlocks,
             R.nBlocks * kBlockSamples * 1000 / (int)kSampleRate, R.gainDb,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    return true;
}

uint32_t Pause()
{
    return send(Cmd::Pause);
}

bool Paused(uint32_t ticket)
{
    return !R.active || (R.acked.load() >= ticket && R.paused.load());
}

void Resume()
{
    if (!R.active) return;
    codec_mic_on(R.gainDb);  // the speaker reprogrammed the codec for playback
    send(Cmd::Resume);
}

bool Stop(AudioStats* out, std::vector<AudioGap>* gaps)
{
    if (!R.active) {
        if (out) *out = Stats();
        return false;
    }
    send(Cmd::Stop);
    xSemaphoreTake(R.done, portMAX_DELAY);  // capture task
    xSemaphoreTake(R.done, portMAX_DELAY);  // writer task: file closed
    AudioStats st = Stats();
    st.active     = false;
    st.paused     = false;
    if (out) *out = st;
    if (gaps) {
        gaps->clear();
        for (uint32_t i = 0; i < std::min<uint32_t>(R.gapCount, kMaxGaps); i++) gaps->push_back(R.gaps[i]);
    }
    R.active = false;
    release();
    M5.Speaker.end();  // a tone may be playing; end() + begin() makes the speaker reprogram the codec
    codec_mic_off();
    M5.Speaker.begin();
    ESP_LOGI(TAG, "stopped: %u samples, %u gaps, peak %.1f dBFS, %u clipped%s", (unsigned)st.samples,
             (unsigned)st.gapCount, PeakDbfs(st.peak), (unsigned)st.clipped, st.failed ? ", WRITE FAILED" : "");
    return !st.failed;
}

AudioStats Stats()
{
    AudioStats s;
    s.active     = R.active;
    s.paused     = R.paused;
    s.failed     = R.failed;
    s.samples    = R.written;
    s.sampleRate = kSampleRate;
    s.peak       = R.peak;
    s.recentPeak = R.recentPeak;
    s.clipped    = R.clipped;
    s.bufferMs   = (uint32_t)R.nBlocks * kBlockSamples * 1000 / kSampleRate;
    s.gapCount   = R.gapCount;
    s.slot       = R.slot;
    s.gainDb     = R.gainDb;
    return s;
}

bool Active()
{
    return R.active;
}

uint32_t StartMs()
{
    return (uint32_t)(R.t0Us / 1000);
}

}  // namespace rec
}  // namespace roast
