#pragma once

// Ocean sound through XAudio2, two flavors:
//  - Soothing (default): a CC0 recording of calm Atlantic waves
//    (assets/ocean_loop.mp3), seamlessly looped, plus a soft synthesized
//    low roar that grows with the sea state.
//  - Realistic: fully procedural synthesis (rumble, surf hiss, breaking-wave
//    events, storm wind) — rougher and more chaotic, but asset-free.
enum class SoundMode
{
    Off = 0,
    Soothing = 1,
    Realistic = 2,
};

// One-shot meteorite event sounds (CC0 recordings in assets/, high-bitrate MP3).
enum class MeteorSfx
{
    Descent = 0, // whoosh while the rock streaks down
    Impact = 1,  // splash as it hits the water
    Waves = 2,   // surge of the first powerful spreading waves
};

class OceanAudio
{
public:
    ~OceanAudio();

    // Returns false (and stays silent) if no audio device is available.
    bool Init();
    void Shutdown();
    bool Available() const { return available; }
    bool RecordingLoaded() const { return recordingLoaded; }
    bool MeteorSfxLoaded() const { return meteorSfxLoaded; }

    // storm01: 0 = glassy .. 1 = severe storm; motion: wave-speed factor 0..1;
    // volume: user volume 0..1. Thread-safe, cheap; call every frame.
    void SetParams(float storm01, float motion, float volume, SoundMode mode);

    // Fire a one-shot meteorite event sound. Uses the current volume, is silent
    // when the sound mode is Off. Cheap and thread-safe; call on the event.
    void PlayMeteor(MeteorSfx which);

    // Offline verification: synth levels per storm setting + recording decode
    // stats, printed without playing anything. Returns process exit code.
    static int OfflineTest();

private:
    struct Impl;
    Impl* impl = nullptr;
    bool available = false;
    bool recordingLoaded = false;
    bool meteorSfxLoaded = false;
};
