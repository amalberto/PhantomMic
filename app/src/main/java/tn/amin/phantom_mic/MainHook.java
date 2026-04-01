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
        // LSPosed classloader uses base.apk!/lib/arm64-v8a (compressed) which Android 16
        // blocks for W^X. The APK installer extracts .so files to disk (extractNativeLibs=true).
        // We parse the apk path from the classloader string and derive {apkDir}/lib/arm64/.
        String apkParentDir = null;
        try {
            String clStr = MainHook.class.getClassLoader().toString();
            java.util.regex.Matcher m = java.util.regex.Pattern
                    .compile("module=([^,\\]]+/base\\.apk)")
                    .matcher(clStr);
            if (m.find()) {
                apkParentDir = new File(m.group(1)).getParent();
                Logger.d("APK parent dir: " + apkParentDir);
            }
        } catch (Throwable t) {
            Logger.d("loadNativeLibrary: parse failed: " + t);
        }

        for (String libName : new String[]{"xposedlab", "androidresampler"}) {
            loadSoFromDisk(libName, apkParentDir);
        }
    }

    private void loadSoFromDisk(String libName, String apkParentDir) {
        // 1. Try extracted path derived from APK dir
        if (apkParentDir != null) {
            String primaryAbi = android.os.Build.SUPPORTED_ABIS[0];
            String canonAbi = primaryAbi.contains("64") ? "arm64" : "arm";
            for (String abiDir : new String[]{canonAbi, primaryAbi, "arm64", "arm64-v8a"}) {
                File soFile = new File(apkParentDir + "/lib/" + abiDir + "/lib" + libName + ".so");
                if (soFile.exists()) {
                    try {
                        System.load(soFile.getAbsolutePath());
                        Logger.d("System.load " + libName + " OK (" + abiDir + ")");
                        return;
                    } catch (Throwable t) {
                        Logger.d("System.load " + libName + " failed (" + abiDir + "): " + t);
                    }
                }
            }
        }
        // 2. Fallback: standard loadLibrary (works on older Android / non-LSPosed)
        try {
            System.loadLibrary(libName);
            Logger.d("loadLibrary " + libName + " OK");
        } catch (Throwable t) {
            Logger.d("ERROR: could not load lib" + libName + ".so: " + t);
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
