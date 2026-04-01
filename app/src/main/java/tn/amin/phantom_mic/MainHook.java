package tn.amin.phantom_mic;

import android.app.Activity;
import android.app.Application;
import android.content.Context;
import android.media.AudioRecord;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.PersistableBundle;

import java.io.File;

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
            loadNativeLibrary();

            Logger.d("Beginning hook");
            doHook(lpparam);
            Logger.d("Successful hook");
        }
    }

    private void loadNativeLibrary() {
        // The .so IS extracted to disk by the installer (extractNativeLibs=true).
        // Path: {apkDir}/lib/arm64/libxposedlab.so
        // LSPosed's classloader string contains: module=/data/app/.../{pkg}-hash/base.apk
        // We parse that to derive the parent dir without needing directory listing (no perms).
        try {
            String clStr = MainHook.class.getClassLoader().toString();
            Logger.d("ClassLoader: " + clStr.substring(0, Math.min(300, clStr.length())));

            java.util.regex.Matcher m = java.util.regex.Pattern
                    .compile("module=([^,\\]]+/base\\.apk)")
                    .matcher(clStr);
            if (m.find()) {
                String apkPath = m.group(1);
                File appDir = new File(apkPath).getParentFile();
                Logger.d("APK parent dir: " + appDir);

                String primaryAbi = android.os.Build.SUPPORTED_ABIS[0]; // e.g. arm64-v8a
                String canonAbi = primaryAbi.contains("64") ? "arm64" : "arm";

                for (String abiDir : new String[]{canonAbi, primaryAbi, "arm64", "arm64-v8a"}) {
                    File soFile = new File(appDir, "lib/" + abiDir + "/libxposedlab.so");
                    Logger.d("Trying: " + soFile.getAbsolutePath() + " exists=" + soFile.exists());
                    if (soFile.exists()) {
                        System.load(soFile.getAbsolutePath());
                        Logger.d("System.load xposedlab OK (" + abiDir + ")");
                        return;
                    }
                }
                Logger.d("No .so found under " + appDir + "/lib/");
            } else {
                Logger.d("Could not parse APK path from classloader string");
            }
        } catch (Throwable t) {
            Logger.d("loadNativeLibrary failed: " + t);
        }

        Logger.d("ERROR: could not load libxposedlab.so");
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
