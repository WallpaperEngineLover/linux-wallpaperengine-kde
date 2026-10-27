#pragma once

#include "MouseInput.h"

namespace WallpaperEngine::Render::Drivers {
class VideoDriver;
}

namespace WallpaperEngine::Input {
class InputContext {
public:
    explicit InputContext (MouseInput& mouseInput);

    void update ();
    void drain ();
    void suspend (bool suspended);

    [[nodiscard]] const MouseInput& getMouseInput () const;

private:
    MouseInput& m_mouse;
};
}
