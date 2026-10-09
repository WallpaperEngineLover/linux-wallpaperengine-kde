// Functions that bundled libraries of the portable release call but older systems don't have.
//
// glibc: libraries retargeted by retarget_glibc_versions.py can't bind to libc's own copy and resolve to this
// unversioned one instead (see the script).
//
// Others are forwarded to the system's own copy when it has one. Libraries referencing them get this one as a
// dependency (package_release.sh).
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/random.h>

// glibc 2.36
void arc4random_buf (void* buffer, size_t size) {
    unsigned char* out = buffer;

    while (size > 0) {
        ssize_t got = getrandom (out, size, 0);

        if (got < 0) {
            if (errno == EINTR)
                continue;
            // no fallback: getrandom exists since Linux 3.17
            __builtin_trap ();
        }

        out += got;
        size -= got;
    }
}

uint32_t arc4random (void) {
    uint32_t value;

    arc4random_buf (&value, sizeof (value));
    return value;
}

uint32_t arc4random_uniform (uint32_t upper) {
    if (upper < 2)
        return 0;

    // rejection sampling to avoid the modulo bias
    uint32_t min = -upper % upper;
    uint32_t value;

    do {
        value = arc4random ();
    } while (value < min);

    return value % upper;
}

// libpipewire 0.3.50, called by mpv (Ubuntu 22.04 has 0.3.48). The older call fills the start of the same struct
int pw_stream_get_time_n (void* stream, void* time, size_t size) {
    static int (*own) (void*, void*, size_t);
    static int (*old) (void*, void*);

    if (!own && !old) {
        own = dlsym (RTLD_NEXT, "pw_stream_get_time_n");
        old = dlsym (RTLD_DEFAULT, "pw_stream_get_time");
    }

    if (own)
        return own (stream, time, size);
    if (!old)
        return -ENOSYS;

    memset (time, 0, size);
    return old (stream, time);
}
