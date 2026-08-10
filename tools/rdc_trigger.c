// LD_PRELOAD companion for headless RenderDoc GL captures - no windowing system to send the
// default F12 hotkey to, and no pyrenderdoc target-control setup in this environment, so this
// calls the in-application C API directly instead. On load, waits a few seconds (long enough for
// the target's GL context and first frames to exist), then triggers one capture.
//
// Build:   gcc -shared -fPIC -o rdc_trigger.so rdc_trigger.c -I/opt/renderdoc/include -ldl -lpthread
// Use:     env -u WAYLAND_DISPLAY DISPLAY=:0 XDG_SESSION_TYPE=x11 \
//            LD_PRELOAD="/opt/renderdoc/lib/librenderdoc.so:/path/to/rdc_trigger.so" \
//            ./your_gl_app [args]
//
// Renderdoc's official Linux build only supports xlib/XCB windowing, so GLX-via-XWayland is the
// only path that opens a capturable GL context here. XDG_SESSION_TYPE must be forced to "x11" -
// GLFWOpenGLDriver otherwise picks the Wayland/EGL backend whenever the session type says
// "wayland", even with DISPLAY set, and RenderDoc's hooks never see a GLX context to capture.
#define _GNU_SOURCE
#include <stdbool.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "renderdoc_app.h"

static void *trigger_thread(void *arg) {
    (void) arg;
    const char *delay_env = getenv("RDC_TRIGGER_DELAY_SEC");
    sleep(delay_env ? (unsigned) atoi(delay_env) : 4);

    void *mod = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
    if (!mod) {
        fprintf(stderr, "[rdc_trigger] librenderdoc.so not loaded yet\n");
        return NULL;
    }

    pRENDERDOC_GetAPI RENDERDOC_GetAPI = (pRENDERDOC_GetAPI) dlsym(mod, "RENDERDOC_GetAPI");
    if (!RENDERDOC_GetAPI) {
        fprintf(stderr, "[rdc_trigger] no RENDERDOC_GetAPI symbol\n");
        return NULL;
    }

    RENDERDOC_API_1_6_0 *rdoc = NULL;
    int ok = RENDERDOC_GetAPI(eRENDERDOC_API_Version_1_6_0, (void **) &rdoc);
    if (!ok || !rdoc) {
        fprintf(stderr, "[rdc_trigger] RENDERDOC_GetAPI failed\n");
        return NULL;
    }

    const char *capfile = getenv("RDC_TRIGGER_CAPFILE");
    if (capfile) {
        rdoc->SetCaptureFilePathTemplate(capfile);
    }
    fprintf(stderr, "[rdc_trigger] path template: %s\n", rdoc->GetCaptureFilePathTemplate());
    fprintf(stderr, "[rdc_trigger] IsFrameCapturing before trigger: %u\n", rdoc->IsFrameCapturing());
    fprintf(stderr, "[rdc_trigger] triggering capture now\n");
    rdoc->TriggerCapture();
    for (int i = 0; i < 10; i++) {
        usleep(200000);
        fprintf(stderr, "[rdc_trigger] tick %d IsFrameCapturing=%u NumCaptures=%d\n", i, rdoc->IsFrameCapturing(),
                rdoc->GetNumCaptures());
    }
    fprintf(stderr, "[rdc_trigger] done, num captures=%d\n", rdoc->GetNumCaptures());
    if (rdoc->GetNumCaptures() > 0) {
        char pathbuf[1024] = {0};
        uint32_t pathlen = sizeof(pathbuf);
        uint64_t ts = 0;
        rdoc->GetCapture(0, pathbuf, &pathlen, &ts);
        fprintf(stderr, "[rdc_trigger] capture 0 path: %s\n", pathbuf);
    }
    return NULL;
}

__attribute__((constructor))
static void rdc_trigger_init(void) {
    pthread_t t;
    pthread_create(&t, NULL, trigger_thread, NULL);
    pthread_detach(t);
}
