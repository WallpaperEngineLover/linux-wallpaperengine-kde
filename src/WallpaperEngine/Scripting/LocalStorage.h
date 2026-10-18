#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace WallpaperEngine::Scripting {
/**
 * Script localStorage files in wallpaper64.exe 2.8.42's format: "LSBK0001", u32 count, then per entry sha1 hex key
 * (40 bytes), u32 size, value ("LSKV0001" + JSON). Global store = sha1hex(path), screen store = sha1hex(path + screen).
 * Written when the last user lets go (sub_140186340) or at exit.
 */
class LocalStorage {
public:
    /** WE's limit on the sum of a store's values, checked on set */
    static constexpr size_t MAX_STORE_BYTES = 100000;

    class Store {
    public:
	explicit Store (std::string name);
	~Store ();

	Store (const Store&) = delete;
	Store& operator= (const Store&) = delete;

	[[nodiscard]] std::optional<std::string> get (const std::string& key) const;
	bool set (const std::string& key, std::string value);
	bool remove (const std::string& key);
	void clear ();
	void flush ();

	[[nodiscard]] const std::string& getName () const { return this->m_name; }
	[[nodiscard]] const std::vector<std::pair<std::string, std::string>>& getEntries () const {
	    return this->m_entries;
	}

    private:
	std::vector<std::pair<std::string, std::string>>::iterator find (const std::string& key);

	std::string m_name;
	std::vector<std::pair<std::string, std::string>> m_entries;
	std::map<std::string, std::optional<std::string>> m_changes;
	bool m_cleared = false;
    };

    static std::shared_ptr<Store> open (const std::string& name);
    /** For exit paths that may not destroy the scenes */
    static void flushAll ();

    static std::string sha1Hex (std::string_view data);

    static std::filesystem::path directory ();
    static void setDirectory (std::filesystem::path directory);

    static std::vector<std::pair<std::string, std::string>> readFile (const std::filesystem::path& file);
    static bool
    writeFile (const std::filesystem::path& file, const std::vector<std::pair<std::string, std::string>>& entries);
};
}
