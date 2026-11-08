#pragma once
namespace audio {
constexpr int DEFAULT_VOLUME = 128;
inline void play_random_tone() {}
inline bool is_quiet_mode() { return true; }
inline void set_keyboard_sfx_enable(bool) {}
}  // namespace audio
