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

    if (g_phantomBridge->overwrite_buffer(raw, size) && need_log > 0) {
        LOGI("Overwritten data");
    }

    if (need_log > 0) {
        need_log--;
        LOGI("[%zu] Inside obtainBuffer (%zu x %zu = %zu)", acc_frame_count, frameCount, frameSize, size);
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
static constexpr int32_t AUDIO_SOURCE_VOICE_UPLINK        = 2;
static constexpr int32_t AUDIO_SOURCE_VOICE_DOWNLINK      = 3;
static constexpr int32_t AUDIO_SOURCE_VOICE_CALL          = 4;
static constexpr int32_t AUDIO_SOURCE_VOICE_COMMUNICATION = 7;

int32_t (*set_backup)(void* thiz, int32_t inputSource, uint32_t sampleRate, uint32_t format,
                      uint32_t channelMask, size_t frameCount, void* callback_ptr, void* callback_refs,
                      uint32_t notificationFrames, bool threadCanCallJava, int32_t sessionId,
                      int transferType, uint32_t flags, uint32_t uid, int32_t pid, void* pAttributes,
                      int selectedDeviceId, int selectedMicDirection, float microphoneFieldDimension,
                      int32_t maxSharedAudioHistoryMs);
int32_t set_hook(void* thiz, int32_t inputSource, uint32_t sampleRate, uint32_t format,
                 uint32_t channelMask, size_t frameCount, void* callback_ptr, void* callback_refs,
                 uint32_t notificationFrames, bool threadCanCallJava, int32_t sessionId,
                 int transferType, uint32_t flags, uint32_t uid, int32_t pid, void* pAttributes,
                 int selectedDeviceId, int selectedMicDirection, float microphoneFieldDimension,
                 int32_t maxSharedAudioHistoryMs) {

    LOGI("[set_hook] PRE  inputSource=%d sampleRate=%u format=0x%x channelMask=0x%x frameCount=%zu",
         inputSource, sampleRate, format, channelMask, frameCount);

    int32_t result = set_backup(thiz, inputSource, sampleRate, format, channelMask, frameCount,
                                callback_ptr, callback_refs, notificationFrames, threadCanCallJava,
                                sessionId, transferType, flags, uid, pid, pAttributes,
                                selectedDeviceId, selectedMicDirection, microphoneFieldDimension,
                                maxSharedAudioHistoryMs);

    LOGI("[set_hook] POST result=%d inputSource=%d sampleRate=%u format=0x%x channelMask=0x%x",
         result, inputSource, sampleRate, format, channelMask);

    // Permissive fallback: if the original inputSource fails, retry with progressively
    // simpler sources. On Android 12+ AudioFlinger can reject VOICE_COMMUNICATION or
    // VOICE_UPLINK/DOWNLINK when the audio policy denies capture for that source type.
    if (result != 0 && inputSource != AUDIO_SOURCE_DEFAULT) {
        static const int32_t fallback_sources[] = {
            AUDIO_SOURCE_MIC,
            AUDIO_SOURCE_DEFAULT
        };
        for (int32_t fb : fallback_sources) {
            if (fb == inputSource) continue;
            LOGI("[set_hook] Retrying with inputSource=%d (was %d, result=%d)",
                 fb, inputSource, result);
            result = set_backup(thiz, fb, sampleRate, format, channelMask, frameCount,
                                callback_ptr, callback_refs, notificationFrames, threadCanCallJava,
                                sessionId, transferType, flags, uid, pid, pAttributes,
                                selectedDeviceId, selectedMicDirection, microphoneFieldDimension,
                                maxSharedAudioHistoryMs);
            LOGI("[set_hook] Fallback inputSource=%d result=%d", fb, result);
            if (result == 0) break;
        }
    }

    JNIEnv* env;
    JVM->AttachCurrentThread(&env, nullptr);
    g_phantomBridge->update_audio_format(env, sampleRate, format, channelMask);
    g_phantomBridge->load(env);

    return result;
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
    ElfScanner g_libTargetELF = ElfScanner::createWithPath(libName);

    uintptr_t set_symbol = HookCompat::get_set_symbol(g_libTargetELF);
    LOGI("AudioRecord::set at %p", (void*) set_symbol);
    uintptr_t obtainBuffer_symbol = HookCompat::get_obtainBuffer_symbol(g_libTargetELF);
    LOGI("AudioRecord::obtainBuffer at %p", (void*) obtainBuffer_symbol);
    uintptr_t stop_symbol = HookCompat::get_stop_symbol(g_libTargetELF);
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

    if (set_symbol != 0) {
        hook_func((void*) set_symbol, (void*) set_hook, (void**) &set_backup);
        LOGI("Hooked AudioRecord::set");
    } else {
        LOGE("AudioRecord::set symbol not found — format detection unavailable, using PCM default");
        // Trigger load now with the pre-initialised PCM default format
        g_phantomBridge->load(env);
    }

    uintptr_t start_symbol = HookCompat::get_start_symbol(g_libTargetELF);
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