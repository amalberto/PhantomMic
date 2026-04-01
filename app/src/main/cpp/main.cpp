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

int32_t (*obtainBuffer_backup)(void*, void*, void*, void*, void*);
int32_t  obtainBuffer_hook(void* v0, void* v1, void* v2, void* v3, void* v4) {
    int32_t status = obtainBuffer_backup(v0, v1, v2, v3, v4);
    size_t frameCount = * (size_t*) v1;
    size_t size = * (size_t*) ((uintptr_t) v1 + sizeof(size_t));
    size_t frameSize = size / frameCount;
    char* raw = * (char**) ((uintptr_t) v1 + sizeof(size_t) * 2);

    bool injected = g_phantomBridge->overwrite_buffer(raw, size);

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
        LOGI("[%zu] Inside obtainBuffer size=%zu injected=%d", acc_frame_count, size, injected ? 1 : 0);
    }

    acc_frame_count += frameCount;
    acc_offset += size;
    return status;
}

void (*stop_backup)(void*);
void  stop_hook(void* thiz) {
    stop_backup(thiz);

    JNIEnv* env;
    JVM->AttachCurrentThread(&env, nullptr);
    LOGI("AudioRecord::stop()");
    g_phantomBridge->unload(env);
}

// AUDIO_SOURCE_* constants (keep in sync with AudioSource.aidl)
static constexpr int32_t AUDIO_SOURCE_DEFAULT             = 0;
static constexpr int32_t AUDIO_SOURCE_MIC                 = 1;
static constexpr int32_t AUDIO_SOURCE_VOICE_COMMUNICATION = 7;

// Shared post-call logic extracted to avoid duplication between hook variants.
static void set_hook_post(JNIEnv** envOut, uint32_t sampleRate, uint32_t format,
                          uint32_t channelMask) {
    JNIEnv* env;
    JVM->AttachCurrentThread(&env, nullptr);
    g_phantomBridge->update_audio_format(env, sampleRate, format, channelMask);
    g_phantomBridge->load(env);
    if (envOut) *envOut = env;
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

    set_hook_post(nullptr, sampleRate, format, channelMask);
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

    set_hook_post(nullptr, sampleRate, format, channelMask);
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
    LOGI("[ctor_hook] POST — calling update_audio_format");
    JNIEnv* env;
    JVM->AttachCurrentThread(&env, nullptr);
    g_phantomBridge->update_audio_format(env, (int)sampleRate, (int)format, (int)channelMask);
    g_phantomBridge->load(env);
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

    LOGI("Doing c++ hook");

    std::string libName = HookCompat::get_library_name();
    LOGI("Target library: %s", libName.c_str());

    // Force-load the target library into the process before any symbol lookup.
    // nativeHook() runs at Application.onCreate() — before WhatsApp creates its
    // first AudioRecord — so libaudioclient.so is NOT yet in /proc/self/maps.
    // ElfScanner::createWithPath() would return an invalid scanner (base=0) and
    // all findSymbol() calls would return 0. Using dlopen() ensures the library
    // is resident in memory so both dlsym() and ElfScanner work correctly.
    void* libHandle = dlopen(libName.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!libHandle) {
        LOGE("dlopen(%s) failed: %s", libName.c_str(), dlerror());
    } else {
        LOGI("dlopen(%s) ok, handle=%p", libName.c_str(), libHandle);
    }

    uintptr_t set_symbol = HookCompat::get_set_symbol_dlsym(libHandle);
    LOGI("AudioRecord::set at %p", (void*) set_symbol);
    uintptr_t obtainBuffer_symbol = HookCompat::get_obtainBuffer_symbol_dlsym(libHandle);
    LOGI("AudioRecord::obtainBuffer at %p", (void*) obtainBuffer_symbol);
    uintptr_t stop_symbol = HookCompat::get_stop_symbol_dlsym(libHandle);
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

    uintptr_t start_symbol = HookCompat::get_start_symbol_dlsym(libHandle);
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