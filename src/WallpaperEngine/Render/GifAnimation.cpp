#include "GifAnimation.h"

#include <algorithm>
#include <cstring>

using namespace WallpaperEngine::Render;

namespace {
bool isGif (const uint8_t* data, size_t size) {
    return size >= 6 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F' && data[3] == '8'
	&& (data[4] == '9' || data[4] == '7') && data[5] == 'a';
}
} // namespace

std::unique_ptr<GifAnimation> GifAnimation::open (const void* data, const size_t size) {
    const auto* bytes = static_cast<const uint8_t*> (data);

    if (!isGif (bytes, size)) {
	return nullptr;
    }

    auto result = std::unique_ptr<GifAnimation> (new GifAnimation ());

    result->m_data.assign (bytes, bytes + size);

    return result;
}

bool GifAnimation::canvasSize (const void* data, const size_t size, int& width, int& height) {
    const auto* bytes = static_cast<const uint8_t*> (data);

    if (!isGif (bytes, size) || size < 10) {
	return false;
    }

    width = bytes[6] | (bytes[7] << 8);
    height = bytes[8] | (bytes[9] << 8);

    return width > 0 && height > 0;
}

uint8_t GifAnimation::get8 () { return m_cursor < m_data.size () ? m_data[m_cursor++] : 0; }

int GifAnimation::get16le () {
    const int low = get8 ();
    return low | (get8 () << 8);
}

void GifAnimation::skip (const int count) {
    m_cursor = count < 0 ? m_data.size () : std::min (m_data.size (), m_cursor + count);
}

// resourceutil64 sub_180006D90
bool GifAnimation::readHeader () {
    if (get8 () != 'G' || get8 () != 'I' || get8 () != 'F' || get8 () != '8') {
	return false;
    }

    const uint8_t version = get8 ();

    if ((version != '7' && version != '9') || get8 () != 'a') {
	return false;
    }

    m_width = get16le ();
    m_height = get16le ();
    m_flags = get8 ();
    m_bgIndex = get8 ();
    get8 (); // aspect ratio
    m_transparent = -1;

    if (m_width > 0x1000000 || m_height > 0x1000000) {
	return false;
    }

    // unlike stb_image, a background index of 0 or past the global table counts as none
    const int count = 2 << (m_flags & 7);

    if ((m_flags & 0x80) == 0) {
	m_bgIndex = -1;
    } else {
	readColorTable (m_palette, count, -1);

	if (m_bgIndex == 0 || m_bgIndex >= count) {
	    m_bgIndex = -1;
	}
    }

    return true;
}

// sub_180006C40, entries are stored BGRA
void GifAnimation::readColorTable (uint8_t (*palette)[4], const int count, const int transparent) {
    for (int i = 0; i < count; i++) {
	palette[i][2] = get8 ();
	palette[i][1] = get8 ();
	palette[i][0] = get8 ();
	palette[i][3] = transparent == i ? 0 : 255;
    }
}

// sub_180007230
void GifAnimation::outputCode (const uint16_t code) {
    if (m_codes[code].prefix >= 0) {
	outputCode (m_codes[code].prefix);
    }

    if (m_curY >= m_maxY) {
	return;
    }

    const int index = m_curX + m_curY;
    uint8_t* pixel = &m_out[index];

    m_history[index / 4] = 1;

    const uint8_t* color = &m_colorTable[m_codes[code].suffix * 4];

    if (color[3] > 128) {
	pixel[0] = color[2];
	pixel[1] = color[1];
	pixel[2] = color[0];
	pixel[3] = color[3];
    }

    m_curX += 4;

    if (m_curX >= m_maxX) {
	m_curX = m_startX;
	m_curY += m_step;

	while (m_curY >= m_maxY && m_parse > 0) {
	    m_step = (1 << m_parse) * m_lineSize;
	    m_curY = m_startY + (m_step >> 1);
	    --m_parse;
	}
    }
}

bool GifAnimation::decodeRaster () {
    const uint8_t lzwSize = get8 ();

    if (lzwSize > 12) {
	return false;
    }

    const int clear = 1 << lzwSize;
    bool first = true;
    int codeSize = lzwSize + 1;
    int codeMask = (1 << codeSize) - 1;
    int bits = 0;
    int validBits = 0;

    for (int code = 0; code < clear; code++) {
	m_codes[code].prefix = -1;
	m_codes[code].first = static_cast<uint8_t> (code);
	m_codes[code].suffix = static_cast<uint8_t> (code);
    }

    int avail = clear + 2;
    int oldCode = -1;
    int length = 0;

    for (;;) {
	if (validBits < codeSize) {
	    if (length == 0) {
		length = get8 ();

		if (length == 0) {
		    return true;
		}
	    }

	    --length;
	    bits |= get8 () << validBits;
	    validBits += 8;
	    continue;
	}

	const int code = bits & codeMask;

	bits >>= codeSize;
	validBits -= codeSize;

	if (code == clear) {
	    codeSize = lzwSize + 1;
	    codeMask = (1 << codeSize) - 1;
	    avail = clear + 2;
	    oldCode = -1;
	    first = false;
	} else if (code == clear + 1) {
	    skip (length);

	    while ((length = get8 ()) > 0) {
		skip (length);
	    }

	    return true;
	} else if (code <= avail) {
	    if (first) {
		return false;
	    }

	    if (oldCode >= 0) {
		Code& entry = m_codes[avail++];

		if (avail > 8192) {
		    return false;
		}

		entry.prefix = static_cast<int16_t> (oldCode);
		entry.first = m_codes[oldCode].first;
		entry.suffix = code == avail ? entry.first : m_codes[code].first;
	    } else if (code == avail) {
		return false;
	    }

	    outputCode (static_cast<uint16_t> (code));

	    if ((avail & codeMask) == 0 && avail <= 0x0FFF) {
		codeSize++;
		codeMask = (1 << codeSize) - 1;
	    }

	    oldCode = code;
	} else {
	    return false;
	}
    }
}

// sub_180007800, stbi__gif_load_next with a restart flag
GifAnimation::Result GifAnimation::loadNext (const uint8_t* twoBack, const bool restart) {
    bool firstFrame = false;

    if (m_out.empty () || restart) {
	if (!readHeader ()) {
	    return Result::Error;
	}

	const size_t count = static_cast<size_t> (m_width) * m_height;

	m_out.assign (count * 4, 0);
	m_history.assign (count, 0);
	firstFrame = true;
    } else {
	int dispose = (m_eflags >> 2) & 7;
	const size_t count = static_cast<size_t> (m_width) * m_height;

	if ((m_eflags & 0x1C) == 12 && twoBack == nullptr) {
	    dispose = 2;
	}

	if (dispose == 3) {
	    for (size_t i = 0; i < count; i++) {
		if (m_history[i]) {
		    memcpy (&m_out[i * 4], &twoBack[i * 4], 4);
		}
	    }
	} else if (dispose == 2) {
	    // no saved background like newer stb_image: the palette entry is copied as stored (BGRA)
	    const uint8_t* fill = nullptr;

	    if (m_bgIndex >= 0) {
		fill = m_transparent < 0 ? m_palette[m_bgIndex] : m_palette[m_transparent];
	    }

	    for (size_t i = 0; i < count; i++) {
		if (!m_history[i]) {
		    continue;
		}

		if (fill == nullptr) {
		    memset (&m_out[i * 4], 0, 4);
		} else {
		    memcpy (&m_out[i * 4], fill, 4);
		}
	    }
	}
    }

    std::fill (m_history.begin (), m_history.end (), 0);

    for (;;) {
	if (m_cursor >= m_data.size ()) {
	    return Result::Error;
	}

	const uint8_t tag = get8 ();

	if (tag == 0x21) {
	    const uint8_t extension = get8 ();

	    if (extension == 0xF9) {
		const uint8_t length = get8 ();

		if (length != 4) {
		    skip (length);
		    continue;
		}

		m_eflags = get8 ();
		m_delay = 10 * get16le ();

		if (m_transparent >= 0) {
		    m_palette[m_transparent][3] = 255;
		}

		if (m_eflags & 1) {
		    m_transparent = get8 ();
		    m_palette[m_transparent][3] = 0;
		} else {
		    skip (1);
		    m_transparent = -1;
		}
	    }

	    int length;

	    while ((length = get8 ()) != 0) {
		skip (length);
	    }

	    continue;
	}

	if (tag == 0x3B) {
	    return Result::Trailer;
	}

	if (tag != 0x2C) {
	    return Result::Error;
	}

	const int x = get16le ();
	const int y = get16le ();
	const int w = get16le ();
	const int h = get16le ();

	if (x + w > m_width || y + h > m_height) {
	    return Result::Error;
	}

	m_lineSize = m_width * 4;
	m_startX = x * 4;
	m_startY = y * m_lineSize;
	m_maxX = m_startX + w * 4;
	m_maxY = m_startY + h * m_lineSize;
	m_curX = m_startX;
	m_curY = w == 0 ? m_maxY : m_startY;

	const int localFlags = get8 ();

	if (localFlags & 0x40) {
	    m_step = 8 * m_lineSize;
	    m_parse = 3;
	} else {
	    m_step = m_lineSize;
	    m_parse = 0;
	}

	if (localFlags & 0x80) {
	    readColorTable (m_localPalette, 2 << (localFlags & 7), m_eflags & 1 ? m_transparent : -1);
	    m_colorTable = &m_localPalette[0][0];
	} else if (m_flags & 0x80) {
	    m_colorTable = &m_palette[0][0];
	} else {
	    return Result::Error;
	}

	if (!decodeRaster ()) {
	    return Result::Error;
	}

	// first frame without transparency: pixels it doesn't draw get the background entry, alpha as stored
	if (firstFrame && m_bgIndex >= 0 && (m_eflags & 1) == 0) {
	    for (size_t i = 0; i < m_history.size (); i++) {
		if (!m_history[i]) {
		    memcpy (&m_out[i * 4], m_palette[m_bgIndex], 4);
		}
	    }
	}

	return Result::Frame;
    }
}

bool GifAnimation::advance (const float dt) {
    if (!m_first && m_timer > 0.0f) {
	m_timer -= dt;

	if (m_timer > 0.0f) {
	    return false;
	}
    }

    m_first = false;

    const uint8_t* twoBack = m_twoBack.empty () ? nullptr : m_twoBack.data ();
    Result result = loadNext (twoBack, false);

    if (result == Result::Trailer) {
	m_cursor = 0;
	result = loadNext (twoBack, true);
    }

    if (result != Result::Frame) {
	return false;
    }

    const bool sameSize = m_lastWidth == 0 || (m_lastWidth == m_width && m_lastHeight == m_height);

    m_lastWidth = m_width;
    m_lastHeight = m_height;
    m_timer += static_cast<float> (m_delay) / 1000.0f;

    const int dispose = m_eflags & 0x1C;

    if (dispose == 0 || dispose == 4) {
	if (m_twoBack.empty ()) {
	    m_twoBack.resize (m_out.size ());
	}

	if (sameSize) {
	    memcpy (m_twoBack.data (), m_out.data (), m_out.size ());
	}
    }

    return true;
}
