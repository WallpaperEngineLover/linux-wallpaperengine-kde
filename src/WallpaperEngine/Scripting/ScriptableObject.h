#pragma once
#include "WallpaperEngine/Data/Model/Types.h"
#include "WallpaperEngine/Render/CObject.h"

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
    virtual ~ScriptableObject () = default;

    DynamicValue& getProperty (const std::string& name);
    /** Same lookup as getProperty(), but returns nullptr instead of logging+throwing when the
     *  property isn't registered - scripts routinely probe properties (horizontalalign, size, ...)
     *  that aren't backed by a DynamicValue, and that's an expected outcome, not an error. */
    DynamicValue* tryGetProperty (const std::string& name);

    const std::map<std::string, PropertyEntry>& getProperties () const;

    enum class Playback { Playing, Paused, Stopped };

    /** thisLayer.play()/pause()/stop() from scripts, kept here since init() can call it before the derived object exists */
    void setPlayback (Playback playback) { this->m_playback = playback; }
    [[nodiscard]] Playback getPlayback () const { return this->m_playback; }
    [[nodiscard]] virtual bool isPlaying () const { return this->m_playback == Playback::Playing; }

protected:
    void registerProperty (
	const std::string& name, DynamicValue& value, const std::string& animationGroup = {},
	const std::string& animationKey = {}
    );
    /** Effect pass constants (Radius, Area, ...) only matter to the script engine when they carry a script or animation */
    void registerEffectConstants (const std::vector<ImageEffectUniquePtr>& effects);

private:
    Playback m_playback = Playback::Playing;
    std::map<std::string, PropertyEntry> m_properties;
};
}