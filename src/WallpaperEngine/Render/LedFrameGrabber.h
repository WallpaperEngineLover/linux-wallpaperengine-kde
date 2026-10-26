#pragma once

#include <GL/glew.h>
#include <cstdint>
#include <vector>

namespace WallpaperEngine::Render {
/**
 * What wallpaper64.exe 2.8.42 does for the LED plugin (sub_140110630): a single draw of
 * materials/util/downsample_quarter from the source into an 80 x 10 target, read back one frame later. The shader takes
 * four taps around each output pixel's center, two source texels away on the diagonals
 */
class LedFrameGrabber {
public:
    ~LedFrameGrabber ();

    /** Starts the draw and read back of this source; true when the previous one's rgb (rows from the top, the scene's
     * buffers run top down) landed in out */
    bool grab (GLuint texture, int width, int height, std::vector<uint8_t>& out);

private:
    void create ();

    bool m_created = false;
    GLuint m_program = 0;
    GLuint m_target = 0;
    GLuint m_framebuffer = 0;
    GLuint m_sampler = 0;
    GLuint m_vao = 0;
    GLuint m_buffers[2] = {};
    GLint m_texelLocation = -1;
    int m_slot = 0;
    bool m_pending = false;
};
}
