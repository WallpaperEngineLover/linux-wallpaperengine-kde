#pragma once

namespace WallpaperEngine::Render::Wallpapers {
// the cursor's y in the scene buffer's clip space, which grows towards the bottom of the screen like WE's client y.
// cursorScreenY is the buffer coordinate the output samples under the pointer: top down on an unflipped output and
// bottom up on a flipped one, the clip y is the same picture either way
inline float cursorClipY (float cursorScreenY, bool vFlipped) {
    return vFlipped ? 1.0f - cursorScreenY * 2.0f : cursorScreenY * 2.0f - 1.0f;
}
}
