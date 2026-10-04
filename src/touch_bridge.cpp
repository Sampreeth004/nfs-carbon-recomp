/**
 * @file        touch_bridge.cpp
 * @brief       JNI bridge: Android TouchControlsView -> rexruntime touch driver.
 */

#include <jni.h>

#include <cstdint>

// Defined in librexruntime.so (rex/input/touchpad/touchpad_input_driver.cpp).
extern "C" {
void rex_input_set_touch_pad(uint16_t buttons, float lstick_x, float lstick_y, float rstick_x,
                             float rstick_y, uint8_t left_trigger, uint8_t right_trigger);
void rex_input_set_touch_pad_enabled(int enabled);
}

// Defined in librexruntime.so (src/system/runtime.cpp); reported by the xenos
// plugin, shown by the app's fps counter.
extern "C" {
float rex_gpu_get_fps();
float rex_gpu_get_frame_ms();
float rex_gpu_get_worst_ms();
}

namespace {

inline uint8_t TriggerToByte(float value) {
  if (value <= 0.0f) {
    return 0;
  }
  if (value >= 1.0f) {
    return 0xFF;
  }
  return static_cast<uint8_t>(value * 255.0f + 0.5f);
}

}  // namespace

extern "C" JNIEXPORT void JNICALL Java_com_eagames_nfscarbon_GameBridge_nativeSetTouchPad(
    JNIEnv*, jclass, jint buttons, jfloat lstick_x, jfloat lstick_y, jfloat rstick_x,
    jfloat rstick_y, jfloat left_trigger, jfloat right_trigger) {
  rex_input_set_touch_pad(static_cast<uint16_t>(buttons), lstick_x, lstick_y, rstick_x, rstick_y,
                          TriggerToByte(left_trigger), TriggerToByte(right_trigger));
}

extern "C" JNIEXPORT void JNICALL Java_com_eagames_nfscarbon_GameBridge_nativeSetTouchPadEnabled(
    JNIEnv*, jclass, jboolean enabled) {
  rex_input_set_touch_pad_enabled(enabled ? 1 : 0);
}

extern "C" JNIEXPORT jfloat JNICALL
Java_com_eagames_nfscarbon_GameBridge_nativeGetGuestFps(JNIEnv*, jclass) {
  return rex_gpu_get_fps();
}

extern "C" JNIEXPORT jfloat JNICALL
Java_com_eagames_nfscarbon_GameBridge_nativeGetGuestFrameMs(JNIEnv*, jclass) {
  return rex_gpu_get_frame_ms();
}

extern "C" JNIEXPORT jfloat JNICALL
Java_com_eagames_nfscarbon_GameBridge_nativeGetGuestWorstMs(JNIEnv*, jclass) {
  return rex_gpu_get_worst_ms();
}
