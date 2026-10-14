// LD_PRELOAD shim for headless runs without a D-Bus session bus: the MPRIS player retry calls dbus_connection_send_with_reply_and_block() on a NULL connection,
// which aborts inside libdbus; this returns NULL like a failed call instead
// build: gcc -shared -fPIC -o dbus_noop_shim.so dbus_noop_shim.c -ldl
// use: LD_PRELOAD=/path/to/dbus_noop_shim.so ./linux-wallpaperengine ...
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
