#pragma once
#include "WallpaperEngine/Data/Model/Types.h"
#include "WallpaperEngine/Render/CObject.h"

#include <functional>
#include <string>
#include <vector>

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}

namespace WallpaperEngine::Scripting {
class ScriptableObject : virtual public CObject {
public:
    struct PropertyEntry {
	std::string key;
	DynamicValue& value;
    };

    ScriptableObject (Wallpapers::CScene& scene, const Object& object);
    ~ScriptableObject () override = default;

    // nullptr instead of logging and throwing, scripts probe unbacked properties (horizontalalign, size, ...)
    DynamicValue* tryGetProperty (const std::string& name);

    [[nodiscard]] const std::map<std::string, PropertyEntry>& getProperties () const;

    enum class Playback { Playing, Paused, Stopped };

    /** thisLayer.play()/pause()/stop() from scripts, kept here since init() can call it before the derived object
     * exists */
    void setPlayback (Playback playback) { this->m_playback = playback; }
    virtual void applyPlayback (Playback playback) { this->m_playback = playback; }
    [[nodiscard]] Playback getPlayback () const { return this->m_playback; }
    [[nodiscard]] virtual bool isPlaying () const { return this->m_playback == Playback::Playing; }

    /** thisObject is the IAnimationLayer (sub_1401730D0, sub_14026C980) */
    void registerAnimationLayerProperties (size_t serial, const ImageAnimationLayer& layer);
    void unregisterAnimationLayerProperties (size_t serial);
    /** drops a registered property with its animation and script, for values that are about to be freed */
    void unregisterProperty (const std::string& name);

    /** Frees the script text of every registered property, for an object that is being deleted: its modules are gone,
     * the model the values live in stays */
    void dropScriptSources ();

protected:
    void registerProperty (
	const std::string& name, DynamicValue& value, const std::string& animationGroup = {},
	const std::string& animationKey = {}
    );
    void registerRenderableProperties (
	const RenderableSettings& renderable, const UserSetting& colorBlendMode, const UserSetting& brightness
    );
    /** Effect pass constants (Radius, Area, ...) only matter to the script engine when they carry a script or animation
     */
    void registerEffectConstants (const std::vector<ImageEffectUniquePtr>& effects);
    /** The constants of one pass that carry a script or an animation, as "<prefix><name>" in the pass's own animation
     * group; returns the property names, configure runs for each */
    std::vector<std::string> registerPassConstants (
	const std::string& prefix, const ShaderConstantMap& constants,
	const std::function<void (const std::string& property)>& configure = {}
    );

private:
    Playback m_playback = Playback::Playing;
    std::map<std::string, PropertyEntry> m_properties;
    // values a later registration of the same name replaced, their script text goes with the rest in dropScriptSources
    std::vector<DynamicValue*> m_replacedValues;
};
}