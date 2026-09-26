#include "CText.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "WallpaperEngine/Data/Model/DynamicValue.h"
#include "WallpaperEngine/Data/Model/Material.h"
#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Data/Model/UserSetting.h"
#include "WallpaperEngine/Data/Parsers/MaterialParser.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/CFBO.h"
#include "WallpaperEngine/Render/Camera.h"
#include "WallpaperEngine/Render/TextureProvider.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"
#include "WallpaperEngine/Scripting/ScriptEngine.h"

using namespace WallpaperEngine::Render::Objects;
using namespace WallpaperEngine::Render::Objects::Effects;
using WallpaperEngine::Data::Parsers::MaterialParser;

namespace {
// Fallback fonts, used only when fontconfig finds nothing at all
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

// The glyph atlas as a TextureProvider: R8 coverage for plain glyphs, RGBA for MSDF ones
class TextAtlasTexture final : public WallpaperEngine::Render::TextureProvider {
public:
    TextAtlasTexture () {
	glGenTextures (1, &m_textureID);
	glBindTexture (GL_TEXTURE_2D, m_textureID);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    ~TextAtlasTexture () override { glDeleteTextures (1, &m_textureID); }

    void upload (int size, int channels, const uint8_t* pixels) {
	m_size = static_cast<uint32_t> (size);
	m_channels = channels;
	m_resolution = glm::vec4 (static_cast<float> (size));

	glBindTexture (GL_TEXTURE_2D, m_textureID);
	glPixelStorei (GL_UNPACK_ALIGNMENT, 1);

	if (channels == 4) {
	    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	} else {
	    glTexImage2D (GL_TEXTURE_2D, 0, GL_R8, size, size, 0, GL_RED, GL_UNSIGNED_BYTE, pixels);
	}
    }

    [[nodiscard]] GLuint getTextureID (uint32_t) const override { return m_textureID; }
    [[nodiscard]] uint32_t getTextureWidth (uint32_t) const override { return m_size; }
    [[nodiscard]] uint32_t getTextureHeight (uint32_t) const override { return m_size; }
    [[nodiscard]] uint32_t getRealWidth () const override { return m_size; }
    [[nodiscard]] uint32_t getRealHeight () const override { return m_size; }
    [[nodiscard]] TextureFormat getFormat () const override {
	return m_channels == 4 ? TextureFormat_ARGB8888 : TextureFormat_R8;
    }
    [[nodiscard]] uint32_t getFlags () const override { return TextureFlags_ClampUVs; }
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
    [[nodiscard]] bool isReady () const override { return m_size > 0; }

private:
    std::vector<FrameSharedPtr> m_frames;
    glm::vec4 m_resolution = { 0.0f, 0.0f, 0.0f, 0.0f };
    uint32_t m_size = 0;
    int m_channels = 1;
    GLuint m_textureID = GL_NONE;
};

// Final pass: blits the effect-processed buffer onto the actual scene
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

TextAlign parseAlign (const std::string& align) {
    if (align == "left") {
	return TextAlign::Left;
    }
    if (align == "right") {
	return TextAlign::Right;
    }

    return TextAlign::Center;
}
} // namespace

CText::CText (Wallpapers::CScene& scene, const Text& text) :
    CObject (scene, text), CRenderable (scene, text, compositeMaterial ()), ScriptableObject (scene, text),
    m_text (text), m_font (text.font), m_loadedFont (text.font) {
    this->registerProperty ("color", *text.color->value);
    this->registerProperty ("alpha", *text.alpha->value);
    this->registerProperty ("origin", *text.origin->value);
    this->registerProperty ("scale", *text.scale->value);
    this->registerProperty ("visible", *text.visible->value);
    this->registerProperty ("text", *text.text->value);
    this->registerProperty ("font", m_font);
    this->registerProperty ("parallaxDepth", *text.parallaxDepth->value);

    // the text object's property table in wallpaper64.exe 2.8.42 (sub_140258CA0), same names as ITextLayer
    const std::pair<const char*, const UserSettingUniquePtr&> properties[] = {
	{ "pointsize", text.pointSize },
	{ "padding", text.padding },
	{ "spacing", text.spacing },
	{ "horizontalalign", text.horizontalAlign },
	{ "verticalalign", text.verticalAlign },
	{ "anchor", text.anchor },
	{ "limitwidth", text.limitWidth },
	{ "maxwidth", text.maxWidth },
	{ "limitrows", text.limitRows },
	{ "maxrows", text.maxRows },
	{ "limituseellipsis", text.limitUseEllipsis },
	{ "blockalign", text.blockAlign },
	{ "opaquebackground", text.opaqueBackground },
	{ "backgroundcolor", text.backgroundColor },
	{ "backgroundbrightness", text.backgroundBrightness },
	{ "msdf", text.msdf },
	{ "outline", text.outline },
	{ "outlinethickness", text.outlineThickness },
	{ "outlinecolor", text.outlineColor },
	{ "blur", text.blur },
	{ "blursize", text.blurSize },
	{ "dropshadow", text.dropShadow },
	{ "dropshadowsize", text.dropShadowSize },
	{ "dropshadowopacity", text.dropShadowOpacity },
	{ "dropshadowoffset", text.dropShadowOffset },
	{ "dropshadowcolor", text.dropShadowColor },
    };

    for (const auto& [name, setting] : properties) {
	this->registerProperty (name, *setting->value);
    }
    this->registerEffectConstants (text.effects);
}

CText::~CText () {
    if (m_layerHandle != Scripting::kInvalidLayerHandle) {
	this->getScene ().getScriptEngine ().destroyLayer (m_layerHandle);
	m_layerHandle = Scripting::kInvalidLayerHandle;
    }

    this->destroyPasses ();

    for (GLuint* buffer : { &m_glyphPositions, &m_glyphTexcoords, &m_backgroundPositions, &m_passSpacePosition,
			    &m_compositePosition, &m_quadTexcoords }) {
	if (*buffer != 0) {
	    glDeleteBuffers (1, buffer);
	}
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

    if (!loadFont ()) {
	return;
    }

    m_atlas = std::make_shared<TextAtlasTexture> ();
    this->m_texture = m_atlas;

    for (GLuint* buffer : { &m_glyphPositions, &m_glyphTexcoords, &m_backgroundPositions, &m_passSpacePosition,
			    &m_compositePosition, &m_quadTexcoords }) {
	glGenBuffers (1, buffer);
    }

    const GLfloat passSpacePosition[]
	= { -1.0f, 1.0f, 0.0f, -1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f };
    const GLfloat quadTexcoords[] = { 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f };

    glBindBuffer (GL_ARRAY_BUFFER, m_passSpacePosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (passSpacePosition), passSpacePosition, GL_STATIC_DRAW);
    glBindBuffer (GL_ARRAY_BUFFER, m_quadTexcoords);
    glBufferData (GL_ARRAY_BUFFER, sizeof (quadTexcoords), quadTexcoords, GL_STATIC_DRAW);

    // scripted text may start out empty, a space keeps the layout alive until the script produces a value
    this->relayout (text.empty () ? std::string (" ") : text);

    if (scripted) {
	initScriptLayer ();
    }

    CRenderable::setup ();

    m_passLayout = this->currentPassLayout ();
    this->uploadGeometry ();
    buildPasses ();
    m_initialized = true;
}

bool CText::loadFont () {
    // wallpapers packed in .pkg don't expose physical paths, so the font is read into memory
    if (!m_loadedFont.empty () && m_loadedFont.rfind ("systemfont_", 0) != 0) {
	try {
	    auto stream = getAssetLocator ().read (m_loadedFont);
	    stream->seekg (0, std::ios::end);
	    const auto size = stream->tellg ();
	    stream->seekg (0, std::ios::beg);
	    std::vector<uint8_t> data (static_cast<size_t> (size));
	    stream->read (reinterpret_cast<char*> (data.data ()), size);

	    if (m_layout.setPrimaryFont (std::move (data), m_loadedFont)) {
		return true;
	    }
	} catch (const std::exception& e) {
	    sLog.error ("CText: cannot read font '", m_loadedFont, "': ", e.what ());
	}
    }

    // a font WE can't load falls back to arial.ttf
    const bool systemFont = m_loadedFont.rfind ("systemfont_", 0) == 0;
    std::string fontPath = fontconfigMatch (systemFont ? m_loadedFont : std::string ("systemfont_arial"));

    if (fontPath.empty ()) {
	for (const auto& candidate : kFontCandidates) {
	    if (std::filesystem::exists (candidate)) {
		fontPath = candidate;
		break;
	    }
	}
    }

    if (fontPath.empty () || !m_layout.setPrimaryFont ({}, fontPath)) {
	sLog.error ("CText: no usable font found for ", m_text.name);
	return false;
    }

    return true;
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

TextLayoutParams CText::currentParams () const {
    // sub_140256F20: any of the glyph effects switches the whole text to MSDF glyphs
    const bool msdf = m_text.msdf->value->getBool () || m_text.outline->value->getBool ()
	|| m_text.blur->value->getBool () || m_text.dropShadow->value->getBool ();

    return {
	.size = std::clamp (m_text.pointSize->value->getFloat (), 1.0f, 256.0f),
	.spacing = m_text.spacing->value->getVec2 (),
	.msdf = msdf,
	.align = parseAlign (m_text.horizontalAlign->value->getString ()),
	.maxWidth = m_text.limitWidth->value->getBool () ? m_text.maxWidth->value->getFloat () : 0.0f,
	.maxRows = m_text.limitRows->value->getBool () ? m_text.maxRows->value->getInt () : 0,
	.ellipsis = m_text.limitUseEllipsis->value->getBool (),
	.blockAlign = m_text.blockAlign->value->getBool (),
    };
}

glm::vec2 CText::currentPadding () const {
    const glm::vec2 padding = m_text.padding->value->getVec2 ();
    return { std::min (padding.x, 512.0f), std::min (padding.y, 512.0f) };
}

// sub_1402585C0 with the margins sub_140183A70 computes: how much of the scene the output crops off each edge (negative
// when it letterboxes), so a text anchored to an edge or corner keeps its distance to what is actually visible there
glm::vec2 CText::screenAnchorOffset () const {
    static const std::vector<std::pair<std::string, glm::ivec2>> anchors = {
	{ "center", { 0, 0 } },	      { "top", { 0, 1 } },	      { "topright", { 1, 1 } },
	{ "right", { 1, 0 } },	      { "bottomright", { 1, -1 } }, { "bottom", { 0, -1 } },
	{ "bottomleft", { -1, -1 } }, { "left", { -1, 0 } },	      { "topleft", { -1, 1 } },
    };

    const std::string& name = m_text.anchor->value->getString ();
    const auto it = std::ranges::find_if (anchors, [&name] (const auto& anchor) { return anchor.first == name; });
    const auto& state = this->getScene ().getState ();

    if (it == anchors.end () || state.getViewportWidth () <= 0 || state.getViewportHeight () <= 0) {
	return { 0.0f, 0.0f };
    }

    const auto& camera = this->getScene ().getCamera ();
    const auto uvs = state.getTextureUVs ();
    const float canvasWidth = camera.getCanvasWidth ();
    const float canvasHeight = camera.getCanvasHeight ();
    // --expand-canvas grows the canvas evenly around the scene
    const float overhangX = (canvasWidth - camera.getWidth ()) * 0.5f;
    const float overhangY = (canvasHeight - camera.getHeight ()) * 0.5f;
    const float left = uvs.ustart * canvasWidth - overhangX;
    const float right = (1.0f - uvs.uend) * canvasWidth - overhangX;
    // v runs top down in a flipped state, bottom up otherwise
    const float top = (state.isVFlipped () ? uvs.vstart : 1.0f - uvs.vstart) * canvasHeight - overhangY;
    const float bottom = (state.isVFlipped () ? 1.0f - uvs.vend : uvs.vend) * canvasHeight - overhangY;

    const auto axis = [] (int side, float low, float high) {
	// low is the left/bottom margin, high the right/top one; y is up
	return side < 0 ? low : side > 0 ? -high : (low - high) * 0.5f;
    };

    return { axis (it->second.x, left, right), axis (it->second.y, bottom, top) };
}

CText::PassLayout CText::currentPassLayout () const {
    const auto& debug = this->getScene ().getContext ().getApp ().getContext ().settings.render.debug;
    const bool dropShadow = m_text.dropShadow->value->getBool ();
    PassLayout layout {
	.msdf = m_params.msdf,
	.outline = m_params.msdf && m_text.outline->value->getBool (),
	.blur = m_params.msdf && m_text.blur->value->getBool (),
	.dropShadow = m_params.msdf && dropShadow,
	.background = m_text.opaqueBackground->value->getBool (),
	.blendMode = m_text.colorBlendMode->value->getInt (),
    };

    // sub_1401E6F50: a blend mode other than 0 and 31 (additive) makes the text composite through
    // effectpassthrough, like having effects
    // fog too (renderer flags 0x1800000)
    const bool fog = this->getScene ().hasDistanceFog () || this->getScene ().hasHeightFog ();
    layout.buffered
	= (!m_text.effects.empty () || (layout.blendMode != 0 && layout.blendMode != 31) || fog) && !debug.baseOnly;

    // sub_140258900: text with effects renders into a buffer of its box plus padding on every side
    if (layout.buffered && m_result.valid) {
	const glm::vec2 padding = this->currentPadding ();
	layout.bufferSize = {
	    std::max (1, static_cast<int> (padding.x * 2.0f + (m_result.maxX - m_result.minX))),
	    std::max (1, static_cast<int> (padding.y * 2.0f + (m_result.top - m_result.bottom))),
	};
    }

    return layout;
}

void CText::relayout (const std::string& text) {
    m_params = this->currentParams ();
    m_result = m_layout.layout (text, m_params);
    m_lastRenderedText = text;

    if (m_layout.takeAtlasChanged ()) {
	static_cast<TextAtlasTexture*> (m_atlas.get ())
	    ->upload (m_layout.getAtlasSize (), m_layout.getAtlasChannels (), m_layout.getAtlasPixels ().data ());
    }

    this->uploadGeometry ();
}

void CText::uploadGeometry () {
    std::vector<GLfloat> positions;
    std::vector<GLfloat> texcoords;
    positions.reserve (m_result.quads.size () * 18);
    texcoords.reserve (m_result.quads.size () * 12);

    // two triangles per glyph, WE's index order 0 2 1 / 1 2 3 over top left, top right, bottom left, bottom right
    for (const auto& quad : m_result.quads) {
	const auto [x0, y0, x1, y1] = std::array { quad.rect.x, quad.rect.y, quad.rect.z, quad.rect.w };
	const auto [u0, v0, u1, v1] = std::array { quad.uv.x, quad.uv.y, quad.uv.z, quad.uv.w };

	positions.insert (positions.end (), { x0, y1, 0.0f, x0, y0, 0.0f, x1, y1, 0.0f,
					      x1, y1, 0.0f, x0, y0, 0.0f, x1, y0, 0.0f });
	texcoords.insert (texcoords.end (), { u0, v0, u0, v1, u1, v0, u1, v0, u0, v1, u1, v1 });
    }

    m_glyphVertexCount = static_cast<GLsizei> (m_result.quads.size () * 6);

    glBindBuffer (GL_ARRAY_BUFFER, m_glyphPositions);
    glBufferData (GL_ARRAY_BUFFER, static_cast<GLsizeiptr> (positions.size () * sizeof (GLfloat)), positions.data (), GL_DYNAMIC_DRAW);
    glBindBuffer (GL_ARRAY_BUFFER, m_glyphTexcoords);
    glBufferData (GL_ARRAY_BUFFER, static_cast<GLsizeiptr> (texcoords.size () * sizeof (GLfloat)), texcoords.data (), GL_DYNAMIC_DRAW);

    // the opaque background covers the text box plus padding (sub_140258050)
    const glm::vec2 padding = this->currentPadding ();
    const float bx0 = m_result.minX - padding.x;
    const float by0 = m_result.bottom - padding.y;
    const float bx1 = m_result.maxX + padding.x;
    const float by1 = m_result.top + padding.y;
    const GLfloat background[] = { bx0, by1, 0.0f, bx0, by0, 0.0f, bx1, by1, 0.0f, bx1, by1, 0.0f, bx0, by0, 0.0f, bx1, by0, 0.0f };

    glBindBuffer (GL_ARRAY_BUFFER, m_backgroundPositions);
    glBufferData (GL_ARRAY_BUFFER, sizeof (background), background, GL_DYNAMIC_DRAW);

    // the effect buffer's composite quad, centered like the text box it holds
    const float hx = std::max (1.0f, static_cast<float> (m_passLayout.bufferSize.x)) * 0.5f;
    const float hy = std::max (1.0f, static_cast<float> (m_passLayout.bufferSize.y)) * 0.5f;
    const GLfloat composite[] = { -hx, -hy, 0.0f, -hx, hy, 0.0f, hx, -hy, 0.0f, hx, -hy, 0.0f, -hx, hy, 0.0f, hx, hy, 0.0f };

    glBindBuffer (GL_ARRAY_BUFFER, m_compositePosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (composite), composite, GL_DYNAMIC_DRAW);
}

// sub_1401B3B60: g_RenderVar0..3 of materials/fonts/font.frag in atlas pixels (32 per em), clamped like WE
void CText::updateRenderVars () {
    const bool outline = m_text.outline->value->getBool ();
    const bool blur = m_text.blur->value->getBool ();
    const bool dropShadow = m_text.dropShadow->value->getBool ();
    const float thickness = m_text.outlineThickness->value->getFloat ();
    const float outlineWidth = outline ? (thickness <= 1.0f ? 1.0f : thickness) : 0.0f;
    const float blurRadius = blur ? std::max (m_text.blurSize->value->getFloat (), 0.01f) : 0.0f;
    const float shadowRadius = dropShadow ? std::max (m_text.dropShadowSize->value->getFloat (), 0.01f) : 0.0f;
    const glm::vec2 shadowOffset = dropShadow ? m_text.dropShadowOffset->value->getVec2 () : glm::vec2 (0.0f);
    const float toAtlas = 32.0f / m_params.size * 0.24f;
    constexpr float kMaxOutlineAndBlur = 5.1000004f;

    m_renderVars[0] = {
	24.0f,
	std::min (kMaxOutlineAndBlur, toAtlas * outlineWidth),
	std::min (6.0f, toAtlas * blurRadius),
	std::min (6.0f, toAtlas * shadowRadius),
    };
    m_renderVars[1] = glm::vec4 (m_text.outlineColor->value->getVec3 (), std::min (6.0f, toAtlas * shadowOffset.x));
    m_renderVars[2] = glm::vec4 (m_text.dropShadowColor->value->getVec3 (), std::min (6.0f, toAtlas * shadowOffset.y));
    m_renderVars[3] = { m_text.dropShadowOpacity->value->getFloat (), 0.0f, 0.0f, 0.0f };

    if (m_renderVars[0].z + m_renderVars[0].y > kMaxOutlineAndBlur) {
	m_renderVars[0].y = std::max (kMaxOutlineAndBlur - m_renderVars[0].z, 0.0f);
    }
}

CPass* CText::createFontPass (const std::shared_ptr<const CFBO>& destination, const glm::mat4* mvp) {
    auto* pass = new CPass (
	*this, std::make_shared<FBOProvider> (this), **m_fontMaterial->passes.begin (), m_fontOverride, std::nullopt,
	std::nullopt
    );
    pass->setDestination (destination);
    pass->setInput (m_atlas);
    pass->setPosition (m_glyphPositions);
    pass->setTexCoord (m_glyphTexcoords);
    pass->setModelMatrix (&m_modelMatrix);
    pass->setViewProjectionMatrix (&m_viewProjectionMatrix);
    pass->setModelViewProjectionMatrix (mvp);
    pass->setModelViewProjectionMatrixInverse (mvp);
    pass->setGeometryCallback (nullptr, [this] () { glDrawArrays (GL_TRIANGLES, 0, m_glyphVertexCount); }, nullptr);

    for (int i = 0; i < 4; i++) {
	pass->addUniform ("g_RenderVar" + std::to_string (i), &m_renderVars[i]);
    }

    return pass;
}

void CText::buildPasses () {
    this->destroyPasses ();

    const auto& project = this->getScene ().getScene ().project;
    const PassLayout& layout = m_passLayout;

    // sub_1401B3430 picks the material by atlas kind, the effect combos come from sub_1401B3B60
    try {
	m_fontMaterial = MaterialParser::load (
	    project, layout.msdf ? "materials/fonts/basefont_msdf.json" : "materials/fonts/basefont.json"
	);
	m_backgroundMaterial = layout.background ? MaterialParser::load (project, "materials/fonts/fontbackground.json")
						 : nullptr;
	m_clearAlphaMaterial = layout.buffered && !layout.background
	    ? MaterialParser::load (project, "materials/util/composelayer_clearalpha.json")
	    : nullptr;
	// scenes from version 3 on composite with genericimage4, the one with fog (sub_140257840)
	m_passthroughMaterial = layout.buffered
		&& ((layout.blendMode != 0 && layout.blendMode != 31) || this->getScene ().hasDistanceFog ()
		    || this->getScene ().hasHeightFog ())
	    ? MaterialParser::load (
		  project,
		  project.sceneVersion >= 3 ? "materials/util/effectpassthrough_4.json" : "materials/util/effectpassthrough.json"
	      )
	    : nullptr;
    } catch (const std::exception& e) {
	sLog.error ("CText: cannot load the font materials for ", m_text.name, ": ", e.what ());
	return;
    }

    m_fontOverride.combos.clear ();

    if (layout.outline) {
	m_fontOverride.combos.emplace ("OUTLINE_ENABLED", 1);
    }
    if (layout.blur) {
	m_fontOverride.combos.emplace ("BLUR_ENABLED", 1);
    }
    if (layout.dropShadow) {
	m_fontOverride.combos.emplace ("DROP_SHADOW_ENABLED", 1);
    }

    if (!layout.buffered) {
	// no effects: background and glyphs go straight into the scene like sub_140258050
	if (m_backgroundMaterial != nullptr) {
	    auto* background = new CPass (
		*this, std::make_shared<FBOProvider> (this), **m_backgroundMaterial->passes.begin (), std::nullopt,
		std::nullopt, std::nullopt
	    );
	    background->setDestination (this->getScene ().getFBO ());
	    // the flat shader samples nothing, but passes without an input are skipped
	    background->setInput (m_atlas);
	    background->setPosition (m_backgroundPositions);
	    background->setTexCoord (m_quadTexcoords);
	    background->setModelMatrix (&m_modelMatrix);
	    background->setViewProjectionMatrix (&m_viewProjectionMatrix);
	    background->setModelViewProjectionMatrix (&m_glyphSceneMatrix);
	    background->setModelViewProjectionMatrixInverse (&m_glyphSceneMatrixInverse);
	    background->addUniform ("g_Color", &m_backgroundColor);
	    background->addUniform ("g_Alpha", &m_backgroundAlpha);
	    background->addUniform ("g_Color4", &m_backgroundColor4);

	    // colorBlendMode 31 draws the text and its background additively (sub_140258050)
	    if (layout.blendMode == 31) {
		background->setBlendingMode (BlendingMode_Additive);
	    }

	    m_passes.push_back (background);
	}

	auto* glyphs = this->createFontPass (this->getScene ().getFBO (), &m_glyphSceneMatrix);

	if (layout.blendMode == 31) {
	    glyphs->setBlendingMode (BlendingMode_Additive);
	}

	m_passes.push_back (glyphs);
	return;
    }

    if (layout.bufferSize.x <= 0 || layout.bufferSize.y <= 0) {
	return;
    }

    const glm::vec2 fboSize = { static_cast<float> (layout.bufferSize.x), static_cast<float> (layout.bufferSize.y) };

    std::ostringstream nameA, nameB;
    nameA << "_rt_textComposite_" << this->getId () << "_a";
    nameB << "_rt_textComposite_" << this->getId () << "_b";

    this->m_currentMainFBO = this->m_mainFBO
	= this->create (nameA.str (), TextureFormat_ARGB8888, TextureFlags_ClampUVs, 1.0f, fboSize, fboSize);
    this->m_currentSubFBO = this->m_subFBO
	= this->create (nameB.str (), TextureFormat_ARGB8888, TextureFlags_ClampUVs, 1.0f, fboSize, fboSize);

    // sub_140257C30: the buffer starts out as the opaque background, or as the scene behind the text with alpha 0
    // (composelayer_clearalpha), so the glyphs' translucent edges blend towards what they will be drawn over
    auto* base = this->createFontPass (m_currentMainFBO, &m_glyphBufferMatrix);

    if (layout.background) {
	base->setClearColor (&m_backgroundColor4);
    } else if (m_clearAlphaMaterial != nullptr) {
	auto* clear = new CPass (
	    *this, std::make_shared<FBOProvider> (this), **m_clearAlphaMaterial->passes.begin (), std::nullopt,
	    std::nullopt, std::nullopt
	);
	clear->setDestination (m_currentMainFBO);
	clear->setInput (this->getScene ().getFBO ());
	clear->setPosition (m_compositePosition);
	clear->setTexCoord (m_quadTexcoords);
	clear->setModelMatrix (&m_modelMatrix);
	clear->setViewProjectionMatrix (&m_viewProjectionMatrix);
	clear->setModelViewProjectionMatrix (&m_compositeMatrix);
	clear->setModelViewProjectionMatrixInverse (&m_compositeMatrixInverse);
	m_passes.push_back (clear);
	base->setKeepDestination (true);
    }

    m_passes.push_back (base);

    std::shared_ptr<const TextureProvider> asInput = m_currentMainFBO;

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
		cpass->setTexCoord (m_quadTexcoords);
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

    // final pass: composite the accumulated result onto the actual scene. With a blend mode that is WE's
    // effectpassthrough material with BLENDMODE (sub_140257840), drawn translucent in white at full alpha
    // (sub_1401E8AA0); colorBlendMode 31 is a plain additive composite
    m_passthroughOverride.combos.clear ();

    if (m_passthroughMaterial != nullptr) {
	m_passthroughOverride.combos.emplace ("BLENDMODE", layout.blendMode == 31 ? 0 : layout.blendMode);
	m_passthroughOverride.combos.emplace ("FOG_COMPUTED", 1);
    }

    const Material& finalMaterial = m_passthroughMaterial != nullptr ? *m_passthroughMaterial : compositeMaterial ();

    for (const auto& pass : finalMaterial.passes) {
	auto* cpass = m_passthroughMaterial != nullptr
	    ? new CPass (*this, std::make_shared<FBOProvider> (this), *pass, m_passthroughOverride, std::nullopt, std::nullopt)
	    : new CPass (*this, std::make_shared<FBOProvider> (this), *pass, std::nullopt, std::nullopt, std::nullopt);

	if (m_passthroughMaterial != nullptr) {
	    cpass->setBlendingMode (BlendingMode_Translucent);
	    cpass->addUniform ("g_Color4", &m_white);
	    cpass->addUniform ("g_EyePosition", &this->getScene ().getFog ().eyeLocal);
	} else if (layout.blendMode == 31) {
	    cpass->setBlendingMode (BlendingMode_Additive);
	}

	cpass->setDestination (this->getScene ().getFBO ());
	cpass->setInput (asInput);
	cpass->setPosition (m_compositePosition);
	cpass->setTexCoord (m_quadTexcoords);
	// the fog of the composite measures where the text is in the scene
	cpass->setModelMatrix (m_passthroughMaterial != nullptr ? &m_compositeModel : &m_modelMatrix);
	cpass->setViewProjectionMatrix (&m_viewProjectionMatrix);
	cpass->setModelViewProjectionMatrix (&m_compositeMatrix);
	cpass->setModelViewProjectionMatrixInverse (&m_compositeMatrixInverse);
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

    const std::string& requestedFont = m_font.getString ().empty () ? m_text.font : m_font.getString ();
    bool fontChanged = false;
    if (requestedFont != m_loadedFont) {
	m_loadedFont = requestedFont;
	fontChanged = true;

	if (!loadFont ()) {
	    return;
	}
    }

    if (!m_layout.hasFont ()) {
	return;
    }

    // pointsize, spacing, wrapping and the glyph effects can all be bound to user properties or scripts
    if (fontChanged || renderedText != m_lastRenderedText || this->currentParams () != m_params) {
	this->relayout (renderedText);
    }

    const PassLayout passLayout = this->currentPassLayout ();
    if (passLayout != m_passLayout) {
	m_passLayout = passLayout;
	this->uploadGeometry ();
	this->buildPasses ();
    }

    if (!m_result.valid || m_passes.empty ()) {
	return;
    }

    this->updateRenderVars ();

    const float alpha = m_text.alpha->value->getFloat ();
    // backgroundbrightness only counts in WE's HDR scene rendering
    m_backgroundColor = m_text.backgroundColor->value->getVec3 ()
	* (this->getScene ().isHDR () ? m_text.backgroundBrightness->value->getFloat () : 1.0f);
    m_backgroundAlpha = alpha;
    m_backgroundColor4 = glm::vec4 (m_backgroundColor, alpha);

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

    // the screen anchor is applied on the matrix stack below the object's world matrix (sub_1401E8AA0), in scene units
    const glm::vec2 screenAnchor = this->screenAnchorOffset ();
    origin.x += screenAnchor.x;
    origin.y += screenAnchor.y;

    // sub_140256F20: the box is centered on the object, then moved by an anchor offset from the alignment
    // (y up): left/right put that edge on the origin, top puts the first line's ascender there, bottom the last
    // line's descender, center the middle between the first ascender and the last baseline
    const float boxWidth = m_result.maxX - m_result.minX;
    const float boxHeight = m_result.top - m_result.bottom;
    const float boxCenter = m_result.top - boxHeight * 0.5f;
    const float extraLines = static_cast<float> (m_result.lines - 1) * m_result.pitch;
    glm::vec2 anchor (0.0f);

    if (m_params.align == TextAlign::Left) {
	anchor.x = boxWidth * 0.5f;
    } else if (m_params.align == TextAlign::Right) {
	anchor.x = -boxWidth * 0.5f;
    }

    const std::string& verticalAlign = m_text.verticalAlign->value->getString ();

    if (verticalAlign == "bottom") {
	anchor.y = boxCenter - (m_result.descender - extraLines);
    } else if (verticalAlign == "top") {
	anchor.y = boxCenter - m_result.ascender;
    } else {
	anchor.y = boxCenter - (m_result.ascender - extraLines) * 0.5f;
    }

    const float offsetX = anchor.x * scale.x;
    const float offsetY = anchor.y * scale.y;
    const float scaledHalfWidth = boxWidth * 0.5f * scale.x;
    const float scaledHalfHeight = boxHeight * 0.5f * scale.y;

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

    const auto& camera = getScene ().getCamera ();
    // "perspective" text gets the perspective layer camera (sub_14025FAF0 -> sub_1401E5B60)
    const glm::mat4 viewProjection = camera.isOrthogonal () && m_text.perspective->value->getBool ()
	? camera.getPerspectiveLayerViewProjection ()
	: camera.getProjection () * camera.getLookAt ();
    const glm::mat4 model = glm::scale (glm::translate (glm::mat4 (1.0f), gl_origin), scale);

    // layout space is y up, first baseline at 0; sub_140258050 centers the box: x - w/2 - min(minX, 0), y + h/2 - top
    const glm::vec3 center = { -boxWidth * 0.5f - std::min (m_result.minX, 0.0f), boxHeight * 0.5f - m_result.top, 0.0f };

    m_glyphSceneMatrix = viewProjection * glm::translate (glm::scale (model, glm::vec3 (1.0f, -1.0f, 1.0f)), center);
    m_glyphSceneMatrixInverse = glm::inverse (m_glyphSceneMatrix);
    m_compositeMatrix = viewProjection * model;
    m_compositeModel = model;
    m_compositeMatrixInverse = glm::inverse (m_compositeMatrix);

    if (m_passLayout.buffered) {
	// sub_140257D70: inside the buffer the box starts at the padding
	const glm::vec2 padding = this->currentPadding ();
	const glm::vec2 size = m_passLayout.bufferSize;

	m_glyphBufferMatrix = glm::translate (
	    glm::ortho<float> (0.0f, size.x, 0.0f, size.y),
	    glm::vec3 (padding.x - std::min (m_result.minX, 0.0f), padding.y - m_result.bottom, 0.0f)
	);
	m_glyphBufferMatrixInverse = glm::inverse (m_glyphBufferMatrix);
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

const glm::vec3& CText::getColor () const {
    // brightness only scales the color in HDR scene rendering (renderer flag 0x2000)
    m_colorCache = m_text.color->value->getVec3 ()
	* (this->getScene ().isHDR () ? m_text.brightness->value->getFloat () : 1.0f);
    return m_colorCache;
}

const glm::vec4& CText::getColor4 () const {
    m_color4Cache = glm::vec4 (this->getColor (), m_text.alpha->value->getFloat ());
    return m_color4Cache;
}

const glm::vec3& CText::getCompositeColor () const { return m_text.color->value->getVec3 (); }
