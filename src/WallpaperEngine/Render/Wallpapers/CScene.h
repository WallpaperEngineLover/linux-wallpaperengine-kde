#pragma once

#include "WallpaperEngine/Render/Camera.h"

#include "WallpaperEngine/Render/CWallpaper.h"
#include "WallpaperEngine/Scripting/ScriptEngine.h"

#include <set>

namespace WallpaperEngine::Render {
class Camera;
class CObject;
}

namespace WallpaperEngine::Render::Wallpapers {
using namespace WallpaperEngine::Data::Model;

class CScene final : public CWallpaper {
public:
    CScene (
	const Wallpaper& wallpaper, RenderContext& context, AudioContext& audioContext,
	const WallpaperState::TextureUVsScaling& scalingMode, const uint32_t& clampMode
    );

    ~CScene () override;

    /** The project's user properties (project.json "properties"), keyed by name */
    [[nodiscard]] const Data::Model::Properties& getUserProperties () const;

    [[nodiscard]] Scripting::ScriptEngine& getScriptEngine () const;
    [[nodiscard]] Camera& getCamera () const;

    [[nodiscard]] const Scene& getScene () const;

    [[nodiscard]] int getWidth () const override;
    [[nodiscard]] int getHeight () const override;
    [[nodiscard]] int getCanvasWidth () const override;
    [[nodiscard]] int getCanvasHeight () const override;

    // Used by CText/ScriptEngine; read from the same g_Time/g_TimeLast globals CParticle consumes via extern.
    [[nodiscard]] float getTime () const;
    [[nodiscard]] float getDeltaTime () const;
    [[nodiscard]] float getFps () const;

    const glm::vec2* getMousePosition () const;
    const glm::vec2* getMousePositionLast () const;
    const glm::vec2* getMousePositionNormalized () const;
    [[nodiscard]] bool isCursorLeftDown () const { return this->m_cursorLeftDown; }
    /** Position fed to shaders as g_ParallaxPosition: 0.5 +- the smoothed, influence-scaled mouse offset */
    const glm::vec2* getParallaxPosition () const;
    /**
     * Parallax translation of an object in scene units, in the y-down space CImage/CText/CParticle position
     * themselves in. Like the real engine, the whole parent chain shifts as one rigid group using the topmost
     * ancestor's origin and parallaxDepth, a child's own depth is ignored.
     */
    [[nodiscard]] glm::vec2 getParallaxOffset (const Data::Model::Object& object) const;

    [[nodiscard]] const std::vector<CObject*>& getObjectsByRenderOrder () const;
    [[nodiscard]] const CObject* getObject (int id) const;
    /** True when any group above the object (through "parent") is hidden */
    [[nodiscard]] bool isHiddenByAncestor (const CObject& object) const;
    [[nodiscard]] int getObjectIndex (const CObject* object) const;

    void setAudioPolicy (bool muted, std::optional<int> ambientVolume) override;

    /** Script play()/stop() request for a Sound object, remembered because scripts can ask before the CSound exists */
    void setSoundPlaying (int id, bool playing);
    [[nodiscard]] std::optional<bool> getSoundPlayRequest (int id) const;

    /** Creates a new image layer from a model json at runtime, appended to the render order. Backs
     *  the scripting API's thisScene.createLayer(). Returns nullptr if the model couldn't be set up. */
    Render::CObject* createLayer (const std::string& imagePath);

    /** Moves an existing layer to the given render-order slot. Backs thisScene.sortLayer(). */
    void sortLayer (CObject* object, int index);

protected:
    void renderFrame (const glm::ivec4& viewport) override;
    void renderFrameSteps (const glm::ivec4& viewport);
    void updateMouse (const glm::ivec4& viewport);
    /** Hover/press tracking that turns pointer state into cursorEnter/cursorClick/... calls on scripted layers */
    void dispatchCursorEvents ();

    friend class CWallpaper;

private:
    /**
     * Grows the render canvas (symmetrically around the layout center, so object positions stay valid) until
     * every top-level image layer with a declared size fits, see --expand-canvas
     */
    void expandCanvasToContent (
	const Scene& scene, float width, float height, float& canvasWidth, float& canvasHeight
    ) const;

    Render::CObject* createObject (const Object& object);
    void createObjectDependencies (const Object& object);
    Render::CObject* dispatchObjectType (const Object& object);
    void addObjectToRenderOrder (const Object& object);

    std::unique_ptr<Scripting::ScriptEngine> m_scriptEngine;
    std::unique_ptr<Camera> m_camera;
    ObjectUniquePtr m_bloomObjectData;
    CObject* m_bloomObject = nullptr;
    std::map<int, CObject*> m_objects = {};
    std::set<int> m_objectsInCreation = {};
    std::set<int> m_objectsInRenderOrderWalk = {};
    std::map<int, bool> m_soundPlayRequests = {};
    std::vector<CObject*> m_objectsByRenderOrder = {};
    std::vector<DynamicValue*> m_scriptedValues = {};
    // owns the synthesized model data backing createLayer()'d objects; must outlive the CObject
    // built from it (same pattern as m_bloomObjectData)
    std::vector<ObjectUniquePtr> m_dynamicObjectData = {};
    int m_nextDynamicLayerId = 2000000000;
    glm::vec2 m_mousePosition = {};
    glm::vec2 m_mousePositionLast = {};
    glm::vec2 m_mousePositionNormalized = {};
    /** Smoothed camera offset from the scene center as a fraction of the scene size, in mouse coordinates */
    glm::vec2 m_cameraParallax = {};
    glm::vec2 m_parallaxPosition = { 0.5f, 0.5f };
    /** Wallpaper position offset (WallpaperState) riding through the parallax path */
    glm::vec2 m_parallaxBias = {};
    bool m_cursorLeftDown = false;
    glm::vec2 m_cursorLastScenePosition = {};
    // object ids the pointer is over / that the current press started on
    std::set<int> m_cursorInside = {};
    std::set<int> m_cursorPressed = {};
    std::shared_ptr<const CFBO> _rt_4FrameBuffer = nullptr;
    std::shared_ptr<const CFBO> _rt_8FrameBuffer = nullptr;
    std::shared_ptr<const CFBO> _rt_Bloom = nullptr;
    std::shared_ptr<const CFBO> _rt_shadowAtlas = nullptr;
};
} // namespace WallpaperEngine::Render::Wallpaper
