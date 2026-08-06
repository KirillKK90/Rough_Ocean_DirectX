#include "Audio.h"

#include <windows.h>
#include <xaudio2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "Util.h"

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr uint32_t kRate = 48000;       // synth rate (recording keeps its own)
    constexpr uint32_t kBlockFrames = 4800; // 100 ms
    constexpr uint32_t kNumBuffers = 3;
    constexpr float kTwoPi = 6.28318530718f;

    struct Rng
    {
        uint32_t s;
        // uniform in [-1, 1)
        inline float Next()
        {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            return static_cast<int32_t>(s) * (1.0f / 2147483648.0f);
        }
    };

    inline float LpCoef(float fc)
    {
        return 1.0f - std::exp(-kTwoPi * fc / kRate);
    }

    // One breaking-wave event: smooth attack, exponential decay, stereo pan,
    // and a per-event blend between the low "crash" and high "wash" bands.
    struct Wash
    {
        bool active = false;
        float attState = 1, decState = 1;
        float attMul = 1, decMul = 1;
        float gain = 0, panL = 0, panR = 0, crash = 0;
    };

    // Pure DSP state, touched only by the audio thread (or OfflineTest).
    struct Synth
    {
        float storm = 0.35f;
        float master = 0.0f;

        Rng rngL{ 0x12345671u }, rngR{ 0x89abcdefu }, rngM{ 0xfedc1234u }, rngEv{ 0x5555aaaau };
        float rl1 = 0, rl2 = 0;
        float sLhi = 0, sLlo = 0, sRhi = 0, sRlo = 0;
        float wLhi = 0, wLlo = 0, wRhi = 0, wRlo = 0;
        float cLhi = 0, cLlo = 0, cRhi = 0, cRlo = 0;
        float dLhi = 0, dLlo = 0, dRhi = 0, dRlo = 0;
        float lfoPhase = 0.0f, lfoPhase2 = 2.1f, windPhase = 0.7f;
        Wash washes[10];
        float washTimer = kRate * 1.5f;

        // rumbleOnly: just the deep roar (layered under the recording).
        void Render(float* interleaved, uint32_t frames, float targetStorm,
                    float targetMaster, bool rumbleOnly);
    };

    void Synth::Render(float* out, uint32_t frames, float targetStorm, float targetMaster,
                       bool rumbleOnly)
    {
        // Parameter glides: sea state ~1.2 s, volume/mute ~0.25 s (click-free).
        float blockDur = frames / float(kRate);
        storm += (targetStorm - storm) * (1.0f - std::exp(-blockDur / 1.2f));
        master += (targetMaster - master) * (1.0f - std::exp(-blockDur / 0.25f));

        // Deep roar: two cascaded one-pole LPs on noise; cutoff and gain rise
        // with the storm. (2/a) renormalizes the LP's amplitude loss.
        const float aR = LpCoef(55.0f + 260.0f * storm);
        const float rumbleGain = 0.42f * std::pow(storm, 1.7f) * (2.0f / aR)
                               * (rumbleOnly ? 0.8f : 1.0f);
        // Continuous surf hiss band.
        const float aShi = LpCoef(2500.0f), aSlo = LpCoef(380.0f);
        const float surfGain = (0.030f + 0.26f * storm) * 4.0f;
        // Breaking-wave bands.
        const float aWhi = LpCoef(3600.0f), aWlo = LpCoef(850.0f);
        const float aChi = LpCoef(520.0f), aClo = LpCoef(130.0f);
        const float washNorm = 2.2f, crashNorm = 4.5f;
        // Wind whistle, only above ~sea state 5.
        const float aDhi = LpCoef(3000.0f), aDlo = LpCoef(1000.0f);
        float windAmt = std::max((storm - 0.5f) * 2.0f, 0.0f);
        const float windGain = windAmt * windAmt * 0.45f;
        // Breaking-wave scheduling rate.
        const float baseInterval = 10.0f + (1.1f - 10.0f) * std::pow(storm, 1.15f);

        for (uint32_t i = 0; i < frames; ++i)
        {
            float nL = rngL.Next(), nR = rngR.Next(), nM = rngM.Next();

            // Rumble (mono, felt in both channels).
            rl1 += aR * (nM - rl1);
            rl2 += aR * (rl1 - rl2);
            float rumble = rl2 * rumbleGain;

            if (rumbleOnly)
            {
                float v = std::tanh(rumble * 0.55f) * master;
                out[i * 2 + 0] = v;
                out[i * 2 + 1] = v;
                continue;
            }

            // --- schedule breaking waves ---
            washTimer -= 1.0f;
            if (washTimer <= 0.0f)
            {
                float u = rngEv.Next() * 0.5f + 0.5f;
                washTimer = baseInterval * (0.45f + 1.1f * u) * kRate;
                for (Wash& w : washes)
                {
                    if (w.active)
                        continue;
                    float r1 = rngEv.Next() * 0.5f + 0.5f;
                    float r2 = rngEv.Next() * 0.5f + 0.5f;
                    float r3 = rngEv.Next() * 0.5f + 0.5f;
                    float r4 = rngEv.Next() * 0.5f + 0.5f;
                    float att = (0.25f + 0.8f * r1) * (1.2f - 0.6f * storm);
                    float dec = 1.2f + 2.8f * r2;
                    w.attMul = std::exp(-1.0f / (att * kRate));
                    w.decMul = std::exp(-1.0f / (dec * kRate));
                    w.attState = 1.0f;
                    w.decState = 1.0f;
                    w.gain = (0.25f + 0.75f * r3) * (0.09f + 0.75f * storm);
                    float pan = 0.15f + 0.7f * r4;
                    w.panL = std::sqrt(1.0f - pan);
                    w.panR = std::sqrt(pan);
                    w.crash = (0.15f + 0.65f * storm) * (0.5f + 0.5f * r1);
                    w.active = true;
                    break;
                }
            }

            float wSumL = 0, wSumR = 0, cSumL = 0, cSumR = 0;
            for (Wash& w : washes)
            {
                if (!w.active)
                    continue;
                w.attState *= w.attMul;
                w.decState *= w.decMul;
                if (w.decState < 1e-3f)
                {
                    w.active = false;
                    continue;
                }
                float env = (1.0f - w.attState) * w.decState * w.gain;
                wSumL += env * (1.0f - w.crash) * w.panL;
                wSumR += env * (1.0f - w.crash) * w.panR;
                cSumL += env * w.crash * w.panL;
                cSumR += env * w.crash * w.panR;
            }

            // Surf hiss with slow, decorrelated swell modulation.
            float lfo1 = 0.55f + 0.45f * std::sin(lfoPhase);
            float lfo2 = 0.55f + 0.45f * std::sin(lfoPhase2);
            lfoPhase += kTwoPi * 0.070f / kRate;
            lfoPhase2 += kTwoPi * 0.053f / kRate;
            if (lfoPhase > kTwoPi) lfoPhase -= kTwoPi;
            if (lfoPhase2 > kTwoPi) lfoPhase2 -= kTwoPi;
            sLhi += aShi * (nL - sLhi); sLlo += aSlo * (nL - sLlo);
            sRhi += aShi * (nR - sRhi); sRlo += aSlo * (nR - sRlo);
            float surfL = (sLhi - sLlo) * surfGain * lfo1;
            float surfR = (sRhi - sRlo) * surfGain * lfo2;

            // Breaking-wave bands.
            wLhi += aWhi * (nL - wLhi); wLlo += aWlo * (nL - wLlo);
            wRhi += aWhi * (nR - wRhi); wRlo += aWlo * (nR - wRlo);
            cLhi += aChi * (nL - cLhi); cLlo += aClo * (nL - cLlo);
            cRhi += aChi * (nR - cRhi); cRlo += aClo * (nR - cRlo);
            float washL = (wLhi - wLlo) * washNorm * wSumL + (cLhi - cLlo) * crashNorm * cSumL;
            float washR = (wRhi - wRlo) * washNorm * wSumR + (cRhi - cRlo) * crashNorm * cSumR;

            // Wind with flutter (complementary between channels).
            float flut = 0.6f + 0.4f * std::sin(windPhase);
            windPhase += kTwoPi * 1.6f / kRate;
            if (windPhase > kTwoPi) windPhase -= kTwoPi;
            dLhi += aDhi * (nL - dLhi); dLlo += aDlo * (nL - dLlo);
            dRhi += aDhi * (nR - dRhi); dRlo += aDlo * (nR - dRlo);
            float windL = (dLhi - dLlo) * windGain * flut;
            float windR = (dRhi - dRlo) * windGain * (1.2f - flut);

            // Mix, soft-limit (headroom scale keeps tanh mostly linear so the
            // storm gets loud without constant saturation), master.
            float mixL = (rumble + surfL + washL + windL) * 0.55f;
            float mixR = (rumble + surfR + washR + windR) * 0.55f;
            out[i * 2 + 0] = std::tanh(mixL) * master;
            out[i * 2 + 1] = std::tanh(mixR) * master;
        }
    }

    // ------------------------------------------------------------------
    // Recorded loop: locate + decode assets/ocean_loop.mp3 via Media
    // Foundation, peak-normalize, and crossfade the tail into the head so
    // the infinite loop has no seam.
    // ------------------------------------------------------------------
    struct Recording
    {
        std::vector<int16_t> pcm; // interleaved
        uint32_t rate = 0;
        uint16_t channels = 0;
        uint32_t loopBeginFrames = 0;
        double seconds = 0;
        float rms = 0;
    };

    std::wstring FindAssetPath(const wchar_t* name)
    {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::filesystem::path dir = std::filesystem::path(exePath).parent_path();
        for (int i = 0; i < 4; ++i)
        {
            std::filesystem::path candidate = dir / L"assets" / name;
            if (std::filesystem::exists(candidate))
                return candidate.wstring();
            dir = dir.parent_path();
        }
        return L"";
    }

    // Decode any Media-Foundation-supported audio file to interleaved float
    // PCM at its native rate/channels. Used by the ambient loop and the
    // meteorite one-shots.
    bool DecodeMp3(const std::wstring& path, std::vector<float>& data,
                   uint32_t& rate, uint16_t& channels)
    {
        data.clear();
        rate = 0;
        channels = 0;
        if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
            return false;

        bool ok = false;
        {
            ComPtr<IMFSourceReader> reader;
            if (SUCCEEDED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader)))
            {
                // Ask for float PCM; the reader inserts the decoder, native
                // rate/channels are kept.
                ComPtr<IMFMediaType> want;
                MFCreateMediaType(&want);
                want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
                if (SUCCEEDED(reader->SetCurrentMediaType(
                        static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, want.Get())))
                {
                    ComPtr<IMFMediaType> got;
                    reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &got);
                    UINT32 v = 0;
                    got->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &v);
                    rate = v;
                    got->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &v);
                    channels = static_cast<uint16_t>(v);

                    for (;;)
                    {
                        DWORD flags = 0;
                        ComPtr<IMFSample> sample;
                        if (FAILED(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM),
                                                      0, nullptr, &flags, nullptr, &sample)))
                            break;
                        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
                        {
                            ok = rate != 0 && channels != 0 && !data.empty();
                            break;
                        }
                        if (!sample)
                            continue;
                        ComPtr<IMFMediaBuffer> buf;
                        if (FAILED(sample->ConvertToContiguousBuffer(&buf)))
                            break;
                        BYTE* ptr = nullptr;
                        DWORD len = 0;
                        if (SUCCEEDED(buf->Lock(&ptr, nullptr, &len)))
                        {
                            size_t count = len / sizeof(float);
                            const float* f = reinterpret_cast<const float*>(ptr);
                            data.insert(data.end(), f, f + count);
                            buf->Unlock();
                        }
                    }
                }
            }
        }
        MFShutdown();
        return ok;
    }

    bool LoadRecording(Recording& rec)
    {
        std::wstring path = FindAssetPath(L"ocean_loop.mp3");
        if (path.empty())
        {
            LogF("Audio: assets/ocean_loop.mp3 not found.\n");
            return false;
        }
        std::vector<float> data;
        uint32_t rate = 0;
        uint16_t channels = 0;
        if (!DecodeMp3(path, data, rate, channels))
            return false;

        uint32_t frames = static_cast<uint32_t>(data.size() / channels);
        if (frames < rate * 10) // want at least 10 s of material
            return false;

        // Peak-normalize to 0.95.
        float peak = 1e-6f;
        for (float v : data)
            peak = std::max(peak, std::fabs(v));
        float gain = 0.95f / peak;

        // Crossfade the last F frames toward the first F frames: the loop
        // region [F, N) then wraps seamlessly to frame F.
        uint32_t F = std::min(rate * 2, frames / 4);
        for (uint32_t k = 0; k < F; ++k)
        {
            float t = float(k) / float(F);
            t = t * t * (3.0f - 2.0f * t);
            uint32_t i = frames - F + k;
            for (uint32_t c = 0; c < channels; ++c)
            {
                float& dst = data[size_t(i) * channels + c];
                dst = dst * (1.0f - t) + data[size_t(k) * channels + c] * t;
            }
        }

        rec.pcm.resize(data.size());
        double sum2 = 0;
        for (size_t i = 0; i < data.size(); ++i)
        {
            float v = std::clamp(data[i] * gain, -1.0f, 1.0f);
            sum2 += double(v) * v;
            rec.pcm[i] = static_cast<int16_t>(std::lround(v * 32767.0f));
        }
        rec.rate = rate;
        rec.channels = channels;
        rec.loopBeginFrames = F;
        rec.seconds = double(frames) / rate;
        rec.rms = static_cast<float>(std::sqrt(sum2 / double(data.size())));
        return true;
    }

    // ------------------------------------------------------------------
    // Meteorite event one-shots: decoded, peak-normalized 16-bit PCM held
    // whole in memory and fired on demand through their own source voices.
    // ------------------------------------------------------------------
    struct Sfx
    {
        std::vector<int16_t> pcm; // interleaved
        uint32_t rate = 0;
        uint16_t channels = 0;
        double seconds = 0;
    };

    // Load an event one-shot. When windowSec > 0 keep only the loudest window
    // of that length (used to pull the punchiest crash out of a long wave
    // recording); otherwise keep the whole clip. Peak-normalized, then lifted
    // by upward "loudness" compression (drive > 1) so the event sits clearly
    // above the ambient sea; short raised-cosine fades keep retriggers click-free.
    bool LoadSfx(const wchar_t* name, Sfx& sfx, float windowSec, float drive)
    {
        std::wstring path = FindAssetPath(name);
        if (path.empty())
        {
            LogF("Audio: assets/%ls not found.\n", name);
            return false;
        }
        std::vector<float> data;
        uint32_t rate = 0;
        uint16_t channels = 0;
        if (!DecodeMp3(path, data, rate, channels) || rate == 0 || channels == 0)
            return false;

        uint32_t frames = static_cast<uint32_t>(data.size() / channels);
        if (frames == 0)
            return false;

        auto frameEnergy = [&](uint32_t f) {
            double e = 0;
            for (uint16_t c = 0; c < channels; ++c)
            {
                float s = data[size_t(f) * channels + c];
                e += double(s) * s;
            }
            return e;
        };

        // Pick the most energetic window if asked; else keep everything.
        uint32_t start = 0;
        uint32_t keep = frames;
        if (windowSec > 0.0f && frames > uint32_t(windowSec * rate))
        {
            uint32_t win = uint32_t(windowSec * rate);
            double e = 0;
            for (uint32_t f = 0; f < win; ++f)
                e += frameEnergy(f);
            double best = e;
            uint32_t bestStart = 0;
            for (uint32_t f = 1; f + win <= frames; ++f)
            {
                e += frameEnergy(f + win - 1) - frameEnergy(f - 1);
                if (e > best)
                {
                    best = e;
                    bestStart = f;
                }
            }
            start = bestStart;
            keep = win;
        }

        float peak = 1e-6f;
        for (uint32_t f = 0; f < keep; ++f)
            for (uint16_t c = 0; c < channels; ++c)
                peak = std::max(peak, std::fabs(data[size_t(start + f) * channels + c]));
        float norm = 1.0f / peak;                 // peak-normalize to unity first
        float tdrive = std::max(drive, 1.0f);
        float tnorm = std::tanh(tdrive);          // renormalizes so peak stays ~1
        const float ceiling = 0.97f;              // leave a hair of headroom

        uint32_t fade = std::min<uint32_t>(uint32_t(0.04f * rate), keep / 2);
        sfx.pcm.resize(size_t(keep) * channels);
        for (uint32_t f = 0; f < keep; ++f)
        {
            float env = 1.0f;
            if (fade > 0 && f < fade)
                env = 0.5f - 0.5f * std::cos(3.14159265f * f / fade);
            else if (fade > 0 && f >= keep - fade)
                env = 0.5f - 0.5f * std::cos(3.14159265f * (keep - 1 - f) / fade);
            for (uint16_t c = 0; c < channels; ++c)
            {
                float v = data[size_t(start + f) * channels + c] * norm;
                if (tdrive > 1.001f)
                    v = std::tanh(tdrive * v) / tnorm; // lift the body, keep peak ~1
                v = std::clamp(v * env * ceiling, -1.0f, 1.0f);
                sfx.pcm[size_t(f) * channels + c] = static_cast<int16_t>(std::lround(v * 32767.0f));
            }
        }
        sfx.rate = rate;
        sfx.channels = channels;
        sfx.seconds = double(keep) / rate;
        return true;
    }

    // XAudio2 buffer-completion callback: just wakes the synth thread.
    struct VoiceCallback : IXAudio2VoiceCallback
    {
        HANDLE event = nullptr;
        void STDMETHODCALLTYPE OnBufferEnd(void*) override { SetEvent(event); }
        void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
        void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
        void STDMETHODCALLTYPE OnStreamEnd() override {}
        void STDMETHODCALLTYPE OnBufferStart(void*) override {}
        void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
        void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override {}
    };
}

struct OceanAudio::Impl
{
    IXAudio2* xaudio = nullptr;
    IXAudio2MasteringVoice* masterVoice = nullptr;
    IXAudio2SourceVoice* synthVoice = nullptr;
    IXAudio2SourceVoice* recVoice = nullptr;
    IXAudio2SourceVoice* sfxVoice[3] = {}; // indexed by MeteorSfx
    Sfx sfx[3];
    std::atomic<float> sfxVolume{ 0.8f };
    VoiceCallback callback;
    Synth synth;
    Recording rec;
    float recBaseGain = 1.0f;
    float recVolSmoothed = 0.0f;
    float buffers[kNumBuffers][kBlockFrames * 2];
    uint32_t nextBuffer = 0;
    std::thread thread;
    std::atomic<bool> quit{ false };
    std::atomic<float> targetStorm{ 0.35f };
    std::atomic<float> targetMaster{ 0.0f };
    std::atomic<int> mode{ int(SoundMode::Soothing) };

    void SubmitNext()
    {
        SoundMode m = SoundMode(mode.load());
        bool rumbleOnly = m != SoundMode::Realistic; // soothing/off: roar layer only
        float master = targetMaster.load();
        float synthMaster = (m == SoundMode::Off) ? 0.0f
                          : (m == SoundMode::Soothing ? master * 0.9f : master);

        float* buf = buffers[nextBuffer];
        nextBuffer = (nextBuffer + 1) % kNumBuffers;
        synth.Render(buf, kBlockFrames, targetStorm.load(), synthMaster, rumbleOnly);
        XAUDIO2_BUFFER xb = {};
        xb.AudioBytes = kBlockFrames * 2 * sizeof(float);
        xb.pAudioData = reinterpret_cast<const BYTE*>(buf);
        synthVoice->SubmitSourceBuffer(&xb);
    }

    void UpdateRecordedVoice()
    {
        if (!recVoice)
            return;
        SoundMode m = SoundMode(mode.load());
        float storm = synth.storm; // already smoothed by the synth
        // Calm seas stay gentle, storms swell up; overall kept mellow.
        float target = 0.0f;
        if (m == SoundMode::Soothing)
            target = targetMaster.load() * (0.40f + 0.60f * std::pow(storm, 0.9f)) * recBaseGain;
        recVolSmoothed += (target - recVolSmoothed) * 0.12f; // ~0.5 s at 10 Hz updates
        recVoice->SetVolume(recVolSmoothed);
    }

    void ThreadMain()
    {
        while (!quit.load())
        {
            WaitForSingleObject(callback.event, 50);
            XAUDIO2_VOICE_STATE st = {};
            synthVoice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            while (st.BuffersQueued < kNumBuffers && !quit.load())
            {
                SubmitNext();
                ++st.BuffersQueued;
            }
            UpdateRecordedVoice();
        }
    }
};

OceanAudio::~OceanAudio()
{
    Shutdown();
}

bool OceanAudio::Init()
{
    impl = new Impl();
    if (FAILED(XAudio2Create(&impl->xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR)) ||
        FAILED(impl->xaudio->CreateMasteringVoice(&impl->masterVoice)))
    {
        LogF("Audio: no output device, sound disabled.\n");
        Shutdown();
        return false;
    }

    // Synth voice (procedural mode + storm roar layer).
    WAVEFORMATEX wf = {};
    wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    wf.nChannels = 2;
    wf.nSamplesPerSec = kRate;
    wf.wBitsPerSample = 32;
    wf.nBlockAlign = wf.nChannels * wf.wBitsPerSample / 8;
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    impl->callback.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(impl->xaudio->CreateSourceVoice(&impl->synthVoice, &wf, 0,
        XAUDIO2_DEFAULT_FREQ_RATIO, &impl->callback)))
    {
        LogF("Audio: source voice creation failed, sound disabled.\n");
        Shutdown();
        return false;
    }

    // Recorded loop voice.
    if (LoadRecording(impl->rec))
    {
        WAVEFORMATEX rwf = {};
        rwf.wFormatTag = WAVE_FORMAT_PCM;
        rwf.nChannels = impl->rec.channels;
        rwf.nSamplesPerSec = impl->rec.rate;
        rwf.wBitsPerSample = 16;
        rwf.nBlockAlign = rwf.nChannels * rwf.wBitsPerSample / 8;
        rwf.nAvgBytesPerSec = rwf.nSamplesPerSec * rwf.nBlockAlign;
        if (SUCCEEDED(impl->xaudio->CreateSourceVoice(&impl->recVoice, &rwf)))
        {
            uint32_t frames = static_cast<uint32_t>(impl->rec.pcm.size() / impl->rec.channels);
            XAUDIO2_BUFFER xb = {};
            xb.AudioBytes = static_cast<UINT32>(impl->rec.pcm.size() * sizeof(int16_t));
            xb.pAudioData = reinterpret_cast<const BYTE*>(impl->rec.pcm.data());
            xb.LoopBegin = impl->rec.loopBeginFrames;
            xb.LoopLength = frames - impl->rec.loopBeginFrames;
            xb.LoopCount = XAUDIO2_LOOP_INFINITE;
            impl->recVoice->SubmitSourceBuffer(&xb);
            // Keep perceived loudness consistent regardless of how hot the
            // source file is (target ~0.18 RMS at full volume).
            impl->recBaseGain = std::clamp(0.18f / std::max(impl->rec.rms, 0.01f), 0.2f, 3.0f);
            impl->recVoice->SetVolume(0.0f);
            impl->recVoice->Start(0);
            recordingLoaded = true;
            LogF("Audio: loaded ocean recording (%.0f s, %u Hz, %u ch, RMS %.3f).\n",
                 impl->rec.seconds, impl->rec.rate, impl->rec.channels, impl->rec.rms);
        }
    }
    if (!recordingLoaded)
        LogF("Audio: recording unavailable, falling back to synthesized sound.\n");

    // Meteorite event one-shots (own source voices, played on demand).
    // drive = upward loudness lift so events ride well above the ambient sea;
    // the splash is peaky and needs the most.
    struct SfxSpec { const wchar_t* file; float windowSec; float drive; };
    const SfxSpec specs[3] = {
        { L"meteor_descent.mp3", 0.0f, 3.5f }, // whoosh: whole clip
        { L"meteor_impact.mp3",  0.0f, 6.0f }, // splash: whole clip (very peaky)
        { L"meteor_waves.mp3",   7.0f, 3.0f }, // loudest 7 s of the crash
    };
    int sfxOk = 0;
    for (int i = 0; i < 3; ++i)
    {
        if (!LoadSfx(specs[i].file, impl->sfx[i], specs[i].windowSec, specs[i].drive))
            continue;
        WAVEFORMATEX sf = {};
        sf.wFormatTag = WAVE_FORMAT_PCM;
        sf.nChannels = impl->sfx[i].channels;
        sf.nSamplesPerSec = impl->sfx[i].rate;
        sf.wBitsPerSample = 16;
        sf.nBlockAlign = sf.nChannels * sf.wBitsPerSample / 8;
        sf.nAvgBytesPerSec = sf.nSamplesPerSec * sf.nBlockAlign;
        if (SUCCEEDED(impl->xaudio->CreateSourceVoice(&impl->sfxVoice[i], &sf)))
            ++sfxOk;
    }
    meteorSfxLoaded = (sfxOk == 3);
    LogF("Audio: meteorite one-shots loaded %d/3.\n", sfxOk);

    for (uint32_t i = 0; i < kNumBuffers; ++i)
        impl->SubmitNext();
    impl->synthVoice->Start(0);
    impl->thread = std::thread([this] { impl->ThreadMain(); });
    available = true;
    LogF("Audio: started (XAudio2, %u Hz).\n", kRate);
    return true;
}

void OceanAudio::Shutdown()
{
    if (!impl)
        return;
    impl->quit = true;
    if (impl->callback.event)
        SetEvent(impl->callback.event);
    if (impl->thread.joinable())
        impl->thread.join();
    if (impl->synthVoice)
    {
        impl->synthVoice->Stop(0);
        impl->synthVoice->DestroyVoice();
    }
    if (impl->recVoice)
    {
        impl->recVoice->Stop(0);
        impl->recVoice->DestroyVoice();
    }
    for (IXAudio2SourceVoice*& v : impl->sfxVoice)
    {
        if (v)
        {
            v->Stop(0);
            v->DestroyVoice();
            v = nullptr;
        }
    }
    if (impl->masterVoice)
        impl->masterVoice->DestroyVoice();
    if (impl->xaudio)
        impl->xaudio->Release();
    if (impl->callback.event)
        CloseHandle(impl->callback.event);
    delete impl;
    impl = nullptr;
    available = false;
    recordingLoaded = false;
}

void OceanAudio::SetParams(float storm01, float motion, float volume, SoundMode mode)
{
    if (!impl)
        return;
    if (mode == SoundMode::Soothing && !recordingLoaded)
        mode = SoundMode::Realistic;
    impl->targetStorm.store(std::clamp(storm01, 0.0f, 1.0f));
    impl->targetMaster.store(std::clamp(volume, 0.0f, 1.0f) * std::clamp(motion, 0.0f, 1.0f));
    impl->sfxVolume.store(std::clamp(volume, 0.0f, 1.0f)); // events ignore wave-speed
    impl->mode.store(int(mode));
}

void OceanAudio::PlayMeteor(MeteorSfx which)
{
    if (!impl)
        return;
    if (SoundMode(impl->mode.load()) == SoundMode::Off)
        return; // sound turned off => silent
    int i = int(which);
    if (i < 0 || i >= 3 || !impl->sfxVoice[i])
        return;
    float vol = impl->sfxVolume.load();
    if (vol <= 0.0f)
        return;

    // Per-event balance: the events are meant to be clearly louder than the sea.
    const float gain[3] = { 1.05f, 1.20f, 1.10f };
    // The splash silences the descent whoosh so they don't fight.
    if (which == MeteorSfx::Impact && impl->sfxVoice[int(MeteorSfx::Descent)])
        impl->sfxVoice[int(MeteorSfx::Descent)]->Stop(0);

    IXAudio2SourceVoice* v = impl->sfxVoice[i];
    v->Stop(0);
    v->FlushSourceBuffers();
    XAUDIO2_BUFFER xb = {};
    xb.AudioBytes = static_cast<UINT32>(impl->sfx[i].pcm.size() * sizeof(int16_t));
    xb.pAudioData = reinterpret_cast<const BYTE*>(impl->sfx[i].pcm.data());
    xb.Flags = XAUDIO2_END_OF_STREAM;
    v->SubmitSourceBuffer(&xb);
    v->SetVolume(vol * gain[i]);
    v->Start(0);
}

int OceanAudio::OfflineTest()
{
    LogF("Offline audio level test (20 s per level):\n");
    bool ok = true;
    for (float storm : { 0.0f, 0.15f, 0.35f, 0.55f, 0.80f, 1.0f })
    {
        Synth synth;
        synth.storm = storm;
        synth.master = 1.0f;
        std::vector<float> buf(kBlockFrames * 2);
        double sum2 = 0;
        double peak = 0;
        uint64_t n = 0;
        for (int block = 0; block < 200; ++block)
        {
            synth.Render(buf.data(), kBlockFrames, storm, 1.0f, false);
            if (block < 3)
                continue; // let filters settle
            for (float v : buf)
            {
                sum2 += double(v) * v;
                peak = std::max(peak, double(std::fabs(v)));
                ++n;
            }
        }
        double rms = std::sqrt(sum2 / double(n));
        LogF("  storm %.2f: RMS %.4f  peak %.3f\n", storm, rms, peak);
        if (!std::isfinite(rms) || peak > 1.0001 || (storm > 0.2f && rms < 1e-4))
            ok = false;
    }

    Recording rec;
    if (LoadRecording(rec))
    {
        LogF("Recording: %.1f s, %u Hz, %u ch, RMS %.3f, loop crossfade %.1f s\n",
             rec.seconds, rec.rate, rec.channels, rec.rms,
             double(rec.loopBeginFrames) / rec.rate);
        if (rec.seconds < 10 || rec.rms < 0.01f)
            ok = false;
    }
    else
    {
        LogF("Recording: FAILED to load assets/ocean_loop.mp3\n");
        ok = false;
    }

    struct { const wchar_t* file; float windowSec; float drive; } sfxList[3] = {
        { L"meteor_descent.mp3", 0.0f, 3.5f },
        { L"meteor_impact.mp3", 0.0f, 6.0f },
        { L"meteor_waves.mp3", 7.0f, 3.0f },
    };
    for (auto& s : sfxList)
    {
        Sfx sfx;
        if (LoadSfx(s.file, sfx, s.windowSec, s.drive))
        {
            double sum2 = 0;
            for (int16_t v : sfx.pcm) { double f = v / 32768.0; sum2 += f * f; }
            double rms = std::sqrt(sum2 / std::max<size_t>(sfx.pcm.size(), 1));
            LogF("SFX %-20ls: %.2f s, %u Hz, %u ch, RMS %.3f\n",
                 s.file, sfx.seconds, sfx.rate, sfx.channels, rms);
            if (sfx.seconds < 0.2 || rms < 0.01)
                ok = false;
        }
        else
        {
            LogF("SFX %-20ls: FAILED to load\n", s.file);
            ok = false;
        }
    }

    LogF("Audio offline test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
