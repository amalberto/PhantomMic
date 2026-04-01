package tn.amin.phantom_mic.audio;

/**
 * Pure-Java linear interpolation resampler for PCM_16BIT audio.
 * No native library required — replaces android-resampler (nailik) which
 * requires libandroidresampler.so that cannot be loaded by LspModuleClassLoader
 * on Android 16 due to W^X extraction restrictions.
 *
 * Input and output are always PCM_16BIT little-endian byte arrays.
 * Channels are interleaved (L0 R0 L1 R1 … for stereo). Channel count must
 * be the same for input and output (downmix should be done before calling).
 *
 * State (fractional position) is preserved across calls so chunk boundaries
 * produce no clicks or artefacts.
 */
public class LinearResampler {
    private final int mSrcRate;
    private final int mDstRate;
    private final int mChannels;

    // Fractional read position into the source stream, in fixed-point 32.32.
    // We use a double to avoid 64-bit integer overflow while still getting
    // sub-sample precision well beyond 16-bit audio quality.
    private double mPhase = 0.0;

    // Last sample from the previous chunk — needed for interpolation at the
    // first output sample when mPhase < 1.0.
    private short[] mPrevSample;

    public LinearResampler(int srcSampleRate, int channels, int dstSampleRate) {
        mSrcRate  = srcSampleRate;
        mDstRate  = dstSampleRate;
        mChannels = channels;
        mPrevSample = new short[channels];
    }

    /**
     * Resample a chunk of PCM_16BIT audio.
     *
     * @param input  raw PCM bytes (little-endian int16, interleaved channels)
     * @return       resampled PCM bytes at the target sample rate
     */
    public byte[] resample(byte[] input) {
        if (mSrcRate == mDstRate) return input;

        int bytesPerFrame = mChannels * 2;
        int srcFrames = input.length / bytesPerFrame;
        if (srcFrames == 0) return new byte[0];

        // Decode source to short[][channels][frames]
        short[][] src = new short[mChannels][srcFrames];
        for (int f = 0; f < srcFrames; f++) {
            for (int c = 0; c < mChannels; c++) {
                int off = f * bytesPerFrame + c * 2;
                src[c][f] = (short) ((input[off + 1] << 8) | (input[off] & 0xFF));
            }
        }

        // Number of output frames for this chunk
        // We advance mPhase by (srcRate/dstRate) per output frame.
        double ratio = (double) mSrcRate / mDstRate;
        // Output frames = ceil((srcFrames - mPhase) / ratio)
        int dstFrames = (int) Math.ceil((srcFrames - mPhase) / ratio);
        if (dstFrames <= 0) {
            // Edge case: not enough src to produce any output — save last sample
            if (srcFrames > 0) {
                for (int c = 0; c < mChannels; c++) mPrevSample[c] = src[c][srcFrames - 1];
            }
            mPhase -= srcFrames;
            return new byte[0];
        }

        short[][] dst = new short[mChannels][dstFrames];
        double phase = mPhase;

        for (int of = 0; of < dstFrames; of++) {
            int i0 = (int) phase;
            double frac = phase - i0;

            for (int c = 0; c < mChannels; c++) {
                short s0 = (i0 == 0 && phase < 1.0) ? mPrevSample[c]
                         : (i0 < srcFrames)         ? src[c][i0]
                         :                            src[c][srcFrames - 1];
                short s1 = (i0 + 1 < srcFrames) ? src[c][i0 + 1]
                         : src[c][srcFrames - 1];
                dst[c][of] = (short) Math.round(s0 + frac * (s1 - s0));
            }
            phase += ratio;
        }

        // Save last source sample for next chunk's interpolation
        for (int c = 0; c < mChannels; c++) mPrevSample[c] = src[c][srcFrames - 1];
        // Carry over fractional phase minus the frames we consumed
        mPhase = phase - srcFrames;

        // Encode output to bytes
        byte[] out = new byte[dstFrames * bytesPerFrame];
        for (int f = 0; f < dstFrames; f++) {
            for (int c = 0; c < mChannels; c++) {
                int off = f * bytesPerFrame + c * 2;
                short s = dst[c][f];
                out[off]     = (byte) (s & 0xFF);
                out[off + 1] = (byte) ((s >> 8) & 0xFF);
            }
        }
        return out;
    }
}
