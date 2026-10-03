#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace WallpaperEngine::Render {
/**
 * Animated GIF player for user-picked .gif image properties, a port of WE 2.8.42's resourceutil64 GIF exports
 * (PrepareGIF/OpenGIF/AdvanceGIF/FreeGIF). Their decoder is an older stb_image variant: frames come out at the
 * logical screen size, disposal 2 fills with a palette colour instead of the previous background, and the
 * stream rewinds to the first frame after the trailer.
 */
class GifAnimation {
public:
    /** nullptr when the data isn't a GIF (PrepareGIF's stbi__gif_test) */
    static std::unique_ptr<GifAnimation> open (const void* data, size_t size);

    /**
     * AdvanceGIF: steps the frame timer by dt seconds and decodes the next frame once it runs out. The first call
     * always decodes. Returns true when pixels () holds a new frame
     */
    bool advance (float dt);

    [[nodiscard]] const uint8_t* pixels () const { return m_out.data (); }
    [[nodiscard]] int width () const { return m_width; }
    [[nodiscard]] int height () const { return m_height; }

    /** Logical screen size from the GIF header, without decoding anything */
    static bool canvasSize (const void* data, size_t size, int& width, int& height);

private:
    struct Code {
	int16_t prefix;
	uint8_t first;
	uint8_t suffix;
    };

    enum class Result { Frame, Trailer, Error };

    GifAnimation () = default;

    uint8_t get8 ();
    int get16le ();
    void skip (int count);

    bool readHeader ();
    void readColorTable (uint8_t (*palette)[4], int count, int transparent);
    Result loadNext (const uint8_t* twoBack, bool restart);
    bool decodeRaster ();
    void outputCode (uint16_t code);

    std::vector<uint8_t> m_data;
    size_t m_cursor = 0;

    int m_width = 0;
    int m_height = 0;
    std::vector<uint8_t> m_out;
    std::vector<uint8_t> m_history;
    int m_flags = 0;
    int m_bgIndex = -1;
    int m_transparent = -1;
    int m_eflags = 0;
    uint8_t m_palette[256][4] {};
    uint8_t m_localPalette[256][4] {};
    Code m_codes[8192] {};
    const uint8_t* m_colorTable = nullptr;
    int m_parse = 0;
    int m_step = 0;
    int m_startX = 0;
    int m_startY = 0;
    int m_maxX = 0;
    int m_maxY = 0;
    int m_curX = 0;
    int m_curY = 0;
    int m_lineSize = 0;
    int m_delay = 0;

    /** the last frame whose disposal keeps it (0 or 1), what disposal 3 restores to */
    std::vector<uint8_t> m_twoBack;
    float m_timer = 0.0f;
    bool m_first = true;
    int m_lastWidth = 0;
    int m_lastHeight = 0;
};
} // namespace WallpaperEngine::Render
