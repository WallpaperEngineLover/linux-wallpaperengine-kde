// A stand-in for Corsair's CUESDK.x64_2017.dll (iCUE SDK 3.x C API) that logs what Wallpaper Engine's LED plugin
// (plugins/led/ledextensions64.dll) does, so real WE can be observed under Wine with no hardware.
//
// Devices (CUE_STUB_DIR picks where the logs go, default Z:\tmp\lwe-cuestub):
//   0: a "probe" keyboard with 80 x 10 LEDs of 1 x 1 mm on a grid. The plugin samples its 80x10 frame at every LED's
//      centre, so every flush gives that frame pixel for pixel (written as probe_<n>.ppm)
//   1: a K70 like keyboard, 22 x 6 keys of 18 mm on a 19 mm pitch
//   2: a mouse mat, 15 LEDs in a row
// Every call is written to calls.log with a time stamp.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define EXPORT __declspec (dllexport)

typedef struct {
    const char* sdkVersion;
    const char* serverVersion;
    int sdkProtocolVersion;
    int serverProtocolVersion;
    unsigned char breakingChanges;
} ProtocolDetails;

typedef struct {
    int type;
    const char* model;
    int physicalLayout;
    int logicalLayout;
    int capsMask;
    int ledsCount;
    struct {
	int channelsCount;
	void* channels;
    } channels;
    char deviceId[128];
} DeviceInfo;

typedef struct {
    int ledId;
    double top;
    double left;
    double height;
    double width;
} LedPosition;

typedef struct {
    int numberOfLed;
    LedPosition* pLedPosition;
} LedPositions;

typedef struct {
    int ledId;
    int r;
    int g;
    int b;
} LedColor;

#define DEVICES 3
static DeviceInfo g_info[DEVICES];
static LedPositions g_positions[DEVICES];
static LedColor* g_buffer[DEVICES];
static int g_bufferCount[DEVICES];
static char g_dir[MAX_PATH];
static FILE* g_log;
static int g_frame;
static int g_flushes;
static ULONGLONG g_start;
static int g_ready;

static void logf_ (const char* format, ...) {
    if (!g_log) {
	return;
    }

    va_list args;
    va_start (args, format);
    fprintf (g_log, "%8llu ", GetTickCount64 () - g_start);
    vfprintf (g_log, format, args);
    fputc ('\n', g_log);
    fflush (g_log);
    va_end (args);
}

static void setupDevice (int index, int type, const char* model, int cols, int rows, double pitch, double size) {
    const int count = cols * rows;
    LedPosition* positions = calloc (count, sizeof (LedPosition));

    for (int i = 0; i < count; i++) {
	positions[i].ledId = i + 1;
	positions[i].left = (i % cols) * pitch;
	positions[i].top = (i / cols) * pitch;
	positions[i].width = size;
	positions[i].height = size;
    }

    g_info[index].type = type;
    g_info[index].model = model;
    g_info[index].ledsCount = count;
    snprintf (g_info[index].deviceId, sizeof (g_info[index].deviceId), "stub-%d", index);
    g_positions[index].numberOfLed = count;
    g_positions[index].pLedPosition = positions;
    g_buffer[index] = calloc (count, sizeof (LedColor));
}

static void ensureReady (void) {
    if (g_ready) {
	return;
    }

    g_ready = 1;
    g_start = GetTickCount64 ();

    const char* dir = getenv ("CUE_STUB_DIR");
    snprintf (g_dir, sizeof (g_dir), "%s", dir && *dir ? dir : "Z:\\tmp\\lwe-cuestub");
    CreateDirectoryA (g_dir, NULL);

    char path[MAX_PATH + 32];
    snprintf (path, sizeof (path), "%s\\calls.log", g_dir);
    g_log = fopen (path, "a");

    setupDevice (0, 2, "Probe 80x10", 80, 10, 1.0, 1.0);
    setupDevice (1, 2, "K70 RGB", 22, 6, 19.0, 18.0);
    setupDevice (2, 4, "MM800", 15, 1, 20.0, 18.0);
    logf_ ("loaded");
}

static void writeProbe (void) {
    if (g_frame >= 400) {
	return;
    }

    char path[MAX_PATH + 40];
    snprintf (path, sizeof (path), "%s\\probe_%05d.ppm", g_dir, g_frame++);
    FILE* file = fopen (path, "wb");

    if (!file) {
	return;
    }

    fprintf (file, "P6\n80 10\n255\n");

    for (int i = 0; i < 800; i++) {
	const LedColor* color = &g_buffer[0][i];
	fputc (color->r, file);
	fputc (color->g, file);
	fputc (color->b, file);
    }

    fclose (file);
}

EXPORT ProtocolDetails CorsairPerformProtocolHandshake (void) {
    ensureReady ();
    ProtocolDetails details = { "3.0.0", "4.0.97", 7, 7, 0 };
    logf_ ("CorsairPerformProtocolHandshake");
    return details;
}

EXPORT unsigned char CorsairSubscribeForEvents (void* handler, void* context) {
    ensureReady ();
    logf_ ("CorsairSubscribeForEvents");
    return 1;
}

EXPORT unsigned char CorsairUnsubscribeFromEvents (void) {
    logf_ ("CorsairUnsubscribeFromEvents");
    return 1;
}

EXPORT unsigned char CorsairRequestControl (int mode) {
    ensureReady ();
    logf_ ("CorsairRequestControl %d", mode);
    return 1;
}

EXPORT unsigned char CorsairReleaseControl (int mode) {
    logf_ ("CorsairReleaseControl %d", mode);
    return 1;
}

EXPORT int CorsairGetDeviceCount (void) {
    ensureReady ();
    logf_ ("CorsairGetDeviceCount -> %d", DEVICES);
    return DEVICES;
}

EXPORT DeviceInfo* CorsairGetDeviceInfo (int index) {
    ensureReady ();
    logf_ ("CorsairGetDeviceInfo %d", index);
    return index >= 0 && index < DEVICES ? &g_info[index] : NULL;
}

EXPORT LedPositions* CorsairGetLedPositionsByDeviceIndex (int index) {
    ensureReady ();
    logf_ ("CorsairGetLedPositionsByDeviceIndex %d", index);
    return index >= 0 && index < DEVICES ? &g_positions[index] : NULL;
}

EXPORT unsigned char CorsairSetLedsColorsBufferByDeviceIndex (int index, int size, LedColor* colors) {
    if (index < 0 || index >= DEVICES) {
	return 0;
    }

    if (size > g_info[index].ledsCount) {
	size = g_info[index].ledsCount;
    }

    memcpy (g_buffer[index], colors, size * sizeof (LedColor));
    g_bufferCount[index] = size;
    return 1;
}

EXPORT unsigned char CorsairSetLedsColorsFlushBuffer (void) {
    g_flushes++;

    if (g_bufferCount[0] == 800) {
	writeProbe ();
    }

    if (g_flushes <= 5 || g_flushes % 100 == 0) {
	logf_ ("CorsairSetLedsColorsFlushBuffer #%d (devices %d/%d/%d LEDs set)", g_flushes, g_bufferCount[0], g_bufferCount[1], g_bufferCount[2]);
    }

    for (int i = 0; i < DEVICES; i++) {
	if (g_bufferCount[i] > 0 && (g_flushes <= 3 || g_flushes % 200 == 0)) {
	    char line[2048];
	    int length = 0;

	    for (int led = 0; led < g_bufferCount[i] && led < 40; led++) {
		length += snprintf (line + length, sizeof (line) - length, " %d:%d,%d,%d", g_buffer[i][led].ledId,
				    g_buffer[i][led].r, g_buffer[i][led].g, g_buffer[i][led].b);
	    }

	    logf_ ("  device %d (%s):%s", i, g_info[i].model, line);
	}
    }

    return 1;
}

EXPORT unsigned char CorsairSetLedsColorsAsync (int size, LedColor* colors, void* callback, void* context) {
    logf_ ("CorsairSetLedsColorsAsync %d", size);
    return 1;
}
