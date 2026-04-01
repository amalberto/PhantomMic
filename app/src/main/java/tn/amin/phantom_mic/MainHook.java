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
            loadNativeLibrary(lpparam);

            Logger.d("Beginning hook");
            doHook(lpparam);
            Logger.d("Successful hook");
        }
    }

    private void loadNativeLibrary(XC_LoadPackage.LoadPackageParam lpparam) {
        // LspModuleClassLoader only adds base.apk!/lib/arm64-v8a to its native library
        // search path — it does NOT add the extracted lib dir even when
        // extractNativeLibs="true". So System.loadLibrary() fails with "couldn't find".
        // Fix: derive the extracted path from the module APK path via the classloader
        // string representation and use System.load() with the absolute path.
        try {
            System.loadLibrary("xposedlab");
            Logger.d("loadLibrary xposedlab OK (APK path)");
            return;
        } catch (UnsatisfiedLinkError ignored) {}

        // LspModuleClassLoader.toString() contains "module=/data/app/.../base.apk"
        // Extracted libs live at /data/app/.../<pkg>-<hash>/lib/arm64/
        try {
            String clStr = lpparam.classLoader.toString();
            int start = clStr.indexOf("/data/app/");
            int end = clStr.indexOf("/base.apk");
            if (start >= 0 && end > start) {
                String pkgDir = clStr.substring(start, end);
                String abi = Build.SUPPORTED_64_BIT_ABIS.length > 0 ? "arm64" : "arm";
                File libFile = new File(pkgDir + "/lib/" + abi + "/libxposedlab.so");
                Logger.d("Trying extracted path: " + libFile.getAbsolutePath() + " exists=" + libFile.exists());
                if (libFile.exists()) {
                    System.load(libFile.getAbsolutePath());
                    Logger.d("System.load xposedlab OK (extracted path)");
                    return;
                }
            }
        } catch (UnsatisfiedLinkError | Exception e) {
            Logger.d("Extracted load failed: " + e.getMessage());
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
