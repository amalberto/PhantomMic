//
// Created by amin on 7/24/24.
//

#ifndef PHANTOMMIC_HOOK_COMPAT_H
#define PHANTOMMIC_HOOK_COMPAT_H

#include <string>
#include <dlfcn.h>
#include "utils.h"
#include "KittyScanner.hpp"

namespace HookCompat {
    std::string get_library_name() {
        if (get_sdk_int() <= 24) {
            return "libmedia.so";
        }

        else {
            return "libaudioclient.so";
        }
    }

    std::string get_obtainBuffer_symname() {
        if (get_sdk_int() <= 28) {
            return "_ZN7android11AudioRecord12obtainBufferEPNS0_6BufferEPK8timespecPS3_Pj";
        }
        else {
            return "_ZN7android11AudioRecord12obtainBufferEPNS0_6BufferEPK8timespecPS3_Pm";
        }
    }

    uintptr_t get_symbol(ElfScanner elfScanner, std::vector<std::string> possible_symnames) {
        for (std::string symname: possible_symnames) {
            uintptr_t sym = elfScanner.findSymbol(symname);
            if (sym != 0) {
                return sym;
            }
        }
        return 0;
    }

    // dlsym-based lookup: works regardless of whether the library was already
    // in /proc/self/maps when nativeHook() ran (ElfScanner requires that).
    uintptr_t get_symbol_dlsym(void* handle, const std::vector<std::string>& possible_symnames) {
        if (!handle) return 0;
        for (const auto& symname : possible_symnames) {
            void* sym = dlsym(handle, symname.c_str());
            if (sym) return (uintptr_t) sym;
        }
        return 0;
    }

    uintptr_t get_set_symbol_dlsym(void* handle) {
        return get_symbol_dlsym(handle, {
            // Android 7,8
            "_ZN7android11AudioRecord3setE14audio_source_tj14audio_format_tjmPFviPvS3_ES3_jb15audio_session_tNS0_13transfer_typeE19audio_input_flags_tjiPK18audio_attributes_t",
            "_ZN7android11AudioRecord3setE14audio_source_tj14audio_format_tjjPFviPvS3_ES3_jb15audio_session_tNS0_13transfer_typeE19audio_input_flags_tjiPK18audio_attributes_t",
            // Android 9
            "_ZN7android11AudioRecord3setE14audio_source_tj14audio_format_tjmPFviPvS3_ES3_jb15audio_session_tNS0_13transfer_typeE19audio_input_flags_tjiPK18audio_attributes_ti",
            "_ZN7android11AudioRecord3setE14audio_source_tj14audio_format_tjjPFviPvS3_ES3_jb15audio_session_tNS0_13transfer_typeE19audio_input_flags_tjiPK18audio_attributes_ti",
            // Android 10,11
            "_ZN7android11AudioRecord3setE14audio_source_tj14audio_format_tjmPFviPvS3_ES3_jb15audio_session_tNS0_13transfer_typeE19audio_input_flags_tjiPK18audio_attributes_ti28audio_microphone_direction_tf",
            "_ZN7android11AudioRecord3setE14audio_source_tj14audio_format_tjjPFviPvS3_ES3_jb15audio_session_tNS0_13transfer_typeE19audio_input_flags_tjiPK18audio_attributes_ti28audio_microphone_direction_tf",
            // Android 12
            "_ZN7android11AudioRecord3setE14audio_source_tj14audio_format_t20audio_channel_mask_tmPFviPvS4_ES4_jb15audio_session_tNS0_13transfer_typeE19audio_input_flags_tjiPK18audio_attributes_ti28audio_microphone_direction_tfi",
            // Android 13, 14, 16
            "_ZN7android11AudioRecord3setE14audio_source_tj14audio_format_t20audio_channel_mask_tmRKNS_2wpINS0_20IAudioRecordCallbackEEEjb15audio_session_tNS0_13transfer_typeE19audio_input_flags_tjiPK18audio_attributes_ti28audio_microphone_direction_tfi",
            // Android 15 — extra trailing int
            "_ZN7android11AudioRecord3setE14audio_source_tj14audio_format_t20audio_channel_mask_tmRKNS_2wpINS0_20IAudioRecordCallbackEEEjb15audio_session_tNS0_13transfer_typeE19audio_input_flags_tjiPK18audio_attributes_ti28audio_microphone_direction_tfii",
        });
    }

    uintptr_t get_obtainBuffer_symbol_dlsym(void* handle) {
        return get_symbol_dlsym(handle, {
            "_ZN7android11AudioRecord12obtainBufferEPNS0_6BufferEPK8timespecPS3_Pj",
            "_ZN7android11AudioRecord12obtainBufferEPNS0_6BufferEPK8timespecPS3_Pm",
        });
    }

    uintptr_t get_stop_symbol_dlsym(void* handle) {
        return get_symbol_dlsym(handle, {
            "_ZN7android11AudioRecord4stopEv"
        });
    }

    uintptr_t get_ctor_symbol_dlsym(void* handle) {
        return get_symbol_dlsym(handle, {
            // Android 16: AudioRecord(audio_source_t, uint, audio_format_t,
            //   audio_channel_mask_t, const AttributionSourceState&, size_t,
            //   wp<IAudioRecordCallback> const&, uint, audio_session_t,
            //   transfer_type, audio_input_flags_t, const audio_attributes_t*,
            //   int, audio_microphone_direction_t, float)
            "_ZN7android11AudioRecordC1E14audio_source_tj14audio_format_t20audio_channel_mask_tRKNS_7content22AttributionSourceStateEmRKNS_2wpINS0_20IAudioRecordCallbackEEEj15audio_session_tNS0_13transfer_typeE19audio_input_flags_tPK18audio_attributes_ti28audio_microphone_direction_tf",
        });
    }

    uintptr_t get_start_symbol_dlsym(void* handle) {
        return get_symbol_dlsym(handle, {
            "_ZN7android11AudioRecord5startENS_11AudioSystem12sync_event_tE15audio_session_t"
        });
    }

    uintptr_t get_stop_symbol(ElfScanner elfScanner) {
        return get_symbol(elfScanner, {
            "_ZN7android11AudioRecord4stopEv"
        });
    }

    uintptr_t get_start_symbol(ElfScanner elfScanner) {
        return get_symbol(elfScanner, {
            // Android 5+ (API 21+)
            "_ZN7android11AudioRecord5startENS_11AudioSystem12sync_event_tE15audio_session_t"
        });
    }

    uintptr_t get_obtainBuffer_symbol(ElfScanner elfScanner) {
        return get_symbol(elfScanner, {
                "_ZN7android11AudioRecord12obtainBufferEPNS0_6BufferEPK8timespecPS3_Pj",
                "_ZN7android11AudioRecord12obtainBufferEPNS0_6BufferEPK8timespecPS3_Pm",
        });
    }

    uintptr_t get_set_symbol(KittyScanner::ElfScanner elfScanner) {
        return get_symbol(elfScanner, {
            // Android 7,8
                "_ZN7android11AudioRecord3setE14audio_source_tj14audio_format_tjmPFviPvS3_ES3_jb15audio_session_tNS0_13transfer_typeE19audio_input_flags_tjiPK18audio_attributes_t",
                "_ZN7android11AudioRecord3setE14audio_source_tj14audio_format_tjjPFviPvS3_ES3_jb15audio_session_tNS0_13transfer_typeE19audio_input_flags_tjiPK18audio_attributes_t",
            // Android 9
                "_ZN7android11AudioRecord3setE14audio_source_tj14audio_format_tjmPFviPvS3_ES3_jb15audio_session_tNS0_13transfer_typeE19audio_input_flags_tjiPK18audio_attributes_ti",
                "_ZN7android11AudioRecord3setE14audio_source_tj14audio_format_tjjPFviPvS3_ES3_jb15audio_session_tNS0_13transfer_typeE19audio_input_flags_tjiPK18audio_attributes_ti",
            // Android 10,11
                "_ZN7android11AudioRecord3setE14audio_source_tj14audio_format_tjmPFviPvS3_ES3_jb15audio_session_tNS0_13transfer_typeE19audio_input_flags_tjiPK18audio_attributes_ti28audio_microphone_direction_tf",
                "_ZN7android11AudioRecord3setE14audio_source_tj14audio_format_tjjPFviPvS3_ES3_jb15audio_session_tNS0_13transfer_typeE19audio_input_flags_tjiPK18audio_attributes_ti28audio_microphone_direction_tf",
            // Android 12
                "_ZN7android11AudioRecord3setE14audio_source_tj14audio_format_t20audio_channel_mask_tmPFviPvS4_ES4_jb15audio_session_tNS0_13transfer_typeE19audio_input_flags_tjiPK18audio_attributes_ti28audio_microphone_direction_tfi",
            // Android 13, 14
                "_ZN7android11AudioRecord3setE14audio_source_tj14audio_format_t20audio_channel_mask_tmRKNS_2wpINS0_20IAudioRecordCallbackEEEjb15audio_session_tNS0_13transfer_typeE19audio_input_flags_tjiPK18audio_attributes_ti28audio_microphone_direction_tfi",
            // Android 15 — wp<> callback; maxSharedAudioHistoryMs moved to int32_t
                "_ZN7android11AudioRecord3setE14audio_source_tj14audio_format_t20audio_channel_mask_tmRKNS_2wpINS0_20IAudioRecordCallbackEEEjb15audio_session_tNS0_13transfer_typeE19audio_input_flags_tjiPK18audio_attributes_ti28audio_microphone_direction_tfii",
        });
    }
}

#endif //PHANTOMMIC_HOOK_COMPAT_H
