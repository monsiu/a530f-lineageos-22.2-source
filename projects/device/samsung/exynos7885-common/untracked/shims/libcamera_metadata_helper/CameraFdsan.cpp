#include <android/fdsan.h>

// The Samsung camera HAL closes an fd owned by a unique_fd. Keep the proven
// warning behavior inside processes that load this camera-only compatibility
// library, instead of weakening fdsan for the entire system with debug.fdsan.
__attribute__((constructor)) static void set_camera_fdsan_level() {
    if (android_fdsan_set_error_level != nullptr) {
        android_fdsan_set_error_level(ANDROID_FDSAN_ERROR_LEVEL_WARN_ONCE);
    }
}
