// LD_PRELOAD shim for headless test environments with no D-Bus session bus (no dbus-daemon,
// no /etc/machine-id, no permission to create one - e.g. a minimal container).
//
// The engine already treats a failed dbus_bus_get() as non-fatal in most places (returns
// nullptr, callers null-check), but at least one call path (a periodic MPRIS "detect player"
// retry) calls dbus_connection_send_with_reply_and_block() without checking for a null
// connection first, which aborts the whole process instead of just skipping that feature.
// This shim intercepts that one libdbus entry point: if the connection is NULL, return NULL
// (which is exactly what the real function would do on failure) instead of hitting libdbus's
// own internal assertion and calling abort().
//
// Build:   gcc -shared -fPIC -o dbus_noop_shim.so dbus_noop_shim.c -ldl
// Use:     LD_PRELOAD=/path/to/dbus_noop_shim.so ./linux-wallpaperengine ...
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>

typedef struct DBusConnection DBusConnection;
typedef struct DBusMessage DBusMessage;
typedef struct DBusError DBusError;

typedef DBusMessage *(*real_fn_t)(DBusConnection *, DBusMessage *, int, DBusError *);

DBusMessage *dbus_connection_send_with_reply_and_block(DBusConnection *connection, DBusMessage *message,
                                                          int timeout_milliseconds, DBusError *error) {
    if (connection == NULL) {
        fprintf(stderr, "[dbus_noop_shim] blocked send_with_reply_and_block on NULL connection\n");
        return NULL;
    }
    static real_fn_t real = NULL;
    if (!real) {
        real = (real_fn_t) dlsym(RTLD_NEXT, "dbus_connection_send_with_reply_and_block");
    }
    return real(connection, message, timeout_milliseconds, error);
}
