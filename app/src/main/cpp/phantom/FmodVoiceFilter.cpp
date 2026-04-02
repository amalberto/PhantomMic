#include "FmodVoiceFilter.h"
#include "../logging.h"

#include <dlfcn.h>
#include <cstring>
#include <cstdio>
#include <string>
#include <algorithm>
#include <cmath>

// FMOD opaque type definitions (from fmod_common.h public headers)
// We only need the opaque pointer — no need to include FMOD headers.
typedef void FMOD_SYSTEM;
typedef void FMOD_CHANNEL;
typedef void FMOD_CHANNELGROUP;
typedef void FMOD_SOUND;
typedef int  FMOD_RESULT;

// FMOD API function pointer types we need directly from libfmod.so
using FMOD_System_Create_t         = FMOD_RESULT (*)(FMOD_SYSTEM**, unsigned int);
using FMOD_System_Init_t           = FMOD_RESULT (*)(FMOD_SYSTEM*, int, unsigned int, void*);
using FMOD_System_Release_t        = FMOD_RESULT (*)(FMOD_SYSTEM*);
using FMOD_System_CreateSound_t    = FMOD_RESULT (*)(FMOD_SYSTEM*, const char*, unsigned int, void*, FMOD_SOUND**);
using FMOD_System_PlaySound_t      = FMOD_RESULT (*)(FMOD_SYSTEM*, FMOD_SOUND*, FMOD_CHANNELGROUP*, int, FMOD_CHANNEL**);
using FMOD_System_Update_t         = FMOD_RESULT (*)(FMOD_SYSTEM*);
using FMOD_Sound_Release_t         = FMOD_RESULT (*)(FMOD_SOUND*);
using FMOD_Channel_IsPlaying_t     = FMOD_RESULT (*)(FMOD_CHANNEL*, int*);

// FMOD_CREATESOUNDEXINFO (partial — only fields we need)
struct FMOD_CREATESOUNDEXINFO {
    int          cbsize;
    unsigned int length;
    unsigned int fileoffset;
    int          numchannels;
    int          defaultfrequency;
    unsigned int format;           // FMOD_SOUND_FORMAT
    unsigned int decodebuffersize;
    int          initialsubsound;
    int          numsubsounds;
    int*         inclusionlist;
    int          inclusionlistnum;
    void*        pcmreadcallback;
    void*        pcmsetposcallback;
    void*        nonblockcallback;
    const char*  dlsname;
    const char*  encryptionkey;
    int          maxpolyphony;
    void*        userdata;
    unsigned int suggestedsoundtype;
    void*        fileuseropen;
    void*        fileuserclose;
    void*        fileuserread;
    void*        fileuserseek;
    void*        fileuserasyncread;
    void*        fileuserasynccancel;
    void*        speakermapptr;
    void*        initialsubsoundlist;
    unsigned int channelmask;
    int          channelorder;
    float        mindistance;
    float        maxdistance;
    unsigned int dspsamplerate;
};

// FMOD constants
#define FMOD_VERSION          0x00020207
#define FMOD_INIT_NORMAL      0x00000000
#define FMOD_OPENRAW          0x00000400
#define FMOD_OPENONLY         0x00000100
#define FMOD_CREATESAMPLE     0x00000002
#define FMOD_LOOP_OFF         0x00000001
#define FMOD_SOUND_FORMAT_PCM16 2

// baviux effect IDs from smali analysis
#define BAVIUX_EFFECT_CHORUS    1
#define BAVIUX_EFFECT_REVERB    2
#define BAVIUX_EFFECT_LOWPASS   3
#define BAVIUX_EFFECT_TREMOLO   4

bool FmodVoiceFilter::init() {
    if (m_initialized) return true;

    // Android namespace isolation (clns-N) prevents dlopen("libfmod.so") by name
    // from within libxposedlab.so because the restricted namespace doesn't expose
    // the APK's lib directory for implicit resolution.
    // Solution: find the full path by looking at /proc/self/maps for any .so from
    // our APK (libxposedlab.so is already loaded), then open libfmod.so by full path.
    std::string libDir;
    {
        FILE* maps = fopen("/proc/self/maps", "r");
        if (maps) {
            char line[512];
            const char* marker = "libxposedlab.so";
            while (fgets(line, sizeof(line), maps)) {
                if (strstr(line, marker)) {
                    // Line format: addr-addr perms offset dev inode /path/to/lib.so
                    char* path = strchr(line, '/');
                    if (path) {
                        path[strcspn(path, "\n")] = '\0';  // strip newline
                        // dirname: everything up to the last '/'
                        char* slash = strrchr(path, '/');
                        if (slash) { libDir = std::string(path, slash - path); }
                    }
                    break;
                }
            }
            fclose(maps);
        }
    }

    if (libDir.empty()) {
        LOGE("[FmodVoiceFilter] Could not locate lib dir from /proc/self/maps");
        return false;
    }
    LOGI("[FmodVoiceFilter] lib dir = %s", libDir.c_str());

    std::string fmodPath   = libDir + "/libfmod.so";
    std::string baviuxPath = libDir + "/libfmod_baviux.so";

    // Load libfmod.so first (libfmod_baviux.so depends on it)
    m_fmodLib = dlopen(fmodPath.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!m_fmodLib) {
        LOGE("[FmodVoiceFilter] dlopen libfmod.so failed: %s", dlerror());
        return false;
    }

    // Load libfmod_baviux.so
    m_baviuxLib = dlopen(baviuxPath.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!m_baviuxLib) {
        LOGE("[FmodVoiceFilter] dlopen libfmod_baviux.so failed: %s", dlerror());
        dlclose(m_fmodLib);
        m_fmodLib = nullptr;
        return false;
    }

    // Resolve baviux function pointers
    m_fnInitSystem     = (FnInitSystem)     dlsym(m_baviuxLib, "FBC_InitSystem");
    m_fnCreateChannels = (FnCreateChannels) dlsym(m_baviuxLib, "CreateChannelGroups");
    m_fnAddPitchShift  = (FnAddPitchShift)  dlsym(m_baviuxLib, "AddPitchShift");
    m_fnAddVocoder     = (FnAddVocoder)     dlsym(m_baviuxLib, "AddVocoder");
    m_fnAddEffect      = (FnAddEffect)      dlsym(m_baviuxLib, "AddDSP");
    m_fnAddEcho        = (FnAddEcho)        dlsym(m_baviuxLib, "AddEcho");
    m_fnRemoveEffects  = (FnRemoveEffects)  dlsym(m_baviuxLib, "Java_com_baviux_voicechanger_FModWrapper_cRemoveEffects");
    m_fnAddInvert      = (FnAddInvert)      dlsym(m_baviuxLib, "AddInvert");

    if (!m_fnInitSystem || !m_fnCreateChannels || !m_fnAddPitchShift) {
        LOGE("[FmodVoiceFilter] Failed to resolve baviux symbols");
        dlclose(m_baviuxLib); m_baviuxLib = nullptr;
        dlclose(m_fmodLib);   m_fmodLib   = nullptr;
        return false;
    }

    // Create FMOD system via libfmod.so directly
    auto FMOD_System_Create = (FMOD_System_Create_t) dlsym(m_fmodLib, "FMOD_System_Create");
    auto FMOD_System_Init   = (FMOD_System_Init_t)   dlsym(m_fmodLib, "FMOD_System_Init");

    if (!FMOD_System_Create || !FMOD_System_Init) {
        LOGE("[FmodVoiceFilter] Failed to resolve FMOD_System_Create/Init");
        dlclose(m_baviuxLib); m_baviuxLib = nullptr;
        dlclose(m_fmodLib);   m_fmodLib   = nullptr;
        return false;
    }

    FMOD_SYSTEM* system = nullptr;
    if (FMOD_System_Create(&system, FMOD_VERSION) != 0 || !system) {
        LOGE("[FmodVoiceFilter] FMOD_System_Create failed");
        dlclose(m_baviuxLib); m_baviuxLib = nullptr;
        dlclose(m_fmodLib);   m_fmodLib   = nullptr;
        return false;
    }

    if (FMOD_System_Init(system, 32, FMOD_INIT_NORMAL, nullptr) != 0) {
        LOGE("[FmodVoiceFilter] FMOD_System_Init failed");
        dlclose(m_baviuxLib); m_baviuxLib = nullptr;
        dlclose(m_fmodLib);   m_fmodLib   = nullptr;
        return false;
    }

    m_fmodSystem  = system;
    m_initialized = true;
    LOGI("[FmodVoiceFilter] Initialized OK, system=%p", m_fmodSystem);
    return true;
}

void FmodVoiceFilter::release() {
    if (!m_initialized) return;
    auto FMOD_System_Release = (FMOD_System_Release_t) dlsym(m_fmodLib, "FMOD_System_Release");
    if (FMOD_System_Release && m_fmodSystem)
        FMOD_System_Release((FMOD_SYSTEM*) m_fmodSystem);
    m_fmodSystem  = nullptr;
    m_initialized = false;
    if (m_baviuxLib) { dlclose(m_baviuxLib); m_baviuxLib = nullptr; }
    if (m_fmodLib)   { dlclose(m_fmodLib);   m_fmodLib   = nullptr; }
}

void FmodVoiceFilter::reverseBuffer(int16_t* pcm, int numSamples) {
    std::reverse(pcm, pcm + numSamples);
}

bool FmodVoiceFilter::process(int16_t* pcm, int numSamples, int sampleRate) {
    if (m_preset == VoicePreset::NONE) return true; // passthrough

    // REVERSE is pure C++ — no FMOD needed
    if (m_preset == VoicePreset::REVERSE) {
        reverseBuffer(pcm, numSamples);
        return true;
    }

    // processViaFile() uses pure C++ resampling — no FMOD init required for v1.
    // (FMOD init is attempted for future DSP quality, but failure is non-fatal.)
    return processViaFile(pcm, numSamples, sampleRate);
}

bool FmodVoiceFilter::processViaFile(int16_t* pcm, int numSamples, int sampleRate) {
    // Strategy:
    // 1. Write PCM to a temp WAV in /dev/shm or /data/local/tmp
    // 2. Load into FMOD as FMOD_OPENRAW sound
    // 3. Apply DSP chain via baviux functions
    // 4. Render to output buffer
    // 5. Clean up

    // For now, use in-memory PCM processing via FMOD_OPENRAW + custom read callback
    // This avoids disk I/O entirely.

    FMOD_SYSTEM* system = (FMOD_SYSTEM*) m_fmodSystem;

    auto FMOD_System_CreateSound = (FMOD_System_Create_t) nullptr; // placeholder
    // We use a simpler approach: pitch shift via libsoundtouch-compatible math directly,
    // and use baviux's AddPitchShift on a channel group, then route the PCM through it.

    // For the initial implementation, apply pitch shift directly using a simple
    // resampling approach (pitch via speed change), which is fast and allocation-free.
    // Full FMOD file-based processing will be added in next iteration.

    float pitchSemitones = 0.f;
    float tempoFactor    = 1.f;  // 1.0 = no change

    switch (m_preset) {
        case VoicePreset::ROBOT:       pitchSemitones = -5.f;  break;
        case VoicePreset::BABY:        pitchSemitones = +8.f;  break;
        case VoicePreset::TEENAGER:    pitchSemitones = +3.f;  break;
        case VoicePreset::DEEP:        pitchSemitones = -7.f;  break;
        case VoicePreset::DRUNK:       pitchSemitones = -2.f;  break;
        case VoicePreset::FAST:        tempoFactor     = 1.4f; break;
        case VoicePreset::SLOW_MOTION: tempoFactor     = 0.6f; break;
        case VoicePreset::UNDERWATER:  pitchSemitones = -4.f;  break;
        case VoicePreset::FUN1:        pitchSemitones = +5.f;  tempoFactor = 1.2f; break;
        default: break;
    }

    // Apply pitch shift via resampling (changes both pitch and tempo — simple but effective for v1)
    // For true pitch-only shift, FMOD file-based processing is needed (v2).
    if (std::abs(pitchSemitones) > 0.01f || std::abs(tempoFactor - 1.f) > 0.01f) {
        float totalFactor = tempoFactor * std::powf(2.f, pitchSemitones / 12.f);
        int   newLen      = (int)(numSamples / totalFactor);
        if (newLen < 1 || newLen > numSamples * 4) return false;

        static int16_t scratch[1024 * 64]; // 128KB static scratch — enough for 4 sec @ 16kHz
        if (newLen > (int)(sizeof(scratch) / sizeof(scratch[0]))) return false;

        // Linear interpolation resample
        for (int i = 0; i < newLen; i++) {
            float srcF = i * totalFactor;
            int   srcI = (int)srcF;
            float frac = srcF - srcI;
            if (srcI + 1 < numSamples)
                scratch[i] = (int16_t)(pcm[srcI] * (1.f - frac) + pcm[srcI + 1] * frac);
            else
                scratch[i] = pcm[std::min(srcI, numSamples - 1)];
        }

        // Copy back, zero-pad if shorter, truncate if longer
        int copyLen = std::min(newLen, numSamples);
        std::memcpy(pcm, scratch, copyLen * sizeof(int16_t));
        if (copyLen < numSamples)
            std::memset(pcm + copyLen, 0, (numSamples - copyLen) * sizeof(int16_t));
    }

    return true;
}
