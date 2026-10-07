/**
 * @file        turnip_driver.cpp
 * @brief       Opens the system Vulkan loader with a downloaded ICD driver (for example
 *              a turnip build) injected, using libadrenotools (BSD-2-Clause, Billy Laws).
 *
 * The SDK's VulkanInstance calls the registered hook when the `vulkan_icd_driver` cvar
 * is set. Adreno GPUs only; the hook libraries must sit in the app's native library dir.
 */

#include <android/log.h>
#include <dlfcn.h>
#include <sys/stat.h>

#include <string>

#include <adrenotools/driver.h>

extern "C" void rex_ui_set_vulkan_icd_hook(void* (*hook)(const char* driver_path));

namespace {

constexpr const char* kTag = "nfscarbon-driver";

void* OpenIcdDriver(const char* driver_path) {
  const std::string path(driver_path ? driver_path : "");
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos || slash + 1 >= path.size()) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "Bad driver path: %s", path.c_str());
    return nullptr;
  }
  const std::string driver_dir = path.substr(0, slash + 1);
  const std::string driver_name = path.substr(slash + 1);

  // With legacy packaging this library is extracted next to the adrenotools hook libraries.
  Dl_info info{};
  if (!dladdr(reinterpret_cast<void*>(&OpenIcdDriver), &info) || !info.dli_fname) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "Cannot locate the native library directory");
    return nullptr;
  }
  const std::string self(info.dli_fname);
  const size_t self_slash = self.find_last_of('/');
  if (self_slash == std::string::npos) {
    return nullptr;
  }
  const std::string hook_dir = self.substr(0, self_slash + 1);

  const std::string tmp_dir = driver_dir + "tmp/";
  mkdir(tmp_dir.c_str(), 0700);

  void* handle = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM,
                                            tmp_dir.c_str(), hook_dir.c_str(), driver_dir.c_str(),
                                            driver_name.c_str(), nullptr, nullptr);
  if (!handle) {
    __android_log_print(ANDROID_LOG_WARN, kTag,
                        "adrenotools could not open %s (hooks in %s); is this an Adreno GPU?",
                        path.c_str(), hook_dir.c_str());
  } else {
    __android_log_print(ANDROID_LOG_INFO, kTag, "Opened Vulkan with custom driver %s",
                        path.c_str());
  }
  return handle;
}

struct IcdHookRegistration {
  IcdHookRegistration() { rex_ui_set_vulkan_icd_hook(&OpenIcdDriver); }
};
const IcdHookRegistration g_registration;

}  // namespace
