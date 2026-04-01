#include <cstdint>
#include <malloc.h>
#include <memory>
#include <assert.h>
#include "PhantomBridge.h"
#include "../logging.h"

#define ENCODING_PCM_16BIT          2
#define ENCODING_PCM_24BIT_PACKED   21
#define ENCODING_PCM_32BIT          22
#define ENCODING_PCM_8BIT           3
#define ENCODING_PCM_FLOAT          4

int audioFormatToJava(int audioFormat) {
    switch (audioFormat) {
        case 0x1:
            return ENCODING_PCM_16BIT;
        case 0x2:
            return ENCODING_PCM_8BIT;
        case 0x3u:
            return ENCODING_PCM_32BIT;
        case 0x5u:
            return ENCODING_PCM_FLOAT;
        case 0x6u:
            return ENCODING_PCM_24BIT_PACKED;
        default:
            // Unknown/unreported format: fall back to PCM_16BIT to avoid
            // initialisation errors (same strategy as WhatsMicFix permissive mode)
            LOGW("Unknown audio format 0x%x — defaulting to PCM_16BIT", audioFormat);
            return ENCODING_PCM_16BIT;
    }
}


void PhantomBridge::update_audio_format(JNIEnv* env, int sampleRate, int audioFormat, int channelMask) {
    jclass j_phantomManagerClass = env->GetObjectClass(j_phantomManager);
    jmethodID method = env->GetMethodID(j_phantomManagerClass, "updateAudioFormat", "(III)V");
    env->CallVoidMethod(j_phantomManager, method, sampleRate, channelMask, audioFormatToJava(audioFormat));

    mAudioFormat = audioFormat;
}

void PhantomBridge::load(JNIEnv *env) {
    jclass j_phantomManagerClass = env->GetObjectClass(j_phantomManager);
    jmethodID method = env->GetMethodID(j_phantomManagerClass, "load", "()V");
    env->CallVoidMethod(j_phantomManager, method);
}

PhantomBridge::PhantomBridge(jobject j_phantomManager) : j_phantomManager(j_phantomManager) {}

void PhantomBridge::on_buffer_chunk_loaded(jbyte *buffer, jsize size) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_buffer == nullptr) {
        m_buffer = (jbyte*) malloc(m_buffer_size);
    }

    while (m_buffer_write_position + size > m_buffer_size) {
        m_buffer_size *= 2;
        m_buffer = (jbyte*) realloc(m_buffer, m_buffer_size);
    }

    // PCM_FLOAT
    if (mAudioFormat == 0x5u) {
        float* dst_float = reinterpret_cast<float*>(m_buffer);
        int16_t* src16 = reinterpret_cast<int16_t*>(buffer);
        size_t n_samples = size / sizeof(int16_t);
        for (size_t i = 0; i < n_samples; ++i) {
            dst_float[i + m_buffer_write_position / sizeof(float)] = src16[i] / 32768.0f;
        }
        m_buffer_write_position += n_samples * sizeof(float);
    }
    // PCM_16_BIT
    else {
        if (mAudioFormat != 0x1) {
            LOGW("Unsupported audio format %d", mAudioFormat);
        }
        memcpy(m_buffer + m_buffer_write_position, buffer, size);
        m_buffer_write_position += size;
    }
}

bool PhantomBridge::overwrite_buffer(char* buffer, int size) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return overwrite_buffer_locked(buffer, size);
}

bool PhantomBridge::overwrite_buffer_locked(char* buffer, int size) {
    if (m_buffer_read_position + size > m_buffer_write_position) {
        if (m_buffer_loaded) {
            // Loop: fill remainder then wrap
            int until_bounds = m_buffer_write_position - m_buffer_read_position;
            if (until_bounds > 0) {
                memcpy(buffer, m_buffer + m_buffer_read_position, until_bounds);
            }
            m_buffer_read_position = 0;
            return overwrite_buffer_locked(buffer + until_bounds, size - until_bounds);
        }
        // Still loading: inject silence so WhatsApp gets valid PCM
        memset(buffer, 0, size);
        return false;
    }

    memcpy(buffer, m_buffer + m_buffer_read_position, size);
    m_buffer_read_position += size;

    return true;
}

void PhantomBridge::on_load_done() {
    m_buffer_loaded = true;
}

void PhantomBridge::unload(JNIEnv *env) {
    if (m_buffer != nullptr) {
        free(m_buffer);
        m_buffer = nullptr;

        m_buffer_loaded = false;
        m_buffer_size = 16384;
        m_buffer_write_position = 0;
        m_buffer_read_position = 0;

        jclass j_phantomManagerClass = env->GetObjectClass(j_phantomManager);
        jmethodID method = env->GetMethodID(j_phantomManagerClass, "unload", "()V");
        env->CallVoidMethod(j_phantomManager, method);
    }
}
