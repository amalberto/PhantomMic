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
        // 1. Standard path — works if LSPosed configures the native lib dir correctly.
        try {
            System.loadLibrary("xposedlab");
            Logger.d("loadLibrary xposedlab OK (standard)");
            return;
        } catch (UnsatisfiedLinkError ignored) {}

        // 2. Derive the module's own APK path from MainHook's code source.
        //    lpparam.classLoader is the TARGET APP's classloader (WhatsApp), not ours.
        //    MainHook.class.getProtectionDomain().getCodeSource() gives PhantomMic's APK.
        try {
            java.security.CodeSource cs = MainHook.class.getProtectionDomain().getCodeSource();
            if (cs != null) {
                String apkPath = cs.getLocation().getPath(); // .../tn.amin.phantom_mic-.../base.apk
                String dir = apkPath.substring(0, apkPath.lastIndexOf('/'));
                for (String abi : new String[]{"arm64-v8a", "arm64", "armeabi-v7a"}) {
                    File soFile = new File(dir + "/lib/" + abi + "/libxposedlab.so");
                    Logger.d("Trying: " + soFile.getAbsolutePath() + " exists=" + soFile.exists());
                    if (soFile.exists()) {
                        System.load(soFile.getAbsolutePath());
                        Logger.d("System.load xposedlab OK (" + abi + ")");
                        return;
                    }
                }
            }
        } catch (UnsatisfiedLinkError | Exception e) {
            Logger.d("CodeSource load failed: " + e.getMessage());
        }

        // 3. Last resort: scan /data/app/ for the module package directory.
        try {
            File appDir = new File("/data/app/");
            File[] dirs1 = appDir.listFiles();
            if (dirs1 != null) {
                for (File d1 : dirs1) {
                    File[] dirs2 = d1.listFiles();
                    if (dirs2 == null) continue;
                    for (File d2 : dirs2) {
                        if (!d2.getName().startsWith("tn.amin.phantom_mic")) continue;
                        for (String abi : new String[]{"arm64-v8a", "arm64"}) {
                            File soFile = new File(d2, "lib/" + abi + "/libxposedlab.so");
                            if (soFile.exists()) {
                                System.load(soFile.getAbsolutePath());
                                Logger.d("System.load xposedlab OK (scan: " + soFile + ")");
                                return;
                            }
                        }
                    }
                }
            }
        } catch (UnsatisfiedLinkError | Exception e) {
            Logger.d("Scan load failed: " + e.getMessage());
        }

        Logger.d("ERROR: could not load libxposedlab.so from any path");
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
