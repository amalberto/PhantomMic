package tn.amin.phantom_mic.audio;

import android.media.AudioFormat;
import android.media.MediaCodec;
import android.media.MediaExtractor;
import android.media.MediaFormat;

import java.io.FileDescriptor;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

import tn.amin.phantom_mic.log.Logger;

public class AudioMaster {
    private static final int TIMEOUT_MS = 1000;

    private AudioFormat mOutFormat;
    // Cached copy of the last known output format — survives between recordings
    // in case WhatsApp reuses the AudioRecord object (set_hook doesn't fire again).
    private AudioFormat mLastKnownOutFormat = null;
    private boolean mIsLoading = false;
    private LinearResampler mResampler = null;

    // Source format stored at load() time so setFormat() can recreate the
    // Resampler even if set_hook fires after decoding has already started.
    private int mSrcSampleRate = 0;
    private int mSrcChannelCount = 0;

    private final ExecutorService audioLoadExecutor = Executors.newSingleThreadExecutor();

    public void load(FileDescriptor fd) {
        if (mIsLoading) {
            Logger.d("Still loading another audio, aborting");
            return;
        }

        mIsLoading = true;

        MediaExtractor extractor = new MediaExtractor();

        try {
            extractor.setDataSource(fd);
            extractor.selectTrack(0);
            MediaFormat format = extractor.getTrackFormat(0);

            String mimeType = format.getString(MediaFormat.KEY_MIME);
            if (mimeType == null) {
                Logger.d("mimeType cannot be null");
                return;
            }

            mSrcSampleRate = format.getInteger(MediaFormat.KEY_SAMPLE_RATE);
            mSrcChannelCount = format.getInteger(MediaFormat.KEY_CHANNEL_COUNT);

            Logger.d("[AudioMaster] Source: mime=" + mimeType
                    + " sampleRate=" + mSrcSampleRate
                    + " channels=" + mSrcChannelCount);

            // If set_hook hasn't fired yet, fall back to the last known format
            // (WhatsApp often reuses the AudioRecord object between recordings).
            if (mOutFormat == null && mLastKnownOutFormat != null) {
                mOutFormat = mLastKnownOutFormat;
                Logger.d("[AudioMaster] Reusing cached format: " + mOutFormat.getSampleRate() + "Hz");
            }

            if (mOutFormat != null) {
                Logger.d("[AudioMaster] Target: sampleRate=" + mOutFormat.getSampleRate()
                        + " channelCount=" + mOutFormat.getChannelCount()
                        + " encoding=" + mOutFormat.getEncoding());
                mResampler = buildResampler(mSrcSampleRate, mSrcChannelCount);
            } else {
                Logger.d("[AudioMaster] Target format not yet known — waiting for set_hook");
            }

            MediaCodec codec = MediaCodec.createDecoderByType(mimeType);
            codec.configure(format, null, null, 0);
            codec.start();

            audioLoadExecutor.execute(() -> loadData(codec, format, extractor));
        } catch (IOException e) {
            throw new RuntimeException(e);
        }
    }

    public void unload() {
        mIsLoading = false;
        // Unlock the format so the next recording session accepts a fresh
        // set_hook value (in case WhatsApp changes sample rate between sessions).
        mOutFormat = null;
        try {
            //noinspection ResultOfMethodCallIgnored
            audioLoadExecutor.awaitTermination(500, TimeUnit.MILLISECONDS);
        } catch (InterruptedException ignored) {
        }
    }

    private void loadData(MediaCodec codec, MediaFormat format, MediaExtractor extractor) {
        MediaCodec.BufferInfo bufferInfo = new MediaCodec.BufferInfo();

        boolean isEOS = false;
        long totalPcmBytes = 0;
        do {
            if (!isEOS) {
                int inputBufferIndex = codec.dequeueInputBuffer(TIMEOUT_MS);
                if (inputBufferIndex >= 0) {
                    ByteBuffer inputBuffer = codec.getInputBuffer(inputBufferIndex);
                    if (inputBuffer != null) {
                        int sampleSize = extractor.readSampleData(inputBuffer, 0);
                        if (sampleSize < 0) {
                            codec.queueInputBuffer(inputBufferIndex, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                            isEOS = true;
                        } else {
                            long presentationTimeUs = extractor.getSampleTime();
                            codec.queueInputBuffer(inputBufferIndex, 0, sampleSize, presentationTimeUs, 0);
                            extractor.advance();
                        }
                    }
                }
            }

            int outputBufferIndex = codec.dequeueOutputBuffer(bufferInfo, TIMEOUT_MS);
            if (outputBufferIndex >= 0) {
                ByteBuffer outputBuffer = codec.getOutputBuffer(outputBufferIndex);
                if (outputBuffer != null) {
                    byte[] pcmData = new byte[bufferInfo.size];
                    outputBuffer.get(pcmData);
                    outputBuffer.clear();
                    totalPcmBytes += pcmData.length;

                    // Resample and store PCM data
                    processInBuffer(format, pcmData);
                }
                codec.releaseOutputBuffer(outputBufferIndex, false);
            } else if (outputBufferIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                format = codec.getOutputFormat();
                mSrcSampleRate = format.getInteger(MediaFormat.KEY_SAMPLE_RATE);
                mSrcChannelCount = format.getInteger(MediaFormat.KEY_CHANNEL_COUNT);
                Logger.d("[AudioMaster] Format changed → sampleRate=" + mSrcSampleRate + " channels=" + mSrcChannelCount);
                if (mOutFormat != null) {
                    mResampler = buildResampler(mSrcSampleRate, mSrcChannelCount);
                }
            }

            if (!mIsLoading) {
                Logger.d("Loading aborted");
                break;
            }
        } while ((bufferInfo.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) == 0);

        codec.stop();
        codec.release();
        extractor.release();
        // Do NOT clear mOutFormat — it is needed so that a subsequent recording
        // session (set_hook fires again) can recreate the Resampler immediately.
        mResampler = null;

        Logger.d("[AudioMaster] Loading done — raw PCM decoded: " + totalPcmBytes + " bytes");
        mIsLoading = false;
        onLoadDone();
    }

    private void processInBuffer(MediaFormat source, byte[] bufferChunk) {
        if (bufferChunk.length == 0) {
            return;
        }

        // If output format is not yet known, spin-wait up to 300 ms for set_hook
        // to fire before discarding the chunk entirely.
        if (mOutFormat == null) {
            long deadline = System.currentTimeMillis() + 300;
            while (mOutFormat == null && System.currentTimeMillis() < deadline) {
                try { Thread.sleep(10); } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    return;
                }
            }
            if (mOutFormat == null) {
                Logger.d("[AudioMaster] processInBuffer: format still null after 300ms, discarding chunk");
                return;
            }
        }
        // Lazy Resampler creation: handles the case where set_hook fires after
        // load() has already started (the common case on Android 12+/WhatsApp).
        if (mResampler == null && mSrcSampleRate > 0) {
            Logger.d("[AudioMaster] Lazy Resampler creation: src=" + mSrcSampleRate
                    + "Hz dst=" + mOutFormat.getSampleRate() + "Hz");
            mResampler = buildResampler(mSrcSampleRate, mSrcChannelCount);
        }
        if (mResampler == null) {
            return;
        }

        int srcChannels = source.getInteger(MediaFormat.KEY_CHANNEL_COUNT);
        int dstChannels = mOutFormat.getChannelCount();

        // Manual stereo-to-mono downmix BEFORE resampling.
        // SpeexDSP (used internally by the Resampler library) processes channels
        // independently and does NOT downmix — if we skip this step, stereo PCM
        // is fed into a mono-configured pipeline, producing 2x the expected byte
        // count → WhatsApp consumes it at 2x speed → accelerated audio.
        byte[] input = bufferChunk;
        if (srcChannels == 2 && dstChannels == 1) {
            input = downmixStereoToMono(bufferChunk);
        }

        // Reuse the stateful Resampler (created in load()) so sample history is
        // preserved across chunks — avoids clicks/artefacts at every chunk boundary.
        byte[] resampledChunk = mResampler.resample(input);
        onBufferChunkLoaded(resampledChunk);
    }

    /**
     * Converts interleaved stereo PCM_16BIT (L0 R0 L1 R1 …) to mono PCM_16BIT
     * by averaging left and right samples.  Output is half the size of the input.
     */
    private static byte[] downmixStereoToMono(byte[] stereo) {
        int frames = stereo.length / 4; // 4 bytes per stereo frame (2 ch × 2 bytes)
        byte[] mono = new byte[frames * 2];
        for (int i = 0; i < frames; i++) {
            // Little-endian int16 for left and right channels
            short l = (short) ((stereo[i * 4 + 1] << 8) | (stereo[i * 4] & 0xFF));
            short r = (short) ((stereo[i * 4 + 3] << 8) | (stereo[i * 4 + 2] & 0xFF));
            short m = (short) ((l + r) >> 1);
            mono[i * 2]     = (byte) (m & 0xFF);
            mono[i * 2 + 1] = (byte) ((m >> 8) & 0xFF);
        }
        return mono;
    }

    public void setFormat(AudioFormat format) {
        mOutFormat = format;
        recreateResamplerIfReady();
    }

    public void setFormat(int sampleRate, int channelMask, int encoding) {
        if (mOutFormat != null) {
            // Lock: already set by the first AudioRecord (voice recording channel).
            // WhatsApp Business creates a second AudioRecord at 44100Hz (AEC/echo
            // cancellation) right after — ignore it to avoid corrupting the pipeline.
            Logger.d("[AudioMaster] setFormat ignored (locked to "
                    + mOutFormat.getSampleRate() + "Hz) — rejected: " + sampleRate + "Hz");
            return;
        }
        mOutFormat = new AudioFormat.Builder()
                .setSampleRate(sampleRate)
                .setChannelMask(channelMask)
                .setEncoding(encoding)
                .build();
        mLastKnownOutFormat = mOutFormat;
        Logger.d("[AudioMaster] setFormat locked to " + sampleRate + "Hz");
        recreateResamplerIfReady();
    }

    /**
     * Called every time the target format (from set_hook) is updated.
     * If source info is already known, the Resampler is recreated immediately
     * so that any chunks still being decoded use the correct conversion.
     */
    private void recreateResamplerIfReady() {
        if (mSrcSampleRate > 0 && mOutFormat != null) {
            Logger.d("[AudioMaster] setFormat → recreating Resampler: src=" + mSrcSampleRate
                    + "Hz/" + mSrcChannelCount + "ch → dst=" + mOutFormat.getSampleRate()
                    + "Hz/" + mOutFormat.getChannelCount() + "ch");
            mResampler = buildResampler(mSrcSampleRate, mSrcChannelCount);
        }
    }

    private LinearResampler buildResampler(int srcSampleRate, int srcChannelCount) {
        if (mOutFormat == null) return null;
        // Channel downmix (stereo→mono) is done manually BEFORE calling the Resampler,
        // so the Resampler always receives post-downmix (at most mono) input.
        int effectiveSrcCh = (srcChannelCount >= 2 && mOutFormat.getChannelCount() == 1) ? 1 : srcChannelCount;
        Logger.d("[AudioMaster] buildResampler: " + srcSampleRate + "Hz " + effectiveSrcCh
                + "ch → " + mOutFormat.getSampleRate() + "Hz " + mOutFormat.getChannelCount() + "ch");
        return new LinearResampler(srcSampleRate, effectiveSrcCh, mOutFormat.getSampleRate());
    }

    public AudioFormat getFormat() {
        return mOutFormat;
    }

    public native void onBufferChunkLoaded(byte[] bufferChunk);

    public native void onLoadDone();
}
