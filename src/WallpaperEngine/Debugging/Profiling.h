#pragma once

#ifdef LWE_TRACY
#include <GL/glew.h>
#include <string>
#include <tracy/Tracy.hpp>
#include <tracy/TracyOpenGL.hpp>

#define LWE_ZONE(name) ZoneScopedN (name)
#define LWE_ZONE_TEXT(name, text)                                                                                      \
    ZoneScopedN (name);                                                                                                \
    {                                                                                                                  \
	const std::string& zoneText = (text);                                                                          \
	ZoneText (zoneText.c_str (), zoneText.size ());                                                                \
    }
#define LWE_ZONE_NAMED(text)                                                                                           \
    const std::string lweZoneName = (text);                                                                            \
    ZoneTransientN (lweZone, lweZoneName.c_str (), true)
#define LWE_GPU_ZONE(name) TracyGpuZone (name)
#define LWE_GPU_COLLECT() TracyGpuCollect
#define LWE_FRAME_MARK() FrameMark
#define LWE_PLOT(name, value) TracyPlot (name, value)
#else
#define LWE_ZONE(name) ((void)0)
#define LWE_ZONE_TEXT(name, text) ((void)0)
#define LWE_ZONE_NAMED(text) ((void)0)
#define LWE_GPU_ZONE(name) ((void)0)
#define LWE_GPU_COLLECT() ((void)0)
#define LWE_FRAME_MARK() ((void)0)
#define LWE_PLOT(name, value) ((void)0)
#endif
