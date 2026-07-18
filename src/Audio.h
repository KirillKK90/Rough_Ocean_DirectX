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

class OceanAudio
{
public:
    ~OceanAudio();

    // Returns false (and stays silent) if no audio device is available.
    bool Init();
    void Shutdown();
    bool Available() const { return available; }
    bool RecordingLoaded() const { return recordingLoaded; }

    // storm01: 0 = glassy .. 1 = severe storm; motion: wave-speed factor 0..1;
    // volume: user volume 0..1. Thread-safe, cheap; call every frame.
    void SetParams(float storm01, float motion, float volume, SoundMode mode);

    // Offline verification: synth levels per storm setting + recording decode
    // stats, printed without playing anything. Returns process exit code.
    static int OfflineTest();

private:
    struct Impl;
    Impl* impl = nullptr;
    bool available = false;
    bool recordingLoaded = false;
};
