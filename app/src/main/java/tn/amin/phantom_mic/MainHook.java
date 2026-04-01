package tn.amin.phantom_mic;

import android.app.Activity;
import android.app.Application;
import android.content.Context;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.net.Uri;
import android.os.Bundle;
import android.os.PersistableBundle;

import java.nio.ByteBuffer;

import de.robv.android.xposed.IXposedHookLoadPackage;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import de.robv.android.xposed.XposedHelpers;
import de.robv.android.xposed.callbacks.XC_LoadPackage;
import tn.amin.phantom_mic.log.Logger;

public class MainHook implements IXposedHookLoadPackage {
    private PhantomManager phantomManager = null;

    private boolean needHook = true;

    private String packageName;

    private int accBytes = 0;

    @Override
    public void handleLoadPackage(XC_LoadPackage.LoadPackageParam lpparam) {
        if (needHook) {
            needHook = false;

            packageName = lpparam.packageName;
            System.loadLibrary("xposedlab");

            Logger.d("Beginning hook");
            doHook(lpparam);
            Logger.d("Successful hook");
        }
    }

    private void doHook(XC_LoadPackage.LoadPackageParam lpparam) {
        XposedHelpers.findAndHookMethod("android.media.MediaRecorder", lpparam.classLoader, "start" , new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam param) throws Throwable {
                Logger.d("MediaRecorder start");
            }
        });

        XposedHelpers.findAndHookMethod("android.media.AudioRecord", lpparam.classLoader, "startRecording", new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam param) throws Throwable {
                Logger.d("AudioRecord start");
            }
        });

        hookActivityLifecycle(lpparam);

        XposedHelpers.findAndHookMethod("android.app.Instrumentation", lpparam.classLoader, "callApplicationOnCreate", Application.class, new XC_MethodHook() {
            @Override
            protected void afterHookedMethod(MethodHookParam param) throws Throwable {
                Application application = (Application) param.args[0];
                if (phantomManager == null) {
                    initPhantomManager(application.getApplicationContext());
                }
            }
        });
    }

    /**
     * Hook the Activity lifecycle to obtain an Activity instance for prepare().
     * Tries performCreate(Bundle, PersistableBundle) first (API 21+), then
     * performCreate(Bundle) (older), then falls back to
     * Instrumentation.callActivityOnCreate which is stable across all versions.
     */
    private void hookActivityLifecycle(XC_LoadPackage.LoadPackageParam lpparam) {
        XC_MethodHook activityCreateHook = new XC_MethodHook() {
            @Override
            protected void beforeHookedMethod(MethodHookParam param) throws Throwable {
                if (phantomManager != null) {
                    Activity activity = (Activity) param.thisObject;
                    phantomManager.interceptIntent(activity.getIntent());
                }
            }

            @Override
            protected void afterHookedMethod(MethodHookParam param) throws Throwable {
                Activity activity = (Activity) param.thisObject;
                onActivityObtained(activity);
            }
        };

        boolean hooked = false;

        try {
            XposedHelpers.findAndHookMethod(Activity.class, "performCreate",
                    Bundle.class, PersistableBundle.class, activityCreateHook);
            hooked = true;
            Logger.d("Hooked Activity#performCreate(Bundle, PersistableBundle)");
        } catch (Throwable t) {
            Logger.d("performCreate(Bundle,PersistableBundle) not available: " + t.getMessage());
        }

        if (!hooked) {
            try {
                XposedHelpers.findAndHookMethod(Activity.class, "performCreate",
                        Bundle.class, activityCreateHook);
                hooked = true;
                Logger.d("Hooked Activity#performCreate(Bundle)");
            } catch (Throwable t) {
                Logger.d("performCreate(Bundle) not available: " + t.getMessage());
            }
        }

        if (!hooked) {
            // Stable fallback present in all Android versions
            XposedHelpers.findAndHookMethod("android.app.Instrumentation", lpparam.classLoader,
                    "callActivityOnCreate", Activity.class, Bundle.class, new XC_MethodHook() {
                        @Override
                        protected void afterHookedMethod(MethodHookParam param) throws Throwable {
                            Activity activity = (Activity) param.args[0];
                            onActivityObtained(activity);
                        }
                    });
            Logger.d("Hooked Instrumentation#callActivityOnCreate as fallback");
        }
    }

    private void onActivityObtained(Activity activity) {
        if (phantomManager == null) {
            initPhantomManager(activity.getApplicationContext());
        }

        if (phantomManager.needPrepare()) {
            phantomManager.prepare(activity);
        }
    }

    private void initPhantomManager(Context context) {
        phantomManager = new PhantomManager(context, isNativeHook());
        // Pre-initialise with a safe PCM default; the native set() hook will
        // override this with the real format once AudioRecord::set() fires.
        // This mirrors WhatsMicFix's permissive-mode fallback and avoids a
        // null-format crash on the first obtainBuffer() call.
        phantomManager.updateAudioFormat(
                48000,
                AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT);
        Logger.d("Pre-initialised PCM default: 48000 Hz / MONO / PCM_16BIT");

        if (isSpecialCase()) {
            phantomManager.forceUriPath();
        }
    }

    private boolean isSpecialCase() {
        return packageName.equals("com.whatsapp")
                || packageName.equals("com.android.soundrecorder");
    }

    public boolean isNativeHook() {
        return true;
    }
}
