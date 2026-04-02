#include <jni.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <linux/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <iomanip>
#include <unwind.h>
#include <sstream>
#include <fstream>
#include <thread>
#include <codecvt>
#include <android/api-level.h>

#include "logging.h"
#include "native_api.h"
#include "KittyMemory/KittyInclude.hpp"

#ifdef __aarch64__
#include "InlineHook/InlineHook.hpp"
#endif

#include "PhantomBridge.h"
#include "hook_compat.h"
#include "phantom/FmodVoiceFilter.h"

struct UnknownArgs {
    char data[1024];
};

jobject j_phantomManager;

JavaVM* JVM;
PhantomBridge* g_phantomBridge;

HookFunType hook_func;
UnhookFunType unhook_func;

int need_log = 5;
size_t acc_frame_count = 0;
int acc_offset = 0;

// The AudioRecord instance we want to inject into (VOICE_COMMUNICATION, 16000Hz).
// Only set when ctor_hook/set_hook sees inputSource==7 or sampleRate==16000.
// obtainBuffer_hook skips all other AudioRecord instances (e.g. AEC at 44100Hz)
// so the buffer is not consumed 2-3x faster than real-time.
void* g_target_ar = nullptr;

// Real-time FMOD voice filter — applied to mic PCM in obtainBuffer_hook.
// When preset == NONE this is a passthrough; no file injection occurs.
FmodVoiceFilter* g_fmodFilter = nullptr;
// Pending preset: nativeSetPreset() can be called before nativeHook() runs
// (because load() fires from ctor_hook, before startRecording() triggers nativeHook).
// We store the value here and apply it once g_fmodFilter is ready.
static int g_pendingPreset = -1;

int32_t (*obtainBuffer_backup)(void*, void*, void*, void*, void*);
int32_t  obtainBuffer_hook(void* v0, void* v1, void* v2, void* v3, void* v4) {
    int32_t status = obtainBuffer_backup(v0, v1, v2, v3, v4);

    // Skip injection for secondary AudioRecords (AEC, noise cancellation, etc.)
    if (g_target_ar != nullptr && v0 != g_target_ar) {
        return status;
    }

    size_t frameCount = * (size_t*) v1;
    size_t size = * (size_t*) ((uintptr_t) v1 + sizeof(size_t));
    char* raw = * (char**) ((uintptr_t) v1 + sizeof(size_t) * 2);

    // ── Real-time FMOD voice filter (Option A) ──────────────────────────────
    // If a preset is active, process the real mic PCM in-place and return it.
    // File injection (phantomBridge) is only used when preset == NONE.
    bool injected = false;
    if (g_fmodFilter != nullptr &&
        g_fmodFilter->getPreset() != VoicePreset::NONE &&
        raw != nullptr && size > 0) {
        // Process mic PCM in-place: int16_t samples, mono
        int numSamples = (int)(size / sizeof(int16_t));
        injected = g_fmodFilter->process(
            reinterpret_cast<int16_t*>(raw), numSamples,
            g_phantomBridge ? g_phantomBridge->getSampleRate() : 16000);
        if (injected && (need_log > 0)) {
            need_log--;
            LOGI("[obtainBuffer] FMOD RT processed %d samples preset=%d",
                 numSamples, (int)g_fmodFilter->getPreset());
        }
    } else {
        // Fallback: inject pre-recorded file (original PhantomMic behaviour)
        injected = g_phantomBridge != nullptr && g_phantomBridge->overwrite_buffer(raw, size);
    }

    static int injectCount = 0;
    if (injected) {
        injectCount++;
        if (injectCount == 1 || injectCount % 200 == 0) {
            JNIEnv* env;
            if (JVM->AttachCurrentThread(&env, nullptr) == JNI_OK) {
                jclass cls = env->GetObjectClass(j_phantomManager);
                jmethodID mid = env->GetMethodID(cls, "reportInjection", "(I)V");
                if (mid) env->CallVoidMethod(j_phantomManager, mid, injectCount);
            }
        }
    }
    if (need_log > 0) {
        need_log--;
        LOGI("[%zu] Inside obtainBuffer size=%zu injected=%d ar=%p target=%p",
             acc_frame_count, size, injected ? 1 : 0, v0, g_target_ar);
    }

    acc_frame_count += frameCount;
    acc_offset += size;
    return status;
}

void (*stop_backup)(void*);
void  stop_hook(void* thiz) {
    stop_backup(thiz);

    if (thiz != g_target_ar) {
        LOGI("[stop_hook] Ignored stop for non-target ar=%p", thiz);
        return;
    }

    // Do NOT unload the buffer here. WhatsApp Business internally calls stop()
    // mid-recording (e.g. for AEC cycling) while the user is still holding the
    // mic button. Unloading here destroys the PCM and causes silence for the
    // rest of the recording. The buffer is reset naturally in update_audio_format
    // when the next recording session begins.
    LOGI("[stop_hook] stop() for target ar=%p — keeping buffer alive", thiz);
    g_target_ar = nullptr;  // allow next AudioRecord to become the new target
}

// AUDIO_SOURCE_* constants (keep in sync with AudioSource.aidl)
static constexpr int32_t AUDIO_SOURCE_DEFAULT             = 0;
static constexpr int32_t AUDIO_SOURCE_MIC                 = 1;
static constexpr int32_t AUDIO_SOURCE_VOICE_COMMUNICATION = 7;

// Shared post-call logic extracted to avoid duplication between hook variants.
static void set_hook_post(JNIEnv** envOut, void* thiz, int32_t inputSource,
                          uint32_t sampleRate, uint32_t format, uint32_t channelMask) {
    // Only treat VOICE_COMMUNICATION (7) or MIC (1) as the injection target.
    // Ignore AEC / CAMCORDER / etc.
    bool isVoice = (inputSource == AUDIO_SOURCE_VOICE_COMMUNICATION ||
                    inputSource == AUDIO_SOURCE_MIC ||
                    inputSource == AUDIO_SOURCE_DEFAULT);
    if (isVoice && g_target_ar == nullptr) {
        g_target_ar = thiz;
        LOGI("[set_hook_post] Locked injection target ar=%p inputSource=%d sampleRate=%u",
             thiz, inputSource, sampleRate);
        JNIEnv* env;
        JVM->AttachCurrentThread(&env, nullptr);
        g_phantomBridge->update_audio_format(env, sampleRate, format, channelMask);
        g_phantomBridge->load(env);
        if (envOut) *envOut = env;
    } else {
        LOGI("[set_hook_post] Skipping non-voice ar=%p inputSource=%d sampleRate=%u",
             thiz, inputSource, sampleRate);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// MODERN hook — Android 13+ (API 33+)
//   AudioRecord::set() replaced callback_t+void* with wp<IAudioRecordCallback> const&
//   No void* user parameter. Added int32_t maxSharedAudioHistoryMs at the end.
// ─────────────────────────────────────────────────────────────────────────────
int32_t (*set_backup_modern)(void*, int32_t, uint32_t, uint32_t, uint32_t, size_t,
                              void*, uint32_t, bool, int32_t, int, uint32_t, uint32_t, int32_t,
                              void*, int, int, float, int32_t);
int32_t set_hook_modern(void* thiz, int32_t inputSource, uint32_t sampleRate,
                         uint32_t format, uint32_t channelMask, size_t frameCount,
                         void* cbf,         // wp<IAudioRecordCallback> const& → single pointer
                         uint32_t notificationFrames, bool threadCanCallJava, int32_t sessionId,
                         int transferType, uint32_t flags, uint32_t uid, int32_t pid,
                         void* pAttributes, int selectedMicDir, int microphoneDirection,
                         float microphoneFieldDimension, int32_t maxSharedAudioHistoryMs) {
    LOGI("[set_hook] PRE  api=modern inputSource=%d sampleRate=%u format=0x%x channelMask=0x%x",
         inputSource, sampleRate, format, channelMask);

    int32_t result = set_backup_modern(thiz, inputSource, sampleRate, format, channelMask,
                                       frameCount, cbf, notificationFrames, threadCanCallJava,
                                       sessionId, transferType, flags, uid, pid, pAttributes,
                                       selectedMicDir, microphoneDirection,
                                       microphoneFieldDimension, maxSharedAudioHistoryMs);
    LOGI("[set_hook] POST result=%d sampleRate=%u format=0x%x channelMask=0x%x",
         result, sampleRate, format, channelMask);

    if (result != 0 && inputSource != AUDIO_SOURCE_DEFAULT) {
        static const int32_t fallbacks[] = { AUDIO_SOURCE_MIC, AUDIO_SOURCE_DEFAULT };
        for (int32_t fb : fallbacks) {
            if (fb == inputSource) continue;
            LOGI("[set_hook] Retrying inputSource=%d (was %d)", fb, inputSource);
            result = set_backup_modern(thiz, fb, sampleRate, format, channelMask, frameCount,
                                       cbf, notificationFrames, threadCanCallJava, sessionId,
                                       transferType, flags, uid, pid, pAttributes,
                                       selectedMicDir, microphoneDirection,
                                       microphoneFieldDimension, maxSharedAudioHistoryMs);
            LOGI("[set_hook] Fallback inputSource=%d result=%d", fb, result);
            if (result == 0) break;
        }
    }

    set_hook_post(nullptr, thiz, inputSource, sampleRate, format, channelMask);
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// LEGACY hook — Android 7–12L (API 24–32)
//   AudioRecord::set() had callback_t cbf + void* user as two separate params.
//   No maxSharedAudioHistoryMs.
// ─────────────────────────────────────────────────────────────────────────────
int32_t (*set_backup_legacy)(void*, int32_t, uint32_t, uint32_t, uint32_t, size_t,
                              void*, void*, uint32_t, bool, int32_t, int, uint32_t, uint32_t, int32_t,
                              void*, int, float);
int32_t set_hook_legacy(void* thiz, int32_t inputSource, uint32_t sampleRate,
                         uint32_t format, uint32_t channelMask, size_t frameCount,
                         void* cbf, void* user,
                         uint32_t notificationFrames, bool threadCanCallJava, int32_t sessionId,
                         int transferType, uint32_t flags, uint32_t uid, int32_t pid,
                         void* pAttributes, int selectedMicDirection,
                         float microphoneFieldDimension) {
    LOGI("[set_hook] PRE  api=legacy inputSource=%d sampleRate=%u format=0x%x channelMask=0x%x",
         inputSource, sampleRate, format, channelMask);

    int32_t result = set_backup_legacy(thiz, inputSource, sampleRate, format, channelMask,
                                       frameCount, cbf, user, notificationFrames,
                                       threadCanCallJava, sessionId, transferType, flags,
                                       uid, pid, pAttributes, selectedMicDirection,
                                       microphoneFieldDimension);
    LOGI("[set_hook] POST result=%d sampleRate=%u format=0x%x channelMask=0x%x",
         result, sampleRate, format, channelMask);

    if (result != 0 && inputSource != AUDIO_SOURCE_DEFAULT) {
        static const int32_t fallbacks[] = { AUDIO_SOURCE_MIC, AUDIO_SOURCE_DEFAULT };
        for (int32_t fb : fallbacks) {
            if (fb == inputSource) continue;
            LOGI("[set_hook] Retrying inputSource=%d (was %d)", fb, inputSource);
            result = set_backup_legacy(thiz, fb, sampleRate, format, channelMask, frameCount,
                                       cbf, user, notificationFrames, threadCanCallJava,
                                       sessionId, transferType, flags, uid, pid, pAttributes,
                                       selectedMicDirection, microphoneFieldDimension);
            LOGI("[set_hook] Fallback inputSource=%d result=%d", fb, result);
            if (result == 0) break;
        }
    }

    set_hook_post(nullptr, thiz, inputSource, sampleRate, format, channelMask);
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// CONSTRUCTOR hook — Android 16+
//   AudioRecord::set() was merged into the constructor in Android 16.
//   Symbol: _ZN7android11AudioRecordC1E...
//   Params (ARM64 regs): this, inputSource, sampleRate, format, channelMask,
//   attributionSource(ref), frameCount, cbf(ref), notificationFrames,
//   sessionId, transferType, flags, pAttributes, micDir, micFieldDimension
// ─────────────────────────────────────────────────────────────────────────────
typedef void (*ctor_backup_t)(void*, int32_t, uint32_t, uint32_t, uint32_t,
                               void*, size_t, void*, uint32_t, int32_t, int32_t,
                               uint32_t, void*, int32_t, float);
ctor_backup_t ctor_backup = nullptr;
void  ctor_hook(void* thiz, int32_t inputSource, uint32_t sampleRate,
                uint32_t format, uint32_t channelMask,
                void* attributionSource, size_t frameCount, void* cbf,
                uint32_t notificationFrames, int32_t sessionId, int32_t transferType,
                uint32_t flags, void* pAttributes, int32_t micDir, float micFieldDimension) {
    LOGI("[ctor_hook] PRE inputSource=%d sampleRate=%u format=0x%x channelMask=0x%x",
         inputSource, sampleRate, format, channelMask);
    ctor_backup(thiz, inputSource, sampleRate, format, channelMask,
                attributionSource, frameCount, cbf, notificationFrames,
                sessionId, transferType, flags, pAttributes, micDir, micFieldDimension);
    LOGI("[ctor_hook] POST — calling set_hook_post");
    set_hook_post(nullptr, thiz, inputSource, sampleRate, format, channelMask);
}

// AudioRecord::start(AudioSystem::sync_event_t, audio_session_t)
// Symbol: _ZN7android11AudioRecord5startENS_11AudioSystem12sync_event_tE15audio_session_t
int32_t (*start_backup)(void* thiz, int32_t syncEvent, int32_t triggerSession);
int32_t start_hook(void* thiz, int32_t syncEvent, int32_t triggerSession) {
    int32_t result = start_backup(thiz, syncEvent, triggerSession);
    LOGI("[start_hook] result=%d syncEvent=%d", result, syncEvent);

    // If the real start() failed (typically because set() failed and mStatus != NO_ERROR),
    // return success so WhatsApp proceeds past the "cannot configure recorder" dialog.
    // Audio data is still served by obtainBuffer_hook when the recording thread calls it.
    if (result != 0) {
        LOGW("[start_hook] start() failed with %d — returning 0 (permissive mode)", result);
        result = 0;
    }
    return result;
}

void on_library_loaded(const char *name, void *handle) {
//    LOGI("Library Loaded %s", name);
}


extern "C" [[gnu::visibility("default")]] [[gnu::used]]
jint JNI_OnLoad(JavaVM *jvm, void*) {
    JNIEnv *env = nullptr;
    jvm->GetEnv((void **)&env, JNI_VERSION_1_6);
    LOGI("JNI_OnLoad");

    JVM = jvm;

    return JNI_VERSION_1_6;
}

extern "C" [[gnu::visibility("default")]] [[gnu::used]]
NativeOnModuleLoaded native_init(const NativeAPIEntries *entries) {
    hook_func = entries->hook_func;
    unhook_func = entries->unhook_func;
    return on_library_loaded;
}

extern "C"
JNIEXPORT void JNICALL
Java_tn_amin_phantom_1mic_PhantomManager_nativeHook(JNIEnv *env, jobject thiz) {
    j_phantomManager = env->NewGlobalRef(thiz);
    g_phantomBridge = new PhantomBridge(j_phantomManager);
    // Point g_fmodFilter to the existing filter inside PhantomBridge
    g_fmodFilter = &g_phantomBridge->m_voiceFilter;
    // Apply any preset that arrived before g_fmodFilter was ready
    if (g_pendingPreset >= 0) {
        g_fmodFilter->setPreset(static_cast<VoicePreset>(g_pendingPreset));
        if (g_pendingPreset != 0 && !g_fmodFilter->isInitialized())
            g_fmodFilter->init();
        LOGI("[FmodVoiceFilter] applied queued preset %d", g_pendingPreset);
        g_pendingPreset = -1;
    }

    LOGI("Doing c++ hook");

    std::string libName = HookCompat::get_library_name();
    LOGI("Target library: %s", libName.c_str());

    // Strategy: ElfScanner reads /proc/self/maps and finds the address of the
    // library copy that is ALREADY mapped in the process (WhatsApp's own copy).
    // Hooking that address patches the right code path.
    //
    // dlopen(RTLD_NOW | RTLD_GLOBAL) must NOT be used — on Android 14+ with
    // linker namespaces it can create a SECOND copy of the library in a different
    // namespace, giving addresses that WhatsApp never calls.
    //
    // If ElfScanner returns 0 (library not yet in /proc/self/maps), we fall back
    // to dlopen(RTLD_NOLOAD) which returns the handle of the existing mapping
    // without creating a new one. If RTLD_NOLOAD also returns null, the library
    // genuinely isn't loaded and no hook can be installed at this point.
    ElfScanner elfScanner = ElfScanner::createWithPath(libName);
    void* libHandle = nullptr;

    // Obtain fallback handle only if ElfScanner found the library already mapped.
    // RTLD_NOLOAD never creates a new mapping — safe for namespace isolation.
    if (!elfScanner.isValid()) {
        libHandle = dlopen(libName.c_str(), RTLD_NOLOAD | RTLD_NOW);
        if (libHandle) {
            LOGI("ElfScanner not found, using RTLD_NOLOAD handle=%p", libHandle);
        } else {
            LOGI("Library %s not yet in /proc/self/maps and RTLD_NOLOAD=null", libName.c_str());
        }
    } else {
        LOGI("ElfScanner found %s", libName.c_str());
    }

    uintptr_t set_symbol = elfScanner.isValid()
        ? HookCompat::get_set_symbol(elfScanner)
        : HookCompat::get_set_symbol_dlsym(libHandle);
    LOGI("AudioRecord::set at %p", (void*) set_symbol);

    uintptr_t obtainBuffer_symbol = elfScanner.isValid()
        ? HookCompat::get_obtainBuffer_symbol(elfScanner)
        : HookCompat::get_obtainBuffer_symbol_dlsym(libHandle);
    LOGI("AudioRecord::obtainBuffer at %p", (void*) obtainBuffer_symbol);

    uintptr_t stop_symbol = elfScanner.isValid()
        ? HookCompat::get_stop_symbol(elfScanner)
        : HookCompat::get_stop_symbol_dlsym(libHandle);
    LOGI("AudioRecord::stop at %p", (void*) stop_symbol);

    if (obtainBuffer_symbol != 0) {
        hook_func((void*) obtainBuffer_symbol, (void*) obtainBuffer_hook, (void**) &obtainBuffer_backup);
        LOGI("Hooked AudioRecord::obtainBuffer");
    } else {
        LOGE("AudioRecord::obtainBuffer symbol not found — audio injection will not work");
    }

    if (stop_symbol != 0) {
        hook_func((void*) stop_symbol, (void*) stop_hook, (void**) &stop_backup);
        LOGI("Hooked AudioRecord::stop");
    } else {
        LOGE("AudioRecord::stop symbol not found — resources will not be released on stop");
    }

    uintptr_t ctor_symbol = HookCompat::get_ctor_symbol_dlsym(libHandle);
    if (ctor_symbol == 0 && elfScanner.isValid()) {
        // try ElfScanner for ctor too if available
        ctor_symbol = HookCompat::get_symbol(elfScanner, {
            "_ZN7android11AudioRecordC1E14audio_source_tj14audio_format_t20audio_channel_mask_tRKNS_7content22AttributionSourceStateEmRKNS_2wpINS0_20IAudioRecordCallbackEEEj15audio_session_tNS0_13transfer_typeE19audio_input_flags_tPK18audio_attributes_ti28audio_microphone_direction_tf",
        });
    }
    LOGI("AudioRecord::AudioRecord(C1) at %p", (void*) ctor_symbol);

    if (set_symbol != 0) {
        int api = android_get_device_api_level();
        if (api >= 33) {
            hook_func((void*) set_symbol, (void*) set_hook_modern, (void**) &set_backup_modern);
            LOGI("Hooked AudioRecord::set (modern, api=%d)", api);
        } else {
            hook_func((void*) set_symbol, (void*) set_hook_legacy, (void**) &set_backup_legacy);
            LOGI("Hooked AudioRecord::set (legacy, api=%d)", api);
        }
    } else if (ctor_symbol != 0) {
        LOGI("AudioRecord::set not found — hooking constructor C1 instead (Android 16+)");
        hook_func((void*) ctor_symbol, (void*) ctor_hook, (void**) &ctor_backup);
        LOGI("Hooked AudioRecord::AudioRecord(C1)");
    } else {
        LOGE("AudioRecord::set and C1 not found — using PCM default (16000Hz mono PCM16)");
        // Provide a safe default format so PCM chunks are not discarded.
        // 0x1 = AUDIO_FORMAT_PCM_16_BIT, 0x10 = AUDIO_CHANNEL_IN_MONO
        g_phantomBridge->update_audio_format(env, 16000, 0x1, 0x10);
        g_phantomBridge->load(env);
    }

    uintptr_t start_symbol = elfScanner.isValid()
        ? HookCompat::get_start_symbol(elfScanner)
        : HookCompat::get_start_symbol_dlsym(libHandle);
    LOGI("AudioRecord::start at %p", (void*) start_symbol);
    if (start_symbol != 0) {
        hook_func((void*) start_symbol, (void*) start_hook, (void**) &start_backup);
        LOGI("Hooked AudioRecord::start");
    } else {
        LOGW("AudioRecord::start symbol not found — permissive start not available");
    }
}

extern "C"
JNIEXPORT void JNICALL
Java_tn_amin_phantom_1mic_PhantomManager_nativeSetPreset(JNIEnv *env, jobject thiz, jint preset) {
    if (g_fmodFilter) {
        g_fmodFilter->setPreset(static_cast<VoicePreset>(preset));
        // Init FMOD lazily on first non-NONE preset
        if (preset != 0 && !g_fmodFilter->isInitialized()) {
            bool ok = g_fmodFilter->init();
            LOGI("[FmodVoiceFilter] init on preset change: %s", ok ? "OK" : "FAILED");
        }
        LOGI("[FmodVoiceFilter] preset set to %d", preset);
    } else {
        // g_fmodFilter not yet created — store preset so nativeHook() picks it up
        g_pendingPreset = preset;
        LOGW("[FmodVoiceFilter] nativeSetPreset called before nativeHook — queuing preset %d", preset);
    }
}

extern "C"
JNIEXPORT void JNICALL
Java_tn_amin_phantom_1mic_audio_AudioMaster_onBufferChunkLoaded(JNIEnv *env, jobject thiz,
                                                                jbyteArray buffer_chunk) {
    jbyte* buffer = env->GetByteArrayElements(buffer_chunk, nullptr);
    int size = env->GetArrayLength(buffer_chunk);

    g_phantomBridge->on_buffer_chunk_loaded(buffer, size);

    env->ReleaseByteArrayElements(buffer_chunk, buffer, 0);
}
extern "C"
JNIEXPORT void JNICALL
Java_tn_amin_phantom_1mic_audio_AudioMaster_onLoadDone(JNIEnv *env, jobject thiz) {
    g_phantomBridge->on_load_done();
}

// Java-side AudioRecord.read() hook calls this to fill a byte[] with injected PCM.
// Returns true if data was injected, false if audio not ready (caller should pass through).
extern "C"
JNIEXPORT jboolean JNICALL
Java_tn_amin_phantom_1mic_PhantomManager_overwriteBuffer(JNIEnv *env, jobject thiz,
                                                          jbyteArray buffer, jint size) {
    if (!g_phantomBridge) return JNI_FALSE;
    jbyte* raw = env->GetByteArrayElements(buffer, nullptr);
    bool result = g_phantomBridge->overwrite_buffer((char*) raw, (size_t) size);
    env->ReleaseByteArrayElements(buffer, raw, result ? 0 : JNI_ABORT);
    return result ? JNI_TRUE : JNI_FALSE;
}