package tn.amin.phantom_mic;

import android.content.Context;
import android.content.SharedPreferences;
import android.net.Uri;

import java.lang.ref.WeakReference;

public class SPManager {
    private final SharedPreferences sp;

    SPManager(Context context) {
        sp = context.getSharedPreferences("phantom_mic", Context.MODE_PRIVATE);
    }

    public Uri getUriPath() {
        String path =  sp.getString("recordings_path", null);
        if (path == null) {
            return null;
        }

        return Uri.parse(path);
    }

    public void setUriPath(Uri uri) {
        sp.edit().putString("recordings_path", uri.toString()).apply();
    }

    /** Persist the last-known AudioRecord format so PhantomMic can pre-load
     *  on the next app start before ctor_hook fires. */
    public void saveAudioFormat(int sampleRate, int encoding, int channelMask) {
        sp.edit()
                .putInt("last_sample_rate", sampleRate)
                .putInt("last_encoding", encoding)
                .putInt("last_channel_mask", channelMask)
                .apply();
    }

    public boolean hasSavedAudioFormat() {
        return sp.contains("last_sample_rate");
    }

    public int getSavedSampleRate()  { return sp.getInt("last_sample_rate", 16000); }
    public int getSavedEncoding()    { return sp.getInt("last_encoding",     2);     } // ENCODING_PCM_16BIT
    public int getSavedChannelMask() { return sp.getInt("last_channel_mask", 16);    } // CHANNEL_IN_MONO
}
