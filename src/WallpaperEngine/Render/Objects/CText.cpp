#include "CText.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <strings.h>
#include <unordered_map>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "WallpaperEngine/Assets/AssetLocator.h"
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

// The first of these families fontconfig has installed under that very name (fc-match always answers with something)
std::string installedFamily (const std::vector<std::string>& families) {
    static std::unordered_map<std::string, std::string> cache;

    for (const auto& family : families) {
	if (const auto it = cache.find (family); it != cache.end ()) {
	    if (!it->second.empty ()) {
		return it->second;
	    }

	    continue;
	}

	const std::string command = "fc-match -f '%{family}\\n%{file}' '" + family + "' 2>/dev/null";
	FILE* pipe = popen (command.c_str (), "r");
	std::string output;

	if (pipe != nullptr) {
	    char buffer[512];

	    while (fgets (buffer, sizeof (buffer), pipe) != nullptr) {
		output += buffer;
	    }

	    pclose (pipe);
	}

	std::string path;
	const size_t newline = output.find ('\n');

	if (newline != std::string::npos) {
	    std::stringstream names (output.substr (0, newline));
	    std::string name;

	    while (std::getline (names, name, ',')) {
		if (strcasecmp (name.c_str (), family.c_str ()) == 0) {
		    path = output.substr (newline + 1);
		    break;
		}
	    }
	}

	if (!path.empty () && !std::filesystem::exists (path)) {
	    path.clear ();
	}

	cache.emplace (family, path);

	if (!path.empty ()) {
	    return path;
	}
    }

    return {};
}

// wallpaper64.exe's fallback fonts (off_140484C40, tried per character by sub_1401AD670): Windows fonts from the
// system font folder and the assets' Twemoji. Here the Windows fonts count when they are installed under their own
// family, arial.ttf also through its metric compatible clones like the systemfont_arial match, and the CJK ones
// through the usual Linux CJK families since Windows always has YaHei/Malgun but Linux never does
std::vector<TextFontSource> weFallbackFonts (const WallpaperEngine::Assets::AssetLocator& assets) {
    static std::shared_ptr<const std::vector<uint8_t>> twemoji;
    static bool twemojiRead = false;

    if (!twemojiRead) {
	twemojiRead = true;

	try {
	    auto stream = assets.read ("fonts/TwemojiMozilla.ttf");
	    stream->seekg (0, std::ios::end);
	    const auto size = stream->tellg ();
	    stream->seekg (0, std::ios::beg);
	    std::vector<uint8_t> data (static_cast<size_t> (size));
	    stream->read (reinterpret_cast<char*> (data.data ()), size);
	    twemoji = std::make_shared<const std::vector<uint8_t>> (std::move (data));
	} catch (const std::exception& e) {
	    sLog.error ("CText: cannot read fonts/TwemojiMozilla.ttf: ", e.what ());
	}
    }

    const std::vector<std::vector<std::string>> before = {
	{ "Arial", "Liberation Sans", "Arimo" },
	{ "Segoe UI Emoji" },
	{ "Arial Unicode MS" },
	{ "Segoe UI" },
    };
    const std::vector<std::vector<std::string>> after = {
	{ "Segoe UI Symbol" },
	{ "Microsoft YaHei", "Noto Sans CJK SC", "Source Han Sans SC", "WenQuanYi Zen Hei", "Droid Sans Fallback" },
	{ "Malgun Gothic", "Noto Sans CJK KR", "Source Han Sans KR" },
    };
    std::vector<TextFontSource> fonts;

    for (const auto& families : before) {
	if (std::string path = installedFamily (families); !path.empty ()) {
	    fonts.push_back ({ nullptr, std::move (path) });
	}
    }

    if (twemoji != nullptr) {
	fonts.push_back ({ twemoji, "fonts/TwemojiMozilla.ttf" });
    }

    for (const auto& families : after) {
	if (std::string path = installedFamily (families); !path.empty ()) {
	    fonts.push_back ({ nullptr, std::move (path) });
	}
    }

    return fonts;
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

// Mirrors CImage.cpp's clampParallaxAxis: keeps an edge pair from sliding into the on-screen part
// of the canvas once `offset` is added to both; a box too small to cover it on this axis has no
// ground to uncover and moves freely.
float clampParallaxAxis (float offset, float edgeA, float edgeB, float visibleLow, float visibleHigh) {
    const float low = std::min (edgeA, edgeB);
    const float high = std::max (edgeA, edgeB);
    const float maxOffset = visibleLow - low;
    const float minOffset = visibleHigh - high;

    if (minOffset > maxOffset) {
	return offset;
    }

    // a layer that doesn't cover the screen at rest stays where the scene puts it (3621923790's hair), the clamp
    // only keeps the parallax from uncovering more
    return std::clamp (offset, std::min (minOffset, 0.0f), std::max (maxOffset, 0.0f));
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
	{ "depthtest", text.depthTest },
    };

    for (const auto& [name, setting] : properties) {
	this->registerProperty (name, *setting->value);
    }
    this->registerProperty ("copybackground", *text.copyBackground->value);
    this->registerRenderableProperties (text.renderable, *text.colorBlendMode, *text.brightness);
    this->registerEffectConstants (text.effects);
}

CText::~CText () {
    if (m_layerHandle != Scripting::kInvalidLayerHandle) {
	this->getScene ().getScriptEngine ().destroyLayer (m_layerHandle);
	m_layerHandle = Scripting::kInvalidLayerHandle;
    }

    this->destroyPasses ();

    for (GLuint* buffer :
	 { &m_glyphPositions, &m_glyphTexcoords, &m_colorGlyphPositions, &m_colorGlyphTexcoords, &m_backgroundPositions,
	   &m_passSpacePosition, &m_compositePosition, &m_quadTexcoords, &m_compositeTexcoords, &m_finalTexcoords }) {
	if (*buffer != 0) {
	    glDeleteBuffers (1, buffer);
	}
    }
}

void CText::destroyPasses () {
    this->releaseEffectMaterials ();

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

    // sub_140186C90 flags a text (object type 4) that another object lists in its dependencies once every object
    // exists, before the first frame
    m_isDependency = std::ranges::any_of (this->getScene ().getScene ().objects, [this] (const auto& object) {
	return object->id != m_text.id
	    && std::ranges::find (object->dependencies, m_text.id) != object->dependencies.end ();
    });

    m_layout.setFallbackFonts (weFallbackFonts (this->getAssetLocator ()));

    if (!loadFont ()) {
	return;
    }

    m_atlas = std::make_shared<TextAtlasTexture> ();
    m_colorAtlas = std::make_shared<TextAtlasTexture> ();
    m_colorTexture = std::make_shared<TextAtlasTexture> ();
    this->m_texture = m_atlas;

    for (GLuint* buffer :
	 { &m_glyphPositions, &m_glyphTexcoords, &m_colorGlyphPositions, &m_colorGlyphTexcoords, &m_backgroundPositions,
	   &m_passSpacePosition, &m_compositePosition, &m_quadTexcoords, &m_compositeTexcoords, &m_finalTexcoords }) {
	glGenBuffers (1, buffer);
    }

    const GLfloat passSpacePosition[] = { -1.0f, 1.0f, 0.0f, -1.0f, -1.0f, 0.0f, 1.0f, 1.0f,  0.0f,
					  1.0f,  1.0f, 0.0f, -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f };
    const GLfloat quadTexcoords[] = { 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f };

    glBindBuffer (GL_ARRAY_BUFFER, m_passSpacePosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (passSpacePosition), passSpacePosition, GL_STATIC_DRAW);
    glBindBuffer (GL_ARRAY_BUFFER, m_quadTexcoords);
    glBufferData (GL_ARRAY_BUFFER, sizeof (quadTexcoords), quadTexcoords, GL_STATIC_DRAW);
    // the text buffer's rows run top down like WE's (v 0 at the top of the box), the composite quad's y runs down
    const GLfloat compositeTexcoords[] = { 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f };
    glBindBuffer (GL_ARRAY_BUFFER, m_compositeTexcoords);
    glBufferData (GL_ARRAY_BUFFER, sizeof (compositeTexcoords), compositeTexcoords, GL_STATIC_DRAW);

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

glm::vec2 CText::measuredSize () const {
    if (!m_result.valid) {
	return { 2.0f, 2.0f };
    }

    const glm::vec2 padding = m_passLayout.buffered ? this->currentPadding () : glm::vec2 (0.0f);
    return { m_result.maxX - m_result.minX + padding.x * 2.0f, m_result.top - m_result.bottom + padding.y * 2.0f };
}

glm::vec2 CText::currentPadding () const {
    const glm::vec2 padding = m_text.padding->value->getVec2 ();
    return { std::min (padding.x, 512.0f), std::min (padding.y, 512.0f) };
}

// sub_1402585C0 with the margins sub_140183A70 computes: how much of the scene the output crops off each edge (negative
// when it letterboxes), so a text anchored to an edge or corner keeps its distance to what is actually visible there
glm::vec2 CText::screenAnchorOffset () const {
    static const std::vector<std::pair<std::string, glm::ivec2>> anchors = {
	{ "center", { 0, 0 } },       { "top", { 0, 1 } },          { "topright", { 1, 1 } },
	{ "right", { 1, 0 } },        { "bottomright", { 1, -1 } }, { "bottom", { 0, -1 } },
	{ "bottomleft", { -1, -1 } }, { "left", { -1, 0 } },        { "topleft", { -1, 1 } },
    };

    const std::string& name = m_text.anchor->value->getString ();
    const auto it = std::ranges::find_if (anchors, [&name] (const auto& anchor) { return anchor.first == name; });
    const auto& state = this->getScene ().getState ();

    if (it == anchors.end () || state.getViewportWidth () <= 0 || state.getViewportHeight () <= 0) {
	return { 0.0f, 0.0f };
    }

    const auto& camera = this->getScene ().getCamera ();
    const glm::vec4 margins = this->getScene ().getVisibleMargins ();
    // --expand-canvas grows the canvas evenly around the scene
    const float overhangX = (camera.getCanvasWidth () - camera.getWidth ()) * 0.5f;
    const float overhangY = (camera.getCanvasHeight () - camera.getHeight ()) * 0.5f;
    const float left = margins.x - overhangX;
    const float right = margins.y - overhangX;
    const float bottom = margins.z - overhangY;
    const float top = margins.w - overhangY;

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
	.color = !m_result.colorQuads.empty (),
	// the _depth materials outside orthographic scenes (renderer flag 0x400) unless depthtest is "disabled" (+1440,
	// enum table "disabled" 1 / "enabled" 0, any other string is the first entry)
	.depth = this->getScene ().getCamera ().isPerspective () && m_text.depthTest->value->getString () == "enabled",
	.blendMode = m_text.colorBlendMode->value->getInt (),
    };

    // sub_1401E6F50: a blend mode other than 0 and 31 (additive) makes the text composite through
    // effectpassthrough, like having effects, and so does fog (renderer flags 0x1800000 = scene fog flags 0x4000
    // and 0x8000, sub_140186440). Its other trigger, image flag 0x100, is never set in 2.8.42. The scene loader adds
    // 0x1010 to a text another object lists in its dependencies (sub_140186C90), the buffer is what it reads
    const bool fog = this->getScene ().hasDistanceFog () || this->getScene ().hasHeightFog ();
    layout.passthrough = (layout.blendMode != 0 && layout.blendMode != 31) || fog || m_isDependency;
    // sub_140258900: buffered for visible effects or flag 0x10
    layout.buffered
	= (std::ranges::any_of (m_text.effects, [] (const auto& effect) { return effect->visible->value->getBool (); })
	   || layout.passthrough)
	&& !debug.baseOnly;

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

    if (m_layout.takeColorAtlasChanged ()) {
	static_cast<TextAtlasTexture*> (m_colorAtlas.get ())
	    ->upload (m_layout.getColorAtlasSize (), 4, m_layout.getColorAtlasPixels ().data ());

	if (m_layout.getColorScale () > 0) {
	    static_cast<TextAtlasTexture*> (m_colorTexture.get ())
		->upload (
		    m_layout.getColorAtlasSize () * m_layout.getColorScale (), 4,
		    m_layout.getColorTexturePixels ().data ()
		);
	}
    }

    this->uploadGeometry ();
}

void CText::uploadGeometry () {
    const auto upload = [] (const std::vector<TextGlyphQuad>& quads, GLuint positionBuffer, GLuint texcoordBuffer) {
	std::vector<GLfloat> positions;
	std::vector<GLfloat> texcoords;
	positions.reserve (quads.size () * 18);
	texcoords.reserve (quads.size () * 12);

	// two triangles per glyph, WE's index order 0 2 1 / 1 2 3 over top left, top right, bottom left, bottom right
	for (const auto& quad : quads) {
	    const auto [x0, y0, x1, y1] = std::array { quad.rect.x, quad.rect.y, quad.rect.z, quad.rect.w };
	    const auto [u0, v0, u1, v1] = std::array { quad.uv.x, quad.uv.y, quad.uv.z, quad.uv.w };

	    positions.insert (
		positions.end (), { x0, y1, 0.0f, x0, y0, 0.0f, x1, y1, 0.0f, x1, y1, 0.0f, x0, y0, 0.0f, x1, y0, 0.0f }
	    );
	    texcoords.insert (texcoords.end (), { u0, v0, u0, v1, u1, v0, u1, v0, u0, v1, u1, v1 });
	}

	glBindBuffer (GL_ARRAY_BUFFER, positionBuffer);
	glBufferData (
	    GL_ARRAY_BUFFER, static_cast<GLsizeiptr> (positions.size () * sizeof (GLfloat)), positions.data (),
	    GL_DYNAMIC_DRAW
	);
	glBindBuffer (GL_ARRAY_BUFFER, texcoordBuffer);
	glBufferData (
	    GL_ARRAY_BUFFER, static_cast<GLsizeiptr> (texcoords.size () * sizeof (GLfloat)), texcoords.data (),
	    GL_DYNAMIC_DRAW
	);

	return static_cast<GLsizei> (quads.size () * 6);
    };

    m_glyphVertexCount = upload (m_result.quads, m_glyphPositions, m_glyphTexcoords);
    m_colorGlyphVertexCount = upload (m_result.colorQuads, m_colorGlyphPositions, m_colorGlyphTexcoords);

    // the opaque background covers the text box plus padding (sub_140258050)
    const glm::vec2 padding = this->currentPadding ();
    const float bx0 = m_result.minX - padding.x;
    const float by0 = m_result.bottom - padding.y;
    const float bx1 = m_result.maxX + padding.x;
    const float by1 = m_result.top + padding.y;
    const GLfloat background[]
	= { bx0, by1, 0.0f, bx0, by0, 0.0f, bx1, by1, 0.0f, bx1, by1, 0.0f, bx0, by0, 0.0f, bx1, by0, 0.0f };

    glBindBuffer (GL_ARRAY_BUFFER, m_backgroundPositions);
    glBufferData (GL_ARRAY_BUFFER, sizeof (background), background, GL_DYNAMIC_DRAW);

    // composite quad: centered, the buffer's truncated size (sub_140258900)
    const float hx = std::max (1.0f, static_cast<float> (m_passLayout.bufferSize.x)) * 0.5f;
    const float hy = std::max (1.0f, static_cast<float> (m_passLayout.bufferSize.y)) * 0.5f;
    const GLfloat composite[]
	= { -hx, -hy, 0.0f, -hx, hy, 0.0f, hx, -hy, 0.0f, hx, -hy, 0.0f, -hx, hy, 0.0f, hx, hy, 0.0f };

    glBindBuffer (GL_ARRAY_BUFFER, m_compositePosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (composite), composite, GL_DYNAMIC_DRAW);

    // 0.15 texel in from every edge (sub_1401EA310)
    const glm::vec2 inset = 0.15000001f / glm::max (glm::vec2 (m_passLayout.bufferSize), glm::vec2 (1.0f));
    const GLfloat finalTexcoords[]
	= { inset.x,        inset.y, inset.x, 1.0f - inset.y, 1.0f - inset.x, inset.y,
	    1.0f - inset.x, inset.y, inset.x, 1.0f - inset.y, 1.0f - inset.x, 1.0f - inset.y };

    glBindBuffer (GL_ARRAY_BUFFER, m_finalTexcoords);
    glBufferData (GL_ARRAY_BUFFER, sizeof (finalTexcoords), finalTexcoords, GL_DYNAMIC_DRAW);
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

CPass* CText::createFontPass (const std::shared_ptr<const CFBO>& destination, const glm::mat4* mvp, bool color) {
    const Material& material = color ? *m_colorFontMaterial : *m_fontMaterial;
    auto* pass = new CPass (
	*this, std::make_shared<FBOProvider> (this), **material.passes.begin (), m_fontOverride, std::nullopt,
	std::nullopt
    );
    pass->setDestination (destination);
    pass->setInput (color ? m_colorAtlas : m_atlas);
    pass->setPosition (color ? m_colorGlyphPositions : m_glyphPositions);
    pass->setTexCoord (color ? m_colorGlyphTexcoords : m_glyphTexcoords);
    pass->setModelMatrix (&m_modelMatrix);
    pass->setViewProjectionMatrix (&m_viewProjectionMatrix);
    pass->setModelViewProjectionMatrix (mvp);
    pass->setModelViewProjectionMatrixInverse (mvp);

    if (color) {
	// sub_1401B3430: the MSDF colour material samples the colours from g_Texture1 (the atlas' +48 texture)
	if (m_passLayout.msdf) {
	    pass->setTexture (1, m_colorTexture);
	}

	pass->setGeometryCallback (
	    nullptr, [this] () { glDrawArrays (GL_TRIANGLES, 0, m_colorGlyphVertexCount); }, nullptr
	);
    } else {
	pass->setGeometryCallback (nullptr, [this] () { glDrawArrays (GL_TRIANGLES, 0, m_glyphVertexCount); }, nullptr);
    }

    for (int i = 0; i < 4; i++) {
	pass->addUniform ("g_RenderVar" + std::to_string (i), &m_renderVars[i]);
    }

    return pass;
}

void CText::buildPasses () {
    this->destroyPasses ();

    const auto& project = this->getScene ().getScene ().project;
    const PassLayout& layout = m_passLayout;

    // sub_1401B3430 picks the material by atlas kind and the depth flag (params +25), colour glyphs get the rgba
    // variants; the effect combos come from sub_1401B3B60
    const std::string depthSuffix = layout.depth ? "_depth" : "";
    const std::string msdfSuffix = layout.msdf ? "_msdf" : "";

    try {
	m_fontMaterial
	    = MaterialParser::load (project, "materials/fonts/basefont" + msdfSuffix + depthSuffix + ".json");
	m_colorFontMaterial = layout.color
	    ? MaterialParser::load (project, "materials/fonts/basefontrgba" + msdfSuffix + depthSuffix + ".json")
	    : nullptr;
	m_backgroundMaterial = layout.background
	    ? MaterialParser::load (project, "materials/fonts/fontbackground" + depthSuffix + ".json")
	    : nullptr;
	m_clearAlphaMaterial = layout.buffered && !layout.background
	    ? MaterialParser::load (project, "materials/util/composelayer_clearalpha.json")
	    : nullptr;
	// genericimage4 for scene version 3+ (sub_140257840)
	m_passthroughMaterial = layout.buffered && layout.passthrough
	    ? MaterialParser::load (
		  project,
		  project.sceneVersion >= 3 ? "materials/util/effectpassthrough_4.json"
					    : "materials/util/effectpassthrough.json"
	      )
	    : nullptr;
    } catch (const std::exception& e) {
	sLog.error ("CText: cannot load the font materials for ", m_text.name, ": ", e.what ());
	return;
    }

    // sub_140257840: in 3D scenes the composite's material gets depthtest "enabled" unless the text disables it
    if (m_passthroughMaterial != nullptr && layout.depth) {
	for (const auto& pass : m_passthroughMaterial->passes) {
	    pass->depthtest = DepthtestMode_Enabled;
	}
    }

    m_compositePassCount = 0;

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

	auto* glyphs = this->createFontPass (this->getScene ().getFBO (), &m_glyphSceneMatrix, false);

	if (layout.blendMode == 31) {
	    glyphs->setBlendingMode (BlendingMode_Additive);
	}

	m_passes.push_back (glyphs);

	// sub_1401B3430 draws the colour glyphs' buffer after the others
	if (layout.color) {
	    auto* colorGlyphs = this->createFontPass (this->getScene ().getFBO (), &m_glyphSceneMatrix, true);

	    if (layout.blendMode == 31) {
		colorGlyphs->setBlendingMode (BlendingMode_Additive);
	    }

	    m_passes.push_back (colorGlyphs);
	}

	return;
    }

    if (layout.bufferSize.x <= 0 || layout.bufferSize.y <= 0) {
	return;
    }

    const glm::vec2 fboSize = { static_cast<float> (layout.bufferSize.x), static_cast<float> (layout.bufferSize.y) };

    // the same buffer setup as image layers (text and image vtables share slot 23, sub_1401EA500): scene buffers named
    // _rt_imageLayerComposite_<id>_a/_b, which layers depending on the text sample, 16 bit float in HDR scene
    // rendering. They keep their identity when the text box changes size, whoever resolved them keeps a working buffer
    const TextureFormat format = this->getScene ().isHDR () ? TextureFormat_RGBA16161616f : TextureFormat_ARGB8888;
    const auto sceneBuffer = [this, format, fboSize] (const std::string& suffix) {
	auto& scene = this->getScene ();
	const std::string name = "_rt_imageLayerComposite_" + std::to_string (this->getId ()) + suffix;

	if (auto existing = scene.find (name); existing != nullptr && existing->getFormat () == format) {
	    existing->resize (static_cast<uint32_t> (fboSize.x), static_cast<uint32_t> (fboSize.y));
	    return existing;
	}

	return scene.create (name, format, TextureFlags_ClampUVs, 1.0f, fboSize, fboSize);
    };

    this->m_currentMainFBO = this->m_mainFBO = sceneBuffer ("_a");
    this->m_currentSubFBO = this->m_subFBO = sceneBuffer ("_b");

    // sub_140257C30: the buffer starts out as the opaque background, or as the scene behind the text with alpha 0
    // (composelayer_clearalpha), so the glyphs' translucent edges blend towards what they will be drawn over
    auto* base = this->createFontPass (m_currentMainFBO, &m_glyphBufferMatrix, false);

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
	clear->setTexCoord (m_compositeTexcoords);
	clear->setModelMatrix (&m_modelMatrix);
	clear->setViewProjectionMatrix (&m_viewProjectionMatrix);
	clear->setModelViewProjectionMatrix (&m_compositeMatrix);
	clear->setModelViewProjectionMatrixInverse (&m_compositeMatrixInverse);
	// under a passthrough layer WE samples its scene copy through the layer's matrices, at the text's place in
	// the layer buffer
	clear->setFollowLayerTarget (true);
	m_passes.push_back (clear);
	base->setKeepDestination (true);
    }

    m_passes.push_back (base);

    if (layout.color) {
	auto* colorGlyphs = this->createFontPass (m_currentMainFBO, &m_glyphBufferMatrix, true);
	colorGlyphs->setKeepDestination (true);
	m_passes.push_back (colorGlyphs);
    }

    std::shared_ptr<const TextureProvider> asInput = m_currentMainFBO;
    CPass* lastEffectPass = nullptr;
    bool lastWritesToTarget = false;

    for (const auto& effect : m_text.effects) {
	if (!effect->visible->value->getBool ()) {
	    continue;
	}

	const auto fboProvider = std::make_shared<FBOProvider> (this);
	EffectBuffers buffers;

	for (const auto& fbo : effect->effect->fbos) {
	    if (fbo->conditions.holds (effect->combos)) {
		buffers.emplace_back (fbo.get (), fboProvider->create (*fbo, fboSize, this->getScene ().isHDR ()));
	    }
	}

	this->registerEffectBuffers (*effect, std::move (buffers));

	// same target/previous bookkeeping as CImage::setupPasses
	bool inTargetSequence = false;
	std::shared_ptr<const TextureProvider> sequenceInput = nullptr;

	for (size_t passIndex = 0; passIndex < effect->effect->passes.size (); passIndex++) {
	    const auto& effectPass = effect->effect->passes[passIndex];

	    // command-only passes aren't supported for text, overrides go by pass index
	    if (!effectPass->material.has_value () || !effectPass->conditions.holds (effect->combos)) {
		continue;
	    }

	    const auto override = passIndex < effect->passOverrides.size ()
		? std::optional<std::reference_wrapper<const ImageEffectPassOverride>> (
		      *effect->passOverrides[passIndex]
		  )
		: std::nullopt;

	    for (auto& matPass : effectPass->material.value ()->passes) {
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
			sLog.error (
			    "Text pass target FBO '", target->get (), "' could not be resolved for ", m_text.name
			);
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
		this->registerEffectMaterial (*effect, passIndex, cpass);
		lastEffectPass = cpass;
		lastWritesToTarget = writesToTarget;

		asInput = drawTo;

		if (!writesToTarget) {
		    std::swap (this->m_currentMainFBO, this->m_currentSubFBO);
		    inTargetSequence = false;
		    sequenceInput = nullptr;
		}
	    }
	}
    }

    // without flag 0x10 the last effect pass draws onto the scene (sub_1401EBF60), depth tested only for 3D text
    if (m_passthroughMaterial == nullptr && lastEffectPass != nullptr && !lastWritesToTarget) {
	lastEffectPass->setDestination (this->getScene ().getFBO ());
	lastEffectPass->setPosition (m_compositePosition);
	lastEffectPass->setTexCoord (m_finalTexcoords);
	lastEffectPass->setModelViewProjectionMatrix (&m_compositeMatrix);
	lastEffectPass->setModelViewProjectionMatrixInverse (&m_compositeMatrixInverse);
	lastEffectPass->setBlendingMode (BlendingMode_Translucent);
	lastEffectPass->setDepthState (
	    std::make_pair (
		layout.depth ? DepthtestMode_Enabled : DepthtestMode_Disabled, lastEffectPass->getPass ().depthwrite
	    )
	);
	m_compositePassCount = 1;
	return;
    }

    // flag 0x10: effectpassthrough with BLENDMODE (sub_140257840) in white, translucent or additive for mode 31
    m_passthroughOverride.combos.clear ();

    if (m_passthroughMaterial != nullptr) {
	m_passthroughOverride.combos.emplace ("BLENDMODE", layout.blendMode == 31 ? 0 : layout.blendMode);
	m_passthroughOverride.combos.emplace ("FOG_COMPUTED", 1);
    }

    const Material& finalMaterial = m_passthroughMaterial != nullptr ? *m_passthroughMaterial : compositeMaterial ();

    for (const auto& pass : finalMaterial.passes) {
	auto* cpass = m_passthroughMaterial != nullptr
	    ? new CPass (
		  *this, std::make_shared<FBOProvider> (this), *pass, m_passthroughOverride, std::nullopt, std::nullopt
	      )
	    : new CPass (*this, std::make_shared<FBOProvider> (this), *pass, std::nullopt, std::nullopt, std::nullopt);

	if (m_passthroughMaterial != nullptr) {
	    cpass->setBlendingMode (layout.blendMode == 31 ? BlendingMode_Additive : BlendingMode_Translucent);
	    cpass->addUniform ("g_Color4", &m_white);
	    cpass->addUniform ("g_EyePosition", &this->getScene ().getFog ().eyeLocal);
	} else if (layout.blendMode == 31) {
	    cpass->setBlendingMode (BlendingMode_Additive);
	}

	cpass->setDestination (this->getScene ().getFBO ());
	cpass->setInput (asInput);
	cpass->setPosition (m_compositePosition);
	cpass->setTexCoord (m_finalTexcoords);
	// the fog of the composite measures where the text is in the scene
	cpass->setModelMatrix (m_passthroughMaterial != nullptr ? &m_compositeModel : &m_modelMatrix);
	cpass->setViewProjectionMatrix (&m_viewProjectionMatrix);
	cpass->setModelViewProjectionMatrix (&m_compositeMatrix);
	cpass->setModelViewProjectionMatrixInverse (&m_compositeMatrixInverse);
	m_passes.push_back (cpass);
	m_compositePassCount++;
    }
}

void CText::render () {
    if (!m_initialized) {
	return;
    }
    const auto& appContext = this->getScene ().getContext ().getApp ().getContext ();
    const auto visibility = appContext.resolveObjectVisibility (this->getId (), this->getObject ().name);
    const bool visible = visibility.value_or (m_text.visible->value->getBool ());

    // like images, a hidden text other layers depend on still fills its buffer, only the composite is left out
    if (!visible && (visibility.has_value () || !m_isDependency)) {
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
    } else {
	// other scripts can write layer.text too
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

    this->updateTransform ();

    glColorMask (true, true, true, true);
    glDisable (GL_DEPTH_TEST);

#if !NDEBUG
    std::string str = "Text " + this->getObject ().name + " (" + std::to_string (this->getId ()) + ")";
    glPushDebugGroup (GL_DEBUG_SOURCE_APPLICATION, 0, -1, str.c_str ());
#endif /* DEBUG */

    const size_t passCount
	= visible ? m_passes.size () : m_passes.size () - std::min (m_passes.size (), m_compositePassCount);

    // no alpha writes into the scene target (sub_1401E8AA0)
    for (size_t i = 0; i < passCount; i++) {
	glColorMask (true, true, true, m_passes[i]->getDestination () != this->getScene ().getFBO ());
	m_passes[i]->render ();
    }

    glColorMask (true, true, true, true);

#if !NDEBUG
    glPopDebugGroup ();
#endif /* DEBUG */
}

glm::vec2 CText::alignmentAnchor () const {
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

    return anchor;
}

glm::mat4 CText::worldMatrix () const {
    return glm::translate (getScene ().objectWorldMatrix (m_text), glm::vec3 (this->alignmentAnchor (), 0.0f));
}

void CText::updateTransform () {
    const glm::vec3 scale = m_text.scale->value->getVec3 ();

    // sub_140256E10: the text's world matrix is the object's full one (parents, all three angles) moved by the
    // alignment anchor in its own scaled and rotated space. The screen anchor goes on the matrix stack in front of it
    // (sub_1401E8AA0), in scene units
    const glm::vec2 screenAnchor = this->screenAnchorOffset ();
    const glm::mat4 world = glm::translate (glm::mat4 (1.0f), glm::vec3 (screenAnchor, 0.0f)) * this->worldMatrix ();
    const float boxWidth = m_result.maxX - m_result.minX;
    const float boxHeight = m_result.top - m_result.bottom;

    const auto& camera = getScene ().getCamera ();
    // layout space is y up, first baseline at 0; sub_140258050 centers the box: x - w/2 - min(minX, 0), y + h/2 - top
    const glm::vec3 center
	= { -boxWidth * 0.5f - std::min (m_result.minX, 0.0f), boxHeight * 0.5f - m_result.top, 0.0f };
    const glm::mat4 flipY = glm::scale (glm::mat4 (1.0f), glm::vec3 (1.0f, -1.0f, 1.0f));

    // 3D scenes: the world matrix goes through the scene camera like models and image layers (sub_1401E8AA0),
    // "perspective" texts through the perspective layer camera. WE's camera parallax only exists in orthographic scenes
    if (camera.isPerspective ()) {
	const glm::mat4 viewProjection = m_text.perspective->value->getBool ()
	    ? camera.getPerspectiveLayerViewProjection ()
	    : getScene ().getWorldViewProjection ();

	m_glyphSceneMatrix = viewProjection * glm::translate (world, center);
	m_glyphSceneMatrixInverse = glm::inverse (m_glyphSceneMatrix);
	// the composite quad runs y down like in 2D
	m_compositeModel = world * flipY;
	m_compositeMatrix = viewProjection * m_compositeModel;
	m_compositeMatrixInverse = glm::inverse (m_compositeMatrix);
	this->updateBufferMatrix ();
	return;
    }

    // WE's world is y up from the bottom left, this space is centered and y down; the layout below is y up too
    const float scene_w = camera.getWidth ();
    const float scene_h = camera.getHeight ();
    glm::mat4 model
	= flipY * glm::translate (glm::mat4 (1.0f), glm::vec3 (-scene_w * 0.5f, -scene_h * 0.5f, 0.0f)) * world * flipY;

    // Matches CImage's parallax handling (CImage.cpp:updateScreenSpacePosition): WE moves the view, so the offset
    // lands outside the text's own rotation and scale
    // CScene::renderFrame() already folds disableparallax into getParallaxDisplacement()
    if (this->getScene ().getScene ().camera.parallax.enabled->value->getBool ()) {
	glm::vec2 parallaxOffset = this->getScene ().getParallaxOffset (m_text);

	// mirrors CImage's parallax clamp, or a text layer drifts past its edges while a same-depth
	// CImage backing panel freezes, visibly separating the two
	if (this->getScene ().getContext ().getApp ().getContext ().settings.mouse.clampParallaxToImageSize) {
	    const float scaledHalfWidth = boxWidth * 0.5f * scale.x;
	    const float scaledHalfHeight = boxHeight * 0.5f * scale.y;
	    const float baseX = model[3].x;
	    const float baseY = model[3].y;
	    const glm::vec4 visible = getScene ().getVisibleCanvasRegion ();
	    parallaxOffset.x = clampParallaxAxis (
		parallaxOffset.x, baseX - scaledHalfWidth, baseX + scaledHalfWidth, visible.x, visible.y
	    );
	    parallaxOffset.y = clampParallaxAxis (
		parallaxOffset.y, baseY - scaledHalfHeight, baseY + scaledHalfHeight, visible.z, visible.w
	    );
	}

	model = glm::translate (glm::mat4 (1.0f), glm::vec3 (parallaxOffset, 0.0f)) * model;
    }

    // "perspective" text gets the perspective layer camera (sub_14025FAF0 -> sub_1401E5B60)
    const glm::mat4 viewProjection = m_text.perspective->value->getBool ()
	? camera.getPerspectiveLayerViewProjection ()
	: camera.getProjection () * camera.getLookAt ();

    m_glyphSceneMatrix = viewProjection * glm::translate (glm::scale (model, glm::vec3 (1.0f, -1.0f, 1.0f)), center);
    m_glyphSceneMatrixInverse = glm::inverse (m_glyphSceneMatrix);
    m_compositeMatrix = viewProjection * model;
    m_compositeModel = model;
    m_compositeMatrixInverse = glm::inverse (m_compositeMatrix);
    this->updateBufferMatrix ();
}

void CText::updateBufferMatrix () {
    if (m_passLayout.buffered) {
	// sub_140257D70: inside the buffer the box starts at the padding, its top on the buffer's first row like any
	// layer buffer (what dependent layers and orientation dependent effects sample)
	const glm::vec2 padding = this->currentPadding ();
	const glm::vec2 size = m_passLayout.bufferSize;

	m_glyphBufferMatrix = glm::translate (
	    glm::ortho<float> (0.0f, size.x, size.y, 0.0f),
	    glm::vec3 (padding.x - std::min (m_result.minX, 0.0f), padding.y - m_result.bottom, 0.0f)
	);
	m_glyphBufferMatrixInverse = glm::inverse (m_glyphBufferMatrix);
    }
}

bool CText::hitTest (const glm::vec2& ndc) {
    if (!m_result.valid) {
	return false;
    }

    this->updateTransform ();

    // sub_14019DBB0 over the text's world matrix, size +752 (sub_140258900): the layout box, plus the padding on every
    // side when the text goes through its buffer
    glm::vec2 half = { (m_result.maxX - m_result.minX) * 0.5f, (m_result.top - m_result.bottom) * 0.5f };

    if (m_passLayout.buffered) {
	half += this->currentPadding ();
    }

    return Wallpapers::CScene::quadContainsPoint (m_compositeMatrix, half, ndc);
}

glm::vec2 CText::cursorLocalPosition (const glm::vec2& ndc) {
    if (!m_result.valid) {
	return glm::vec2 (0.0f);
    }

    this->updateTransform ();

    glm::vec2 half = { (m_result.maxX - m_result.minX) * 0.5f, (m_result.top - m_result.bottom) * 0.5f };

    if (m_passLayout.buffered) {
	half += this->currentPadding ();
    }

    const auto point = Wallpapers::CScene::quadPlanePoint (m_compositeMatrix, ndc);

    // the composite quad's y runs down, like the local position
    return point.has_value () ? point.value () + half : glm::vec2 (0.0f);
}

const float& CText::getBrightness () const {
    static constexpr float kUnitBrightness = 1.0f;
    return kUnitBrightness;
}

const float& CText::getUserAlpha () const { return m_text.alpha->value->getFloat (); }

const float& CText::getAlpha () const { return m_text.alpha->value->getFloat (); }

const glm::vec3& CText::getColor () const {
    // brightness only scales the color in HDR scene rendering (renderer flag 0x2000)
    m_colorCache
	= m_text.color->value->getVec3 () * (this->getScene ().isHDR () ? m_text.brightness->value->getFloat () : 1.0f);
    return m_colorCache;
}

const glm::vec4& CText::getColor4 () const {
    m_color4Cache = glm::vec4 (this->getColor (), m_text.alpha->value->getFloat ());
    return m_color4Cache;
}

const glm::vec3& CText::getCompositeColor () const { return m_text.color->value->getVec3 (); }
