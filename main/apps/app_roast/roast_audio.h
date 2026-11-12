/*
 * Roast audio: records the roast from the Cardputer ADV's built-in microphone (ES8311 codec) to <id>.wav next to
 * the roast log, from charge until the log ends. 32 kHz, 16-bit, mono PCM.
 *
 * The microphone and the speaker share the codec and its bit/word clocks (G41/G43), so they cannot run at the same
 * time. While recording, the app only plays the tones that matter (warnings, alarms, the drop window): Pause()
 * stops capture and frees the clocks, the tone plays, and Resume() restarts capture and pads the gap with silence.
 * Overruns (the SD card stalling for longer than the buffer) are padded the same way. The WAV timeline therefore
 * stays locked to the roast clock: second N of the file is second N after the recording started.
 *
 * Threads: Start/Pause/Resume/Stop and every codec register write run on the UI task, which also owns the keyboard
 * on the same I2C bus. A capture task (I2S -> ring of 8 KB blocks) and a writer task (blocks -> SD) run on core 1.
 */
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace roast {

enum class GapWhy : uint8_t { Tone = 0, Overrun = 1 };

struct AudioGap {
    uint32_t atMs;  // position in the recording
    uint32_t ms;    // length of the silence that was inserted
    GapWhy why;
};

struct AudioStats {
    bool active        = false;  // recording, running or paused
    bool paused        = false;  // capture stopped and clocks released: the speaker may play
    bool failed        = false;  // SD write failed; the file holds what was written before
    uint32_t samples   = 0;      // samples in the file so far, padding included
    uint32_t sampleRate = 32000;
    int peak           = 0;      // largest |sample| so far
    int recentPeak     = 0;      // largest |sample| in the last ~0.5 s
    uint32_t clipped   = 0;      // samples at full scale
    uint32_t bufferMs  = 0;      // ring buffer size in milliseconds of audio
    uint32_t gapCount  = 0;
    int slot           = -1;     // I2S slot that carries the microphone: 0 left, 1 right, -1 not decided yet
    int gainDb         = 0;
};

namespace rec {

constexpr uint32_t kSampleRate = 32000;
constexpr int kMaxGainDb       = 30;

/// Opens `path`, configures the codec for the microphone and starts capture. Ends the speaker first.
bool Start(const std::string& path, int gainDb, std::string* err);
/// Asks the capture task to stop and release the clocks. Commands run in order; returns a ticket for Paused().
uint32_t Pause();
/// True once the Pause() that returned `ticket` has been carried out: the speaker may use the codec now.
bool Paused(uint32_t ticket);
/// Re-enables the microphone and restarts capture; the time spent paused becomes silence.
void Resume();
/// Stops capture, writes the remaining blocks and the final header, closes the file and restores the speaker.
/// Blocks until done (well under a second unless the card is stalling). Returns false when the file is incomplete.
bool Stop(AudioStats* out, std::vector<AudioGap>* gaps);
AudioStats Stats();
bool Active();
/// When sample 0 was captured, on the millis() clock (esp_timer).
uint32_t StartMs();

}  // namespace rec

/// 20*log10(peak / 32768); -inf for 0.
float PeakDbfs(int peak);

}  // namespace roast
