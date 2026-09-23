#include "CText.h"

#include <algorithm>
#include <cstdio>
#include <unordered_map>
#include <filesystem>
#include <sstream>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "WallpaperEngine/Data/Model/DynamicValue.h"
#include "WallpaperEngine/Data/Model/Material.h"
#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Data/Model/UserSetting.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/CFBO.h"
#include "WallpaperEngine/Render/Camera.h"
#include "WallpaperEngine/Render/TextureProvider.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"
#include "WallpaperEngine/Scripting/ScriptEngine.h"

using namespace WallpaperEngine::Render::Objects;
using namespace WallpaperEngine::Render::Objects::Effects;

namespace {
// Text arrives as UTF-8 (scene JSON, user input, scripted values), but FreeType's FT_Load_Char
// takes one Unicode codepoint per call - decode UTF-8 into codepoints first, or multi-byte
// characters (CJK, emoji, accented Latin) get fed one raw byte at a time and rendered as garbage.
// Malformed sequences are skipped byte-by-byte rather than aborting the whole string.
std::vector<char32_t> decodeUtf8 (const std::string& text) {
    std::vector<char32_t> codepoints;
    size_t i = 0;

    while (i < text.size ()) {
	const auto lead = static_cast<unsigned char> (text[i]);
	size_t extraBytes;
	char32_t codepoint;

	if ((lead & 0x80) == 0x00) {
	    codepoint = lead;
	    extraBytes = 0;
	} else if ((lead & 0xE0) == 0xC0) {
	    codepoint = lead & 0x1F;
	    extraBytes = 1;
	} else if ((lead & 0xF0) == 0xE0) {
	    codepoint = lead & 0x0F;
	    extraBytes = 2;
	} else if ((lead & 0xF8) == 0xF0) {
	    codepoint = lead & 0x07;
	    extraBytes = 3;
	} else {
	    // stray continuation byte or invalid lead byte - skip it and resync
	    i++;
	    continue;
	}

	if (i + extraBytes >= text.size ()) {
	    // truncated multi-byte sequence at the end of the string
	    break;
	}

	bool valid = true;
	for (size_t k = 1; k <= extraBytes; k++) {
	    const auto cont = static_cast<unsigned char> (text[i + k]);
	    if ((cont & 0xC0) != 0x80) {
		valid = false;
		break;
	    }
	    codepoint = (codepoint << 6) | (cont & 0x3F);
	}

	if (!valid) {
	    i++;
	    continue;
	}

	codepoints.push_back (codepoint);
	i += extraBytes + 1;
    }

    return codepoints;
}

// Fallback fonts, used only when the wallpaper's own font (loadEmbeddedFont) can't be loaded.
const std::vector<std::string> kFontCandidates = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
};

// WE's "systemfont_*" names are Windows fonts, ask fontconfig for the closest installed match
std::string fontconfigMatch (const std::string& font) {
    static const std::vector<std::pair<std::string, std::string>> families = {
	{ "systemfont_arial", "Arial,Liberation Sans,Arimo" },
	{ "systemfont_calibri", "Calibri,Carlito" },
	{ "systemfont_cambria", "Cambria,Caladea" },
	{ "systemfont_comicsans", "Comic Sans MS,Comic Neue,Comic Relief" },
	{ "systemfont_consolas", "Consolas,Inconsolata,DejaVu Sans Mono" },
	{ "systemfont_sansserif", "sans-serif" },
	{ "systemfont_segoe", "Segoe UI,Noto Sans,Open Sans" },
	{ "systemfont_verdana", "Verdana,DejaVu Sans" },
    };

    std::string family = "sans-serif";

    for (const auto& [name, list] : families) {
	if (font == name) {
	    family = list;
	    break;
	}
    }

    const std::string command = "fc-match -f '%{file}' '" + family + "' 2>/dev/null";
    FILE* pipe = popen (command.c_str (), "r");

    if (pipe == nullptr) {
	return {};
    }

    std::string path;
    char buffer[512];

    while (fgets (buffer, sizeof (buffer), pipe) != nullptr) {
	path += buffer;
    }

    pclose (pipe);

    return !path.empty () && std::filesystem::exists (path) ? path : std::string ();
}

// Wraps the FreeType-rasterized glyph coverage bitmap (single R8 channel) as a
// TextureProvider so it can be fed into the normal CRenderable/CPass pipeline.
class TextGlyphTexture final : public WallpaperEngine::Render::TextureProvider {
public:
    TextGlyphTexture () {
	glGenTextures (1, &m_textureID);
	glBindTexture (GL_TEXTURE_2D, m_textureID);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    ~TextGlyphTexture () override { glDeleteTextures (1, &m_textureID); }

    void upload (int width, int height, const uint8_t* pixels) {
	m_width = static_cast<uint32_t> (width);
	m_height = static_cast<uint32_t> (height);
	m_resolution = glm::vec4 (
	    static_cast<float> (m_width), static_cast<float> (m_height), static_cast<float> (m_width),
	    static_cast<float> (m_height)
	);

	glBindTexture (GL_TEXTURE_2D, m_textureID);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RED, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, pixels);
    }

    [[nodiscard]] GLuint getTextureID (uint32_t) const override { return m_textureID; }
    [[nodiscard]] uint32_t getTextureWidth (uint32_t) const override { return m_width; }
    [[nodiscard]] uint32_t getTextureHeight (uint32_t) const override { return m_height; }
    [[nodiscard]] uint32_t getRealWidth () const override { return m_width; }
    [[nodiscard]] uint32_t getRealHeight () const override { return m_height; }
    [[nodiscard]] TextureFormat getFormat () const override { return TextureFormat_R8; }
    [[nodiscard]] uint32_t getFlags () const override { return TextureFlags_NoFlags; }
    [[nodiscard]] const std::vector<FrameSharedPtr>& getFrames () const override { return m_frames; }
    [[nodiscard]] const glm::vec4* getResolution () const override { return &m_resolution; }
    [[nodiscard]] bool isAnimated () const override { return false; }
    [[nodiscard]] uint32_t getSpritesheetCols () const override { return 1; }
    [[nodiscard]] uint32_t getSpritesheetRows () const override { return 1; }
    [[nodiscard]] uint32_t getSpritesheetFrames () const override { return 1; }
    [[nodiscard]] float getSpritesheetDuration () const override { return 0.0f; }

    void incrementUsageCount () const override { }
    void decrementUsageCount () const override { }
    void update () const override { }
    [[nodiscard]] bool isReady () const override { return m_width > 0 && m_height > 0; }

private:
    std::vector<FrameSharedPtr> m_frames;
    glm::vec4 m_resolution = { 0.0f, 0.0f, 0.0f, 0.0f };
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    GLuint m_textureID = GL_NONE;
};

// Base pass: tints the R8 glyph coverage texture with g_Color4, using WE's real "font" shader.
// Normal (replace) blending so each frame fully overwrites the FBO's RGBA - CPass never clears
// framebuffers between frames, so translucent blending would accumulate stale alpha over time.
MaterialUniquePtr buildFontMaterial () {
    auto pass = std::make_unique<MaterialPass> (MaterialPass {
	.blending = BlendingMode_Normal,
	.cullmode = CullingMode_Disable,
	.depthtest = DepthtestMode_Disabled,
	.depthwrite = DepthwriteMode_Disabled,
	.shader = "font",
	.textures = {},
	.usertextures = {},
	.combos = { { "COLORFONT", 0 } },
	.constants = {},
    });

    auto material = std::make_unique<Material> ();
    material->filename = "<text/font>";
    material->passes.push_back (std::move (pass));
    return material;
}

// Final pass: blits the (possibly effect-processed) RGBA result onto the actual scene.
MaterialUniquePtr buildCompositeMaterial () {
    auto pass = std::make_unique<MaterialPass> (MaterialPass {
	.blending = BlendingMode_Translucent,
	.cullmode = CullingMode_Disable,
	.depthtest = DepthtestMode_Disabled,
	.depthwrite = DepthwriteMode_Disabled,
	.shader = "genericimage",
	.textures = {},
	.usertextures = {},
	.combos = {},
	.constants = {},
    });

    auto material = std::make_unique<Material> ();
    material->filename = "<text/composite>";
    material->passes.push_back (std::move (pass));
    return material;
}

const Material& fontMaterial () {
    static const MaterialUniquePtr material = buildFontMaterial ();
    return *material;
}

const Material& compositeMaterial () {
    static const MaterialUniquePtr material = buildCompositeMaterial ();
    return *material;
}

// Mirrors CImage.cpp's clampParallaxAxis: keeps an edge pair from sliding past the viewport
// once `offset` is added to both; a box too small to cover the viewport on this axis has no
// ground to uncover and moves freely.
float clampParallaxAxis (float offset, float edgeA, float edgeB, float sceneExtent) {
    const float low = std::min (edgeA, edgeB);
    const float high = std::max (edgeA, edgeB);
    const float half = sceneExtent / 2.0f;
    const float maxOffset = -half - low;
    const float minOffset = half - high;

    if (minOffset > maxOffset)
	return offset;

    return std::clamp (offset, minOffset, maxOffset);
}
} // namespace

CText::CText (Wallpapers::CScene& scene, const Text& text) :
    CObject (scene, text), CRenderable (scene, text, fontMaterial ()), ScriptableObject (scene, text), m_text (text) {
    this->registerProperty ("color", *text.color->value);
    this->registerProperty ("alpha", *text.alpha->value);
    this->registerProperty ("origin", *text.origin->value);
    this->registerProperty ("scale", *text.scale->value);
    this->registerProperty ("visible", *text.visible->value);
    this->registerProperty ("pointSize", *text.pointSize->value);
    this->registerProperty ("text", *text.text->value);
    this->registerProperty ("parallaxDepth", *text.parallaxDepth->value);
    this->registerEffectConstants (text.effects);
}

CText::~CText () {
    if (m_layerHandle != Scripting::kInvalidLayerHandle) {
	this->getScene ().getScriptEngine ().destroyLayer (m_layerHandle);
	m_layerHandle = Scripting::kInvalidLayerHandle;
    }

    this->destroyPasses ();

    if (m_copySpacePosition != 0) {
	glDeleteBuffers (1, &m_copySpacePosition);
    }
    if (m_passSpacePosition != 0) {
	glDeleteBuffers (1, &m_passSpacePosition);
    }
    if (m_sceneSpacePosition != 0) {
	glDeleteBuffers (1, &m_sceneSpacePosition);
    }
    if (m_texcoordCopy != 0) {
	glDeleteBuffers (1, &m_texcoordCopy);
    }

    if (m_ftFace != nullptr) {
	FT_Done_Face (m_ftFace);
    }
    if (m_ftLibrary != nullptr) {
	FT_Done_FreeType (m_ftLibrary);
    }
}

void CText::destroyPasses () {
    for (auto* pass : m_passes) {
	delete pass;
    }
    m_passes.clear ();
    m_mainFBO = nullptr;
    m_subFBO = nullptr;
    m_currentMainFBO = nullptr;
    m_currentSubFBO = nullptr;
}

void CText::setup () {
    const bool scripted = m_text.text->value->getScriptSource ().has_value ();
    const auto& text = m_text.text->value->getString ();

    if (text.empty () && !scripted) {
	return;
    }

    if (!initFreeType ()) {
	return;
    }

    if (!loadEmbeddedFont () && !loadSystemFont ()) {
	return;
    }

    m_lastPixelSize = computeEffectivePixelSize ();
    FT_Set_Pixel_Sizes (m_ftFace, 0, static_cast<FT_UInt> (m_lastPixelSize));

    m_glyphTexture = std::make_shared<TextGlyphTexture> ();
    this->m_texture = m_glyphTexture;

    // Scripted text may have an empty placeholder; use a single space so the
    // glyph texture has non-zero dimensions until the script produces a value.
    rebuildTextureFrom (text.empty () ? std::string (" ") : text);

    if (scripted) {
	initScriptLayer ();
    }

    m_valid = m_glyphTexture != nullptr && m_glyphTexture->isReady ();

    if (!m_valid) {
	return;
    }

    CRenderable::setup ();

    buildPasses ();
    m_initialized = true;
}

bool CText::initFreeType () {
    if (FT_Init_FreeType (&m_ftLibrary) == 0) {
	return true;
    }
    sLog.error ("CText: FT_Init_FreeType failed for object ", m_text.name);
    return false;
}

bool CText::loadEmbeddedFont () {
    // Wallpapers packed in .pkg don't expose physical paths, so we read the font
    // into memory and use FT_New_Memory_Face. m_fontData must outlive the face.
    // `systemfont_*` references signal "use a system font"; let the fallback handle them.
    if (m_text.font.empty () || m_text.font.rfind ("systemfont_", 0) == 0) {
	return false;
    }

    try {
	auto stream = getAssetLocator ().read (m_text.font);
	stream->seekg (0, std::ios::end);
	const auto size = stream->tellg ();
	stream->seekg (0, std::ios::beg);
	m_fontData.resize (static_cast<size_t> (size));
	stream->read (reinterpret_cast<char*> (m_fontData.data ()), size);

	if (FT_New_Memory_Face (
		m_ftLibrary, m_fontData.data (), static_cast<FT_Long> (m_fontData.size ()), 0, &m_ftFace
	    )
	    == 0) {
	    return true;
	}

	sLog.error ("CText: FT_New_Memory_Face failed for '", m_text.font, "', falling back to system font");
    } catch (const std::exception& e) {
	sLog.error ("CText: cannot read font '", m_text.font, "': ", e.what (), ", falling back to system font");
    }

    m_fontData.clear ();
    return false;
}

bool CText::loadSystemFont () {
    std::string fontPath = fontconfigMatch (m_text.font);

    if (fontPath.empty ()) {
	for (const auto& candidate : kFontCandidates) {
	    if (std::filesystem::exists (candidate)) {
		fontPath = candidate;
		break;
	    }
	}
    }
    if (fontPath.empty ()) {
	sLog.error ("CText: no usable system font found");
	return false;
    }
    if (FT_New_Face (m_ftLibrary, fontPath.c_str (), 0, &m_ftFace) != 0) {
	sLog.error ("CText: FT_New_Face failed for ", fontPath);
	return false;
    }
    return true;
}

unsigned int CText::computeEffectivePixelSize () const {
    // WE rasterizes glyphs at pointsize * 300 / 72 pixels (wallpaper64.exe), the object's scale only applies to the quad
    const float pointSize = std::clamp (m_text.pointSize->value->getFloat (), 1.0f, 256.0f);
    return std::max<unsigned int> (1u, static_cast<unsigned int> (std::lround (pointSize * 300.0f / 72.0f)));
}

void CText::initScriptLayer () {
    const auto& script = m_text.text->value->getScriptSource ();

    if (!script.has_value ()) {
	return;
    }

    // a script already running as a regular property module drives the value itself
    if (this->getScene ().getScriptEngine ().hasScript (*m_text.text->value)) {
	m_textFromProperty = true;
	return;
    }

    m_layerHandle = this->getScene ().getScriptEngine ().createLayerScript (
	*script, m_text.text->value->getProperties (), m_text.text->value->getString ()
    );

    if (m_layerHandle == Scripting::kInvalidLayerHandle) {
	sLog.error ("CText: createLayerScript failed for '", m_text.name, "'");
    }
}

void CText::rebuildTextureFrom (const std::string& text) {
    // like wallpaper64.exe, the layout box is made of whole font lines rather than the glyphs' ink
    FT_GlyphSlot slot = m_ftFace->glyph;
    const auto& faceMetrics = m_ftFace->size->metrics;
    const int ascender = std::max (1, static_cast<int> ((faceMetrics.ascender + 63) >> 6));
    const int descender = std::max (0, static_cast<int> ((-faceMetrics.descender + 63) >> 6));
    const int lineHeight = std::max (ascender + descender, static_cast<int> ((faceMetrics.height + 63) >> 6));

    std::unordered_map<char32_t, int> advances;
    const auto advanceOf = [&] (char32_t c) {
	const auto cached = advances.find (c);

	if (cached != advances.end ()) {
	    return cached->second;
	}

	int advance = 0;

	if (FT_Load_Char (m_ftFace, static_cast<FT_ULong> (c), FT_LOAD_DEFAULT) == 0) {
	    advance = static_cast<int> (slot->advance.x >> 6);
	}

	advances.emplace (c, advance);
	return advance;
    };
    const auto widthOf = [&] (const std::vector<char32_t>& codepoints) {
	int width = 0;

	for (const char32_t c : codepoints) {
	    width += advanceOf (c);
	}

	return width;
    };
    const auto trimTrailingSpaces = [] (std::vector<char32_t>& codepoints) {
	while (!codepoints.empty () && (codepoints.back () == U' ' || codepoints.back () == U'\t')) {
	    codepoints.pop_back ();
	}
    };

    const bool limitWidth = m_text.limitWidth->value->getBool ();
    const int maxLineWidth = std::max (1, static_cast<int> (m_text.maxWidth->value->getFloat ()));

    std::vector<std::vector<char32_t>> lines;
    size_t paragraphStart = 0;

    while (true) {
	const size_t pos = text.find ('\n', paragraphStart);
	std::string paragraph = text.substr (paragraphStart, pos == std::string::npos ? pos : pos - paragraphStart);

	if (!paragraph.empty () && paragraph.back () == '\r') {
	    paragraph.pop_back ();
	}

	const auto codepoints = decodeUtf8 (paragraph);

	if (!limitWidth) {
	    lines.push_back (codepoints);
	} else {
	    std::vector<char32_t> current;
	    int currentWidth = 0;
	    const auto flush = [&] () {
		trimTrailingSpaces (current);
		lines.push_back (std::move (current));
		current.clear ();
		currentWidth = 0;
	    };

	    for (size_t i = 0; i < codepoints.size ();) {
		const bool space = codepoints[i] == U' ';
		size_t j = i;

		while (j < codepoints.size () && (codepoints[j] == U' ') == space) {
		    j++;
		}

		const std::vector<char32_t> token (codepoints.begin () + static_cast<long> (i), codepoints.begin () + static_cast<long> (j));
		const int tokenWidth = widthOf (token);
		i = j;

		if (space) {
		    // spaces at the start of a wrapped line are dropped
		    if (!current.empty () || lines.empty ()) {
			current.insert (current.end (), token.begin (), token.end ());
			currentWidth += tokenWidth;
		    }
		    continue;
		}

		if (currentWidth + tokenWidth <= maxLineWidth) {
		    current.insert (current.end (), token.begin (), token.end ());
		    currentWidth += tokenWidth;
		    continue;
		}

		if (!current.empty ()) {
		    flush ();
		}

		if (tokenWidth <= maxLineWidth) {
		    current = token;
		    currentWidth = tokenWidth;
		    continue;
		}

		// a single word wider than the box gets broken between characters
		for (const char32_t c : token) {
		    const int advance = advanceOf (c);

		    if (currentWidth + advance > maxLineWidth && !current.empty ()) {
			flush ();
		    }

		    current.push_back (c);
		    currentWidth += advance;
		}
	    }

	    flush ();
	}

	if (pos == std::string::npos) {
	    break;
	}

	paragraphStart = pos + 1;
    }

    if (m_text.limitRows->value->getBool ()) {
	const auto maxRows = static_cast<size_t> (std::max (1, static_cast<int> (std::lround (m_text.maxRows->value->getFloat ()))));

	if (lines.size () > maxRows) {
	    lines.resize (maxRows);

	    if (m_text.limitUseEllipsis->value->getBool ()) {
		auto& last = lines.back ();
		trimTrailingSpaces (last);

		std::vector<char32_t> ellipsis = { U'\u2026' };

		if (FT_Get_Char_Index (m_ftFace, U'\u2026') == 0) {
		    ellipsis = { U'.', U'.', U'.' };
		}

		if (last.size () < ellipsis.size () || !std::equal (ellipsis.begin (), ellipsis.end (), last.end () - static_cast<long> (ellipsis.size ()))) {
		    last.insert (last.end (), ellipsis.begin (), ellipsis.end ());
		}

		while (limitWidth && widthOf (last) > maxLineWidth && last.size () > ellipsis.size () + 1) {
		    last.erase (last.end () - static_cast<long> (ellipsis.size ()) - 1);
		}
	    }
	}
    }

    int maxWidth = 0;
    std::vector<int> lineWidths (lines.size ());

    for (size_t i = 0; i < lines.size (); ++i) {
	lineWidths[i] = widthOf (lines[i]);
	maxWidth = std::max (maxWidth, lineWidths[i]);
    }

    const int width = std::max (1, maxWidth);
    const int height = ascender + descender + static_cast<int> (lines.size () - 1) * lineHeight;

    const auto forEachGlyph = [&] (const auto& visit) {
	for (size_t i = 0; i < lines.size (); ++i) {
	    int penX = 0;

	    if (m_text.alignment == "center") {
		penX = (width - lineWidths[i]) / 2;
	    } else if (m_text.alignment == "right") {
		penX = width - lineWidths[i];
	    }

	    const int baseline = ascender + static_cast<int> (i) * lineHeight;

	    for (const char32_t c : lines[i]) {
		if (FT_Load_Char (m_ftFace, static_cast<FT_ULong> (c), FT_LOAD_RENDER) != 0) {
		    continue;
		}

		visit (slot->bitmap, penX + slot->bitmap_left, baseline - slot->bitmap_top);
		penX += slot->advance.x >> 6;
	    }
	}
    };

    // grow the texture for overhanging glyphs and leave room for effects that spread past them
    const int margin = std::max (16, static_cast<int> (m_lastPixelSize) / 6);
    int padLeft = margin;
    int padRight = margin;
    int padTop = margin;
    int padBottom = margin;

    forEachGlyph ([&] (const FT_Bitmap& bmp, int originX, int originY) {
	if (bmp.width == 0 || bmp.rows == 0) {
	    return;
	}

	padLeft = std::max (padLeft, margin - originX);
	padRight = std::max (padRight, margin + originX + static_cast<int> (bmp.width) - width);
	padTop = std::max (padTop, margin - originY);
	padBottom = std::max (padBottom, margin + originY + static_cast<int> (bmp.rows) - height);
    });

    const int textureWidth = width + padLeft + padRight;
    const int textureHeight = height + padTop + padBottom;
    std::vector<uint8_t> pixels (static_cast<size_t> (textureWidth) * textureHeight, 0);

    forEachGlyph ([&] (const FT_Bitmap& bmp, int originX, int originY) {
	for (unsigned int row = 0; row < bmp.rows; ++row) {
	    for (unsigned int col = 0; col < bmp.width; ++col) {
		const int dstX = padLeft + originX + static_cast<int> (col);
		const int dstY = padTop + originY + static_cast<int> (row);

		// glyphs may overlap by a pixel (kerning-less advances): keep the stronger coverage
		auto& dst = pixels[static_cast<size_t> (dstY) * textureWidth + dstX];
		dst = std::max (dst, bmp.buffer[row * bmp.pitch + col]);
	    }
	}
    });

    // where the layout box sits relative to the texture's center, in texture pixels (y down)
    m_boxSize = { static_cast<float> (width), static_cast<float> (height) };
    m_boxShift = { static_cast<float> (padLeft - padRight) * 0.5f, static_cast<float> (padTop - padBottom) * 0.5f };

    m_descender = descender;

    const glm::ivec2 previousSize = m_textureSize;

    // FreeType bitmaps store row 0 as the glyph's TOP row; upload as-is and flip
    // when building the quad's V coordinates instead (see uploadQuadVertices).
    std::vector<uint8_t> flipped (pixels.size ());
    for (int row = 0; row < textureHeight; ++row) {
	std::copy_n (
	    pixels.begin () + static_cast<long> (row) * textureWidth, textureWidth,
	    flipped.begin () + static_cast<long> (textureHeight - 1 - row) * textureWidth
	);
    }

    static_cast<TextGlyphTexture*> (m_glyphTexture.get ())->upload (textureWidth, textureHeight, flipped.data ());

    m_textureSize = { textureWidth, textureHeight };
    m_quadSize = { static_cast<float> (textureWidth), static_cast<float> (textureHeight) };
    m_lastRenderedText = text;

    uploadQuadVertices ();

    // The glyph texture's own pixel size drives every FBO in the pass chain, so
    // resizing it (new text with a different width/height) means rebuilding them.
    if (m_initialized && previousSize != m_textureSize) {
	buildPasses ();
    }
}

void CText::uploadQuadVertices () {
    const float w = m_quadSize.x;
    const float h = m_quadSize.y;
    const float hx = w * 0.5f;
    const float hy = h * 0.5f;

    // "Copy space": local, unscaled, un-positioned quad spanning (0,0)-(w,h), matching
    // CImage's copy-space convention - used to render the glyph texture at its natural
    // pixel size into the first FBO, before any scene position/scale is applied.
    const GLfloat copySpacePosition[]
	= { 0.0f, h, 0.0f, 0.0f, 0.0f, 0.0f, w, h, 0.0f, w, h, 0.0f, 0.0f, 0.0f, 0.0f, w, 0.0f, 0.0f };

    const GLfloat passSpacePosition[]
	= { -1.0f, 1.0f, 0.0f, -1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f };

    // "Scene space": centered quad used for the final composite pass, positioned via
    // the scene MVP (translate + scale) computed in render().
    const GLfloat sceneSpacePosition[]
	= { -hx, -hy, 0.0f, -hx, hy, 0.0f, hx, -hy, 0.0f, hx, -hy, 0.0f, -hx, hy, 0.0f, hx, hy, 0.0f };

    const GLfloat texcoord[] = { 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f };

    if (m_copySpacePosition == 0) {
	glGenBuffers (1, &m_copySpacePosition);
    }
    glBindBuffer (GL_ARRAY_BUFFER, m_copySpacePosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (copySpacePosition), copySpacePosition, GL_STATIC_DRAW);

    if (m_passSpacePosition == 0) {
	glGenBuffers (1, &m_passSpacePosition);
    }
    glBindBuffer (GL_ARRAY_BUFFER, m_passSpacePosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (passSpacePosition), passSpacePosition, GL_STATIC_DRAW);

    if (m_sceneSpacePosition == 0) {
	glGenBuffers (1, &m_sceneSpacePosition);
    }
    glBindBuffer (GL_ARRAY_BUFFER, m_sceneSpacePosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (sceneSpacePosition), sceneSpacePosition, GL_STATIC_DRAW);

    if (m_texcoordCopy == 0) {
	glGenBuffers (1, &m_texcoordCopy);
    }
    glBindBuffer (GL_ARRAY_BUFFER, m_texcoordCopy);
    glBufferData (GL_ARRAY_BUFFER, sizeof (texcoord), texcoord, GL_STATIC_DRAW);

    m_modelViewProjectionCopy = glm::ortho<float> (0.0f, w, 0.0f, h);
    m_modelViewProjectionCopyInverse = glm::inverse (m_modelViewProjectionCopy);
    m_modelMatrix = m_modelViewProjectionCopy;
    m_viewProjectionMatrix = glm::mat4 (1.0f);
}

void CText::buildPasses () {
    this->destroyPasses ();

    if (m_quadSize.x <= 0.0f || m_quadSize.y <= 0.0f) {
	return;
    }

    const glm::vec2 fboSize = { std::max (1.0f, m_quadSize.x), std::max (1.0f, m_quadSize.y) };

    std::ostringstream nameA, nameB;
    nameA << "_rt_textComposite_" << this->getId () << "_a";
    nameB << "_rt_textComposite_" << this->getId () << "_b";

    this->m_currentMainFBO = this->m_mainFBO
	= this->create (nameA.str (), TextureFormat_ARGB8888, TextureFlags_ClampUVs, 1.0f, fboSize, fboSize);
    this->m_currentSubFBO = this->m_subFBO
	= this->create (nameB.str (), TextureFormat_ARGB8888, TextureFlags_ClampUVs, 1.0f, fboSize, fboSize);

    // base pass: tint the glyph coverage texture and render it into the first FBO
    for (const auto& pass : fontMaterial ().passes) {
	auto* cpass
	    = new CPass (*this, std::make_shared<FBOProvider> (this), *pass, std::nullopt, std::nullopt, std::nullopt);
	cpass->setDestination (m_currentMainFBO);
	cpass->setInput (m_glyphTexture);
	cpass->setPosition (m_copySpacePosition);
	cpass->setTexCoord (m_texcoordCopy);
	cpass->setModelMatrix (&m_modelMatrix);
	cpass->setViewProjectionMatrix (&m_viewProjectionMatrix);
	cpass->setModelViewProjectionMatrix (&m_modelViewProjectionCopy);
	cpass->setModelViewProjectionMatrixInverse (&m_modelViewProjectionCopyInverse);
	m_passes.push_back (cpass);
    }

    std::shared_ptr<const TextureProvider> asInput = m_currentMainFBO;

    const auto& debug = this->getScene ().getContext ().getApp ().getContext ().settings.render.debug;
    if (!debug.baseOnly) {
	for (const auto& effect : m_text.effects) {
	    if (!effect->visible->value->getBool ()) {
		continue;
	    }

	    const auto fboProvider = std::make_shared<FBOProvider> (this);
	    for (const auto& fbo : effect->effect->fbos) {
		fboProvider->create (*fbo, TextureFlags_ClampUVs, fboSize);
	    }

	    auto curOverride = effect->passOverrides.begin ();
	    const auto endOverride = effect->passOverrides.end ();

	    // same target/previous bookkeeping as CImage::setupPasses
	    bool inTargetSequence = false;
	    std::shared_ptr<const TextureProvider> sequenceInput = nullptr;

	    for (const auto& effectPass : effect->effect->passes) {
		if (!effectPass->material.has_value ()) {
		    // command-only passes (e.g. plain FBO copies) aren't supported for text
		    continue;
		}

		for (auto& matPass : effectPass->material.value ()->passes) {
		    const auto override = curOverride != endOverride
			? **curOverride
			: std::optional<std::reference_wrapper<const ImageEffectPassOverride>> (std::nullopt);
		    const auto target = effectPass->target.has_value ()
			? *effectPass->target
			: std::optional<std::reference_wrapper<std::string>> (std::nullopt);

		    auto* cpass = new CPass (*this, fboProvider, *matPass, override, effectPass->binds, target);
		    std::shared_ptr<const CFBO> drawTo = this->m_currentSubFBO;
		    bool writesToTarget = false;

		    if (target.has_value ()) {
			std::shared_ptr<const CFBO> resolved = fboProvider->find (target->get ());

			if (resolved == nullptr) {
			    resolved = this->getScene ().findFBO (target->get ());
			}

			if (resolved != nullptr) {
			    if (!inTargetSequence) {
				sequenceInput = asInput;
				inTargetSequence = true;
			    }

			    drawTo = resolved;
			    writesToTarget = true;
			} else {
			    sLog.error ("Text pass target FBO '", target->get (), "' could not be resolved for ", m_text.name);
			}
		    }

		    cpass->setDestination (drawTo);
		    cpass->setInput (asInput);
		    cpass->setPreviousInput (inTargetSequence ? sequenceInput : nullptr);
		    cpass->setPosition (m_passSpacePosition);
		    cpass->setTexCoord (m_texcoordCopy);
		    cpass->setModelMatrix (&m_modelMatrix);
		    cpass->setViewProjectionMatrix (&m_viewProjectionMatrix);
		    cpass->setModelViewProjectionMatrix (&m_modelViewProjectionPass);
		    cpass->setModelViewProjectionMatrixInverse (&m_modelViewProjectionPass);
		    m_passes.push_back (cpass);

		    asInput = drawTo;

		    if (!writesToTarget) {
			std::swap (this->m_currentMainFBO, this->m_currentSubFBO);
			inTargetSequence = false;
			sequenceInput = nullptr;
		    }
		}

		if (curOverride != endOverride) {
		    ++curOverride;
		}
	    }
	}
    }

    // final pass: composite the accumulated result onto the actual scene
    for (const auto& pass : compositeMaterial ().passes) {
	auto* cpass
	    = new CPass (*this, std::make_shared<FBOProvider> (this), *pass, std::nullopt, std::nullopt, std::nullopt);
	cpass->setDestination (this->getScene ().getFBO ());
	cpass->setInput (asInput);
	cpass->setPosition (m_sceneSpacePosition);
	cpass->setTexCoord (m_texcoordCopy);
	cpass->setModelMatrix (&m_modelMatrix);
	cpass->setViewProjectionMatrix (&m_viewProjectionMatrix);
	cpass->setModelViewProjectionMatrix (&m_modelViewProjectionScreen);
	cpass->setModelViewProjectionMatrixInverse (&m_modelViewProjectionScreenInverse);
	m_passes.push_back (cpass);
    }
}

void CText::render () {
    if (!m_initialized) {
	return;
    }
    const auto& appContext = this->getScene ().getContext ().getApp ().getContext ();
    const auto visibility = appContext.resolveObjectVisibility (this->getId (), this->getObject ().name);
    if (!visibility.value_or (m_text.visible->value->getBool ())) {
	return;
    }

    std::string renderedText = m_lastRenderedText;
    if (m_layerHandle != Scripting::kInvalidLayerHandle) {
	auto& se = this->getScene ().getScriptEngine ();
	se.tickLayer (
	    m_layerHandle, static_cast<double> (getScene ().getTime ()),
	    static_cast<double> (getScene ().getDeltaTime ()), static_cast<double> (getScene ().getFps ())
	);
	const std::string current = se.layerText (m_layerHandle);
	renderedText = current.empty () ? std::string (" ") : current;
    } else if (m_textFromProperty) {
	const std::string current = m_text.text->value->getString ();
	renderedText = current.empty () ? std::string (" ") : current;
    }

    const unsigned int pixelSize = computeEffectivePixelSize ();
    if (pixelSize != m_lastPixelSize) {
	m_lastPixelSize = pixelSize;
	FT_Set_Pixel_Sizes (m_ftFace, 0, static_cast<FT_UInt> (m_lastPixelSize));
	rebuildTextureFrom (renderedText);
    } else if (renderedText != m_lastRenderedText) {
	rebuildTextureFrom (renderedText);
    }

    glm::vec3 scale = m_text.scale->value->getVec3 ();
    glm::vec3 origin = m_text.origin->value->getVec3 ();

    // texts sit under group/locator objects, same as CImage::resolveTransform
    if (m_text.parent.has_value ()) {
	std::vector<const Object*> ancestors;

	for (const Object* current = &m_text; current->parent.has_value () && ancestors.size () < 32;) {
	    const auto* parentObject = this->getScene ().getObject (current->parent.value ());

	    if (parentObject == nullptr) {
		break;
	    }

	    current = &parentObject->getObject ();
	    ancestors.push_back (current);
	}

	glm::vec3 parentOrigin (0.0f);
	glm::vec3 parentScale (1.0f);
	float parentAngle = 0.0f;
	const auto rotate = [] (const glm::vec2& v, float angle) {
	    const float cosine = std::cos (angle);
	    const float sine = std::sin (angle);
	    return glm::vec2 (v.x * cosine - v.y * sine, v.x * sine + v.y * cosine);
	};

	for (auto it = ancestors.rbegin (); it != ancestors.rend (); ++it) {
	    const Object& node = **it;
	    glm::vec3 nodeOrigin = node.origin->value->getVec3 ();
	    glm::vec3 nodeScale = node.groupScale->value->getVec3 ();
	    float nodeAngle = node.groupAngles->value->getVec3 ().z;

	    if (node.is<Image> ()) {
		nodeScale = node.as<Image> ()->scale->value->getVec3 ();
		nodeAngle = node.as<Image> ()->angles->value->getVec3 ().z;
	    }

	    const glm::vec2 offset = rotate ({ nodeOrigin.x * parentScale.x, nodeOrigin.y * parentScale.y }, parentAngle);
	    parentOrigin = { parentOrigin.x + offset.x, parentOrigin.y + offset.y,
			     parentOrigin.z + nodeOrigin.z * parentScale.z };
	    parentScale *= nodeScale;
	    parentAngle += nodeAngle;
	}

	const glm::vec2 offset = rotate ({ origin.x * parentScale.x, origin.y * parentScale.y }, parentAngle);
	origin = { parentOrigin.x + offset.x, parentOrigin.y + offset.y, parentOrigin.z + origin.z * parentScale.z };
	scale *= parentScale;
    }

    // the glyph bbox is centered on the origin then shifted by the alignment anchor (wallpaper64.exe), json "size" is not used
    const float scaledHalfWidth = m_boxSize.x * 0.5f * scale.x;
    const float scaledHalfHeight = m_boxSize.y * 0.5f * scale.y;

    float offsetX = 0.0f;
    if (m_text.alignment == "left") {
	offsetX = scaledHalfWidth;
    } else if (m_text.alignment == "right") {
	offsetX = -scaledHalfWidth;
    }

    // offsetY is added to origin.y, which grows towards the top of the screen
    float offsetY = 0.0f;
    if (m_text.verticalalign == "top") {
	offsetY = -scaledHalfHeight;
    } else if (m_text.verticalalign == "bottom") {
	offsetY = scaledHalfHeight;
    } else {
	// "center" is moved down by half the descender, matching WE
	offsetY = -static_cast<float> (m_descender) * 0.5f * scale.y;
    }

    // the texture center is off the box center by the overhang padding, move the quad the opposite way
    offsetX -= m_boxShift.x * scale.x;
    offsetY += m_boxShift.y * scale.y;

    // WE uses a Y-down coordinate system; match CImage's convention (CImage.cpp's
    // updateScenePosition) of scene_h/2 - y rather than y - scene_h/2.
    const float scene_w = getScene ().getCamera ().getWidth ();
    const float scene_h = getScene ().getCamera ().getHeight ();

    // Matches CImage's parallax handling (CImage.cpp:updateScreenSpacePosition) in the same
    // pre-scale, canvas-space units as origin, so text stays visually locked to other objects at
    // the same parallaxDepth. Added directly to gl_origin (not after the model matrix) so it
    // isn't inadvertently multiplied by this object's own "scale".
    glm::vec2 parallaxOffset = { 0.0f, 0.0f };
    // CScene::renderFrame() already folds disableparallax into getParallaxDisplacement()
    if (this->getScene ().getScene ().camera.parallax.enabled->value->getBool ()) {
	parallaxOffset = this->getScene ().getParallaxOffset (m_text);

	// mirrors CImage's parallax clamp, or a text layer drifts past its edges while a same-depth
	// CImage backing panel freezes, visibly separating the two
	if (this->getScene ().getContext ().getApp ().getContext ().settings.mouse.clampParallaxToImageSize) {
	    const float baseX = origin.x + offsetX - scene_w * 0.5f;
	    const float baseY = scene_h * 0.5f - (origin.y + offsetY);
	    parallaxOffset.x = clampParallaxAxis (
		parallaxOffset.x, baseX - scaledHalfWidth, baseX + scaledHalfWidth, getScene ().getCanvasWidth ()
	    );
	    parallaxOffset.y = clampParallaxAxis (
		parallaxOffset.y, baseY - scaledHalfHeight, baseY + scaledHalfHeight, getScene ().getCanvasHeight ()
	    );
	}
    }

    const glm::vec3 gl_origin = {
	origin.x + offsetX - scene_w * 0.5f + parallaxOffset.x,
	scene_h * 0.5f - (origin.y + offsetY) + parallaxOffset.y,
	origin.z,
    };

    glm::mat4 model = glm::translate (glm::mat4 (1.0f), gl_origin);
    model = glm::scale (model, scale);

    m_modelViewProjectionScreen = getScene ().getCamera ().getProjection () * getScene ().getCamera ().getLookAt () * model;
    m_modelViewProjectionScreenInverse = glm::inverse (m_modelViewProjectionScreen);

    glColorMask (true, true, true, true);
    glDisable (GL_DEPTH_TEST);

#if !NDEBUG
    std::string str = "Text " + this->getObject ().name + " (" + std::to_string (this->getId ()) + ")";
    glPushDebugGroup (GL_DEBUG_SOURCE_APPLICATION, 0, -1, str.c_str ());
#endif /* DEBUG */

    for (auto* pass : m_passes) {
	pass->render ();
    }

#if !NDEBUG
    glPopDebugGroup ();
#endif /* DEBUG */
}

const float& CText::getBrightness () const {
    static constexpr float kUnitBrightness = 1.0f;
    return kUnitBrightness;
}

const float& CText::getUserAlpha () const { return m_text.alpha->value->getFloat (); }

const float& CText::getAlpha () const { return m_text.alpha->value->getFloat (); }

const glm::vec3& CText::getColor () const { return m_text.color->value->getVec3 (); }

const glm::vec4& CText::getColor4 () const {
    const glm::vec3 rgb = m_text.color->value->getVec3 ();
    m_color4Cache = glm::vec4 (rgb, m_text.alpha->value->getFloat ());
    return m_color4Cache;
}

const glm::vec3& CText::getCompositeColor () const { return m_text.color->value->getVec3 (); }
