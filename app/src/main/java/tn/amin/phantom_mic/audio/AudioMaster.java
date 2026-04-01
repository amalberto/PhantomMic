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

import io.github.nailik.androidresampler.Resampler;
import io.github.nailik.androidresampler.ResamplerConfiguration;
import io.github.nailik.androidresampler.data.ResamplerChannel;
import io.github.nailik.androidresampler.data.ResamplerQuality;
import tn.amin.phantom_mic.log.Logger;

public class AudioMaster {
    private static final int TIMEOUT_MS = 1000;

    private AudioFormat mOutFormat;
    private boolean mIsLoading = false;
    private Resampler mResampler = null;

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

            Logger.d("[AudioMaster] Source: mime=" + mimeType
                    + " sampleRate=" + format.getInteger(MediaFormat.KEY_SAMPLE_RATE)
                    + " channels=" + format.getInteger(MediaFormat.KEY_CHANNEL_COUNT));
            if (mOutFormat != null) {
                Logger.d("[AudioMaster] Target: sampleRate=" + mOutFormat.getSampleRate()
                        + " channelCount=" + mOutFormat.getChannelCount()
                        + " encoding=" + mOutFormat.getEncoding());

                // Create Resampler once for this file so state is preserved across chunks.
                // Channel downmix (stereo→mono) is handled manually in processInBuffer
                // BEFORE resampling, so the Resampler always sees the post-downmix channel
                // count as input (avoids relying on SpeexDSP channel mixing).
                int effectiveSrcChannels = Math.min(
                        format.getInteger(MediaFormat.KEY_CHANNEL_COUNT),
                        mOutFormat.getChannelCount());
                ResamplerChannel inChannel = effectiveSrcChannels == 1
                        ? ResamplerChannel.MONO : ResamplerChannel.STEREO;
                ResamplerChannel outChannel = mOutFormat.getChannelCount() == 1
                        ? ResamplerChannel.MONO : ResamplerChannel.STEREO;
                mResampler = new Resampler(new ResamplerConfiguration(
                        ResamplerQuality.BEST, inChannel,
                        format.getInteger(MediaFormat.KEY_SAMPLE_RATE),
                        outChannel, mOutFormat.getSampleRate()));
            } else {
                Logger.d("[AudioMaster] Target format not yet set — will be skipped until set");
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
                Logger.d("Format changed to " + format);
                // Recreate resampler with updated source format to avoid pitch/speed drift
                if (mOutFormat != null) {
                    int effectiveSrcCh = Math.min(
                            format.getInteger(MediaFormat.KEY_CHANNEL_COUNT),
                            mOutFormat.getChannelCount());
                    ResamplerChannel inChannel = effectiveSrcCh == 1
                            ? ResamplerChannel.MONO : ResamplerChannel.STEREO;
                    ResamplerChannel outChannel = mOutFormat.getChannelCount() == 1
                            ? ResamplerChannel.MONO : ResamplerChannel.STEREO;
                    mResampler = new Resampler(new ResamplerConfiguration(
                            ResamplerQuality.BEST, inChannel,
                            format.getInteger(MediaFormat.KEY_SAMPLE_RATE),
                            outChannel, mOutFormat.getSampleRate()));
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
        mOutFormat = null;
        mResampler = null;

        Logger.d("[AudioMaster] Loading done — raw PCM decoded: " + totalPcmBytes + " bytes");
        mIsLoading = false;
        onLoadDone();
    }

    private void processInBuffer(MediaFormat source, byte[] bufferChunk) {
        if (bufferChunk.length == 0) {
            return;
        }

        // Guard: if no output format has been set yet, skip silently.
        if (mOutFormat == null || mResampler == null) {
            Logger.d("processInBuffer skipped: output format or resampler not yet set");
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
    }

    public void setFormat(int sampleRate, int channelMask, int encoding) {
        mOutFormat = new AudioFormat.Builder()
                .setSampleRate(sampleRate)
                .setChannelMask(channelMask)
                .setEncoding(encoding)
                .setSampleRate(sampleRate)
                .build();
    }

    public AudioFormat getFormat() {
        return mOutFormat;
    }

    public native void onBufferChunkLoaded(byte[] bufferChunk);

    public native void onLoadDone();
}
