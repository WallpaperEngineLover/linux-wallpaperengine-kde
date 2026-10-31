#pragma once

#include <GL/glew.h>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

#include "GLSLContext.h"
#include "WallpaperEngine/Debugging/ThreadSafety.h"

namespace WallpaperEngine::Render::Shaders {
/**
 * glslang/SPIRV-Cross translations and program binaries keyed by the full source, in
 * $XDG_CACHE_HOME/linux-wallpaperengine/shaders, written via rename so engines can share it
 */
class ShaderCache {
public:
    [[nodiscard]] static ShaderCache& get ();

    void setEnabled (bool enabled);

    [[nodiscard]] std::optional<GLSLContext::Sources> loadTranslation (const std::string& key);
    void storeTranslation (const std::string& key, const GLSLContext::Sources& sources);

    [[nodiscard]] std::optional<std::string> loadText (const std::string& kind, const std::string& key);
    void storeText (const std::string& kind, const std::string& key, const std::string& text);

    [[nodiscard]] GLuint loadProgram (const std::string& key);
    /** needs GL_PROGRAM_BINARY_RETRIEVABLE_HINT */
    void storeProgram (const std::string& key, GLuint program);
    [[nodiscard]] bool programsSupported ();

private:
    ShaderCache ();

    struct Entry {
	std::filesystem::path path;
	uint64_t check;
    };

    [[nodiscard]] std::optional<Entry> entry (const std::string& kind, const std::string& key) const;
    [[nodiscard]] std::optional<std::string> read (const Entry& entry) const;
    void write (const Entry& entry, const std::string& payload) LWE_REQUIRES (m_mutex);
    void prune () LWE_REQUIRES (m_mutex);

    std::filesystem::path m_directory;
    bool m_enabled = true;
    std::optional<bool> m_programsSupported;
    std::string m_driver;
    Debugging::Mutex m_mutex;
    uint64_t m_bytesWritten LWE_GUARDED_BY (m_mutex) = 0;
    bool m_pruned LWE_GUARDED_BY (m_mutex) = false;
};
}
