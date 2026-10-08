#pragma once

#include <GL/glew.h>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

#include "GLSLContext.h"

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

    /** 0 when missing or rejected */
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
    void write (const Entry& entry, const std::string& payload);
    void prune ();

    std::filesystem::path m_directory;
    bool m_enabled = true;
    std::optional<bool> m_programsSupported;
    std::string m_driver;
    uint64_t m_bytesWritten = 0;
    bool m_pruned = false;
    std::mutex m_mutex;
};
} // namespace WallpaperEngine::Render::Shaders
