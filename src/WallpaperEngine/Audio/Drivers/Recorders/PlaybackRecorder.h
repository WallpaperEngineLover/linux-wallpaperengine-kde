#pragma once

namespace WallpaperEngine::Audio::Drivers::Recorders {
class PlaybackRecorder {
public:
    virtual ~PlaybackRecorder () = default;

    virtual void update ();

    /** Guards audio16/32/64 for recorders that fill them from a background capture thread; no-op by default */
    virtual void lock () const { }
    virtual void unlock () const { }

    float audio16[16] = { 0 };
    float audio32[32] = { 0 };
    float audio64[64] = { 0 };
};
} // namespace WallpaperEngine::Audio::Drivers::Recorders