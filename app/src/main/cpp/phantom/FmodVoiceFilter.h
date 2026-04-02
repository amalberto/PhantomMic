#pragma once

#include <cstdint>

// Voice filter presets matching baviux VoiceChanger
enum class VoicePreset {
    NONE        = 0,  // Disabled — passthrough
    ROBOT       = 1,
    BABY        = 2,
    TEENAGER    = 3,
    DEEP        = 4,
    DRUNK       = 5,
    FAST        = 6,
    UNDERWATER  = 7,
    SLOW_MOTION = 8,
    REVERSE     = 9,
    FUN1        = 10,
};

class FmodVoiceFilter {
public:
    // Load libfmod.so and libfmod_baviux.so via dlopen, init FMOD system
    // Returns true if init succeeded
    bool init();

    // Release FMOD resources
    void release();

    // Apply the current preset to PCM data in-place.
    // pcm        : pointer to int16_t samples (mono, little-endian)
    // numSamples : number of samples (not bytes)
    // sampleRate : source sample rate (e.g., 16000)
    // Returns true if processing succeeded (otherwise data left untouched)
    bool process(int16_t* pcm, int numSamples, int sampleRate);

    // Set the active preset. Call this from Java side via JNI when user picks a preset.
    void setPreset(VoicePreset preset) { m_preset = preset; }
    VoicePreset getPreset() const { return m_preset; }

    bool isInitialized() const { return m_initialized; }

private:
    bool            m_initialized = false;
    VoicePreset     m_preset      = VoicePreset::NONE;

    // FMOD system handle (opaque pointer from libfmod.so)
    void*           m_fmodSystem  = nullptr;

    // dlopen handles
    void*           m_fmodLib     = nullptr;
    void*           m_baviuxLib   = nullptr;

    // Function pointers from libfmod_baviux.so
    using FnInitSystem      = int  (*)(void* system);
    using FnCreateChannels  = void (*)(void* system, int numChannels);
    using FnAddPitchShift   = void (*)(void* channel, float semitones);
    using FnAddVocoder      = void (*)(void* channel, float carrierFreq, float bandwidth, float modDepth, float modFreq);
    using FnAddEffect       = void (*)(void* channel, int effectId, int param, float* params);
    using FnAddEcho         = void (*)(void* channel, int flag, float delay);
    using FnRemoveEffects   = void (*)(void* channel, int numEffects);
    using FnAddInvert       = void (*)(void* channel);

    FnInitSystem     m_fnInitSystem    = nullptr;
    FnCreateChannels m_fnCreateChannels= nullptr;
    FnAddPitchShift  m_fnAddPitchShift = nullptr;
    FnAddVocoder     m_fnAddVocoder    = nullptr;
    FnAddEffect      m_fnAddEffect     = nullptr;
    FnAddEcho        m_fnAddEcho       = nullptr;
    FnRemoveEffects  m_fnRemoveEffects = nullptr;
    FnAddInvert      m_fnAddInvert     = nullptr;

    // Write PCM to a temp WAV, process with FMOD, read back.
    // Returns processed PCM in output buffer.
    bool processViaFile(int16_t* pcm, int numSamples, int sampleRate);

    // Reverse PCM buffer in-place (for REVERSE preset — pure C++, no FMOD needed)
    void reverseBuffer(int16_t* pcm, int numSamples);
};
