#include <switch.h>

extern "C" void randomGet(void* out, size_t out_size);

extern "C" Result csrngGetRandomBytes(void* out, size_t out_size) {
    // Fallback implementation using libnx randomGet.
    // This provides cryptographically secure random bytes on Horizon OS.
    randomGet(out, out_size);
    return 0; // Result 0 == success
}
