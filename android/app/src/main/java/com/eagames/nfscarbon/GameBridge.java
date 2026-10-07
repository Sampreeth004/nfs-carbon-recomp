package com.eagames.nfscarbon;

public final class GameBridge {
    private static boolean nativeAvailable = false;

    static {
        try {
            System.loadLibrary("nfscarbon");
            nativeAvailable = true;
        } catch (Throwable ignored) {
            nativeAvailable = false;
        }
    }

    private GameBridge() {
    }

    public static boolean isNativeAvailable() {
        return nativeAvailable;
    }

    public static native void nativeSetTouchPad(int buttons, float lstickX, float lstickY,
                                                float rstickX, float rstickY,
                                                float leftTrigger, float rightTrigger);

    public static native void nativeSetTouchPadEnabled(boolean enabled);

    public static native float nativeGetGuestFps();

    public static native float nativeGetGuestFrameMs();

    public static native float nativeGetGuestWorstMs();

    public static native void nativeSetGamePaused(boolean paused);

    public static void setGamePaused(boolean paused) {
        if (!nativeAvailable) {
            return;
        }
        try {
            nativeSetGamePaused(paused);
        } catch (UnsatisfiedLinkError ignored) {
        }
    }

    public static void setTouchPad(int buttons, float lstickX, float lstickY,
                                   float rstickX, float rstickY,
                                   float leftTrigger, float rightTrigger) {
        if (!nativeAvailable) {
            return;
        }
        nativeSetTouchPad(buttons, lstickX, lstickY, rstickX, rstickY,
                leftTrigger, rightTrigger);
    }

    public static void setTouchPadEnabled(boolean enabled) {
        if (!nativeAvailable) {
            return;
        }
        nativeSetTouchPadEnabled(enabled);
    }

    public static float getGuestFps() {
        return nativeAvailable ? nativeGetGuestFps() : 0f;
    }

    public static float getGuestFrameMs() {
        return nativeAvailable ? nativeGetGuestFrameMs() : 0f;
    }

    public static float getGuestWorstMs() {
        return nativeAvailable ? nativeGetGuestWorstMs() : 0f;
    }
}
