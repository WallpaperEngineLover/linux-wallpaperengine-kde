#pragma once

#include <string>

#include "WallpaperEngine/Data/JSON.h"

namespace WallpaperEngine::Render::Objects {
/** The JSON object after a bone's MDLS record, false for text that is not one */
class BoneJsonReader {
public:
    explicit BoneJsonReader (const std::string& text) : m_json (Data::JSON::JSON::parse (text, nullptr, false)) { }

    explicit operator bool () const { return this->m_json.is_object (); }

    [[nodiscard]] const Data::JSON::JSON& json () const { return this->m_json; }

    [[nodiscard]] bool flag (const char* key) const {
	const auto it = this->m_json.find (key);
	return it != this->m_json.end () && it->is_boolean () && it->get<bool> ();
    }

    /** Leaves out untouched when the key is missing or not a number */
    void number (const char* key, float& out) const {
	const auto it = this->m_json.find (key);

	if (it != this->m_json.end () && it->is_number ()) {
	    out = it->get<float> ();
	}
    }

private:
    Data::JSON::JSON m_json;
};
}
