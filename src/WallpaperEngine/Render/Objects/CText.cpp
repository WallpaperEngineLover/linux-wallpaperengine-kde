#include "CText.h"

#include <algorithm>
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
// Fallback fonts, used only when the wallpaper's own font (loadEmbeddedFont) can't be loaded.
const std::vector<std::string> kFontCandidates = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
};

// Wraps the FreeType-rasterized glyph coverage bitmap (single R8 channel) as a
// TextureProvider so it can be fed into the normal CRenderable/CPass pipeline,
// the same way AlbumTexture wraps a dynamically-loaded album cover.
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
// framebuffers between frames, and translucent blending would accumulate stale alpha over time.
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
    std::string fontPath;
    for (const auto& candidate : kFontCandidates) {
	if (std::filesystem::exists (candidate)) {
	    fontPath = candidate;
	    break;
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
    // WE text objects often come with scale ~0.09 that, combined with a modest
    // pointsize, would rasterize glyphs to ~2px on screen (invisible). Rasterize
    // at higher resolution so that after the model scale is applied in render()
    // the on-screen size matches the intended pointsize.
    //
    // For scale >= 1, measurements against real Wallpaper Engine show the final glyph
    // size also needs an extra factor of scale beyond the model-matrix multiply already
    // applied in render() - i.e. final size scales with scale^2, not scale^1.
    const glm::vec3 initialScale = m_text.scale->value->getVec3 ();
    const float avgScale = (initialScale.x + initialScale.y) * 0.5f;
    float compensate = 1.0f;
    if (avgScale > 0.0f && avgScale < 1.0f) {
	compensate = std::min (1.0f / avgScale, 32.0f);
    } else if (avgScale >= 1.0f) {
	compensate = std::min (avgScale, 32.0f);
    }
    return std::max<unsigned int> (1u, static_cast<unsigned int> (m_text.pointSize->value->getFloat () * compensate));
}

void CText::initScriptLayer () {
    const auto& script = m_text.text->value->getScriptSource ();

    if (!script.has_value ()) {
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
    // Two-pass rasterization per line: first measure each line's bounding box, then
    // rasterize every glyph into a single grayscale bitmap. Lines split on '\n' are
    // stacked top-to-bottom and each centered horizontally within the overall width.
    FT_GlyphSlot slot = m_ftFace->glyph;

    std::vector<std::string> lines;
    size_t lineStart = 0;
    while (true) {
	const size_t pos = text.find ('\n', lineStart);
	if (pos == std::string::npos) {
	    lines.push_back (text.substr (lineStart));
	    break;
	}
	lines.push_back (text.substr (lineStart, pos - lineStart));
	lineStart = pos + 1;
    }

    struct LineMetrics {
	int width = 0;
	int ascent = 0;
	int descent = 0;
    };

    std::vector<LineMetrics> lineMetrics (lines.size ());
    int maxWidth = 0;
    int maxLineHeight = 0;

    for (size_t i = 0; i < lines.size (); ++i) {
	int penX = 0;
	int maxAscent = 0;
	int maxDescent = 0;

	for (unsigned char c : lines[i]) {
	    if (FT_Load_Char (m_ftFace, static_cast<FT_ULong> (c), FT_LOAD_RENDER) != 0) {
		continue;
	    }
	    penX += slot->advance.x >> 6;
	    maxAscent = std::max (maxAscent, slot->bitmap_top);
	    maxDescent = std::max (maxDescent, static_cast<int> (slot->bitmap.rows) - slot->bitmap_top);
	}

	lineMetrics[i] = { std::max (0, penX), maxAscent, maxDescent };
	maxWidth = std::max (maxWidth, lineMetrics[i].width);
	maxLineHeight = std::max (maxLineHeight, maxAscent + maxDescent);
    }

    // Uniform line pitch (with a little breathing room) based on the tallest line.
    const int linePitch = std::max (1, static_cast<int> (static_cast<float> (maxLineHeight) * 1.2f));
    const int width = std::max (1, maxWidth);
    const int height
	= std::max (1, linePitch * static_cast<int> (lines.size ()) - (linePitch - maxLineHeight));
    std::vector<uint8_t> pixels (static_cast<size_t> (width) * height, 0);

    for (size_t i = 0; i < lines.size (); ++i) {
	int penX = (width - lineMetrics[i].width) / 2;
	const int lineTop = static_cast<int> (i) * linePitch;
	const int maxAscent = lineMetrics[i].ascent;

	for (unsigned char c : lines[i]) {
	    if (FT_Load_Char (m_ftFace, static_cast<FT_ULong> (c), FT_LOAD_RENDER) != 0) {
		continue;
	    }

	    const auto& bmp = slot->bitmap;
	    const int originX = penX + slot->bitmap_left;
	    const int originY = lineTop + maxAscent - slot->bitmap_top;

	    for (unsigned int row = 0; row < bmp.rows; ++row) {
		for (unsigned int col = 0; col < bmp.width; ++col) {
		    const int dstX = originX + static_cast<int> (col);
		    const int dstY = originY + static_cast<int> (row);
		    if (dstX < 0 || dstX >= width || dstY < 0 || dstY >= height) {
			continue;
		    }
		    pixels[static_cast<size_t> (dstY) * width + dstX] = bmp.buffer[row * bmp.pitch + col];
		}
	    }

	    penX += slot->advance.x >> 6;
	}
    }

    const glm::ivec2 previousSize = m_textureSize;

    // FreeType bitmaps store row 0 as the glyph's TOP row; upload as-is and flip
    // when building the quad's V coordinates instead (see uploadQuadVertices).
    std::vector<uint8_t> flipped (pixels.size ());
    for (int row = 0; row < height; ++row) {
	std::copy_n (
	    pixels.begin () + static_cast<long> (row) * width, width,
	    flipped.begin () + static_cast<long> (height - 1 - row) * width
	);
    }

    static_cast<TextGlyphTexture*> (m_glyphTexture.get ())->upload (width, height, flipped.data ());

    m_textureSize = { width, height };
    m_quadSize = { static_cast<float> (width), static_cast<float> (height) };
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
	= this->create (nameA.str (), TextureFormat_ARGB8888, TextureFlags_NoFlags, 1.0f, fboSize, fboSize);
    this->m_currentSubFBO = this->m_subFBO
	= this->create (nameB.str (), TextureFormat_ARGB8888, TextureFlags_NoFlags, 1.0f, fboSize, fboSize);

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
		fboProvider->create (*fbo, TextureFlags_NoFlags, fboSize);
	    }

	    auto curOverride = effect->passOverrides.begin ();
	    const auto endOverride = effect->passOverrides.end ();

	    for (const auto& effectPass : effect->effect->passes) {
		if (!effectPass->material.has_value ()) {
		    // command-only passes (e.g. plain FBO copies) aren't supported for text
		    continue;
		}

		std::shared_ptr<const CFBO> drawTo = this->m_currentSubFBO;

		for (auto& matPass : effectPass->material.value ()->passes) {
		    const auto override = curOverride != endOverride
			? **curOverride
			: std::optional<std::reference_wrapper<const ImageEffectPassOverride>> (std::nullopt);

		    auto* cpass = new CPass (*this, fboProvider, *matPass, override, effectPass->binds, std::nullopt);
		    cpass->setDestination (drawTo);
		    cpass->setInput (asInput);
		    cpass->setPosition (m_copySpacePosition);
		    cpass->setTexCoord (m_texcoordCopy);
		    cpass->setModelMatrix (&m_modelMatrix);
		    cpass->setViewProjectionMatrix (&m_viewProjectionMatrix);
		    cpass->setModelViewProjectionMatrix (&m_modelViewProjectionCopy);
		    cpass->setModelViewProjectionMatrixInverse (&m_modelViewProjectionCopyInverse);
		    m_passes.push_back (cpass);
		}

		if (curOverride != endOverride) {
		    ++curOverride;
		}

		asInput = drawTo;
		std::swap (this->m_currentMainFBO, this->m_currentSubFBO);
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
    }

    const unsigned int pixelSize = computeEffectivePixelSize ();
    if (pixelSize != m_lastPixelSize) {
	m_lastPixelSize = pixelSize;
	FT_Set_Pixel_Sizes (m_ftFace, 0, static_cast<FT_UInt> (m_lastPixelSize));
	rebuildTextureFrom (renderedText);
    } else if (renderedText != m_lastRenderedText) {
	rebuildTextureFrom (renderedText);
    }

    const glm::vec3 scale = m_text.scale->value->getVec3 ();
    const glm::vec3 origin = m_text.origin->value->getVec3 ();

    // origin is the box's center (like every other object in the scene); size/padding/align
    // place the glyph quad within that box, relative to its center. Half-extents use the
    // post-scale size so edges land on the padded box boundary regardless of "scale".
    const float scaledHalfWidth = m_quadSize.x * 0.5f * scale.x;
    const float scaledHalfHeight = m_quadSize.y * 0.5f * scale.y;
    const float padding = static_cast<float> (m_text.padding);

    float offsetX;
    if (m_text.alignment == "left") {
	offsetX = -m_text.size.x * 0.5f + padding + scaledHalfWidth;
    } else if (m_text.alignment == "right") {
	offsetX = m_text.size.x * 0.5f - padding - scaledHalfWidth;
    } else {
	offsetX = 0.0f;
    }

    float offsetY;
    if (m_text.verticalalign == "top") {
	offsetY = -m_text.size.y * 0.5f + padding + scaledHalfHeight;
    } else if (m_text.verticalalign == "bottom") {
	offsetY = m_text.size.y * 0.5f - padding - scaledHalfHeight;
    } else {
	offsetY = 0.0f;
    }

    // WE uses a Y-down coordinate system; match CImage's convention (CImage.cpp's
    // updateScenePosition) of scene_h/2 - y rather than y - scene_h/2.
    const float scene_w = getScene ().getCamera ().getWidth ();
    const float scene_h = getScene ().getCamera ().getHeight ();

    // Match CImage's parallax handling (CImage.cpp:updateScreenSpacePosition), applied here in
    // the same pre-scale, canvas-space units as origin - other objects at the same parallaxDepth
    // (e.g. a background box behind this text) use this exact formula, so text needs it too to
    // stay visually locked to them. Added directly to gl_origin (not appended after the model
    // matrix) so the offset isn't inadvertently multiplied by this object's own "scale".
    glm::vec2 parallaxOffset = { 0.0f, 0.0f };
    if (this->getScene ().getScene ().camera.parallax.enabled
	&& !this->getScene ().getContext ().getApp ().getContext ().settings.mouse.disableparallax) {
	const double parallaxAmount = this->getScene ().getScene ().camera.parallax.amount->value->getFloat ();
	const glm::vec2 depth = m_text.parallaxDepth->value->getVec2 ();
	const glm::vec2* displacement = this->getScene ().getParallaxDisplacement ();
	const float referenceSize = static_cast<float> (this->getScene ().getWidth ());
	parallaxOffset.x = (depth.x + parallaxAmount) * displacement->x * referenceSize;
	parallaxOffset.y = (depth.y + parallaxAmount) * displacement->y * referenceSize;
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

    if (!m_debugLogged) {
	m_debugLogged = true;
	sLog.out (
	    "[text-debug] '", m_text.name, "' pointSize=", m_text.pointSize->value->getFloat (), " scale=", scale.x, ",",
	    scale.y, " pixelSize=", pixelSize, " quadSize=", m_quadSize.x, ",", m_quadSize.y,
	    " scaledHalf=", scaledHalfWidth, ",", scaledHalfHeight, " size=", m_text.size.x, ",", m_text.size.y,
	    " padding=", m_text.padding, " align=", m_text.alignment, "/", m_text.verticalalign, " origin=", origin.x,
	    ",", origin.y, " offset=", offsetX, ",", offsetY, " scene=", scene_w, ",", scene_h, " gl_origin=",
	    gl_origin.x, ",", gl_origin.y, " effects=", m_text.effects.size (), " passes=", m_passes.size ()
	);
    }

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
