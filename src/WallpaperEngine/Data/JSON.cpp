#include "JSON.h"

#include "WallpaperEngine/Data/Parsers/UserSettingParser.h"

#include <cctype>

using namespace WallpaperEngine::Data::JSON;
using namespace WallpaperEngine::Data::Model;
using namespace WallpaperEngine::Data::Parsers;

UserSettingUniquePtr JsonExtensions::user (const std::string& key, const Properties& properties) const {
    const auto value = this->require (key, "User setting without default value must be present");

    return UserSettingParser::parse (value, properties);
}

UserSettingUniquePtr JsonExtensions::color (const std::string& key, const Properties& properties) const {
    const auto value = this->require (key, "User setting without default value must be present");

    return UserSettingParser::parse (value, properties, true);
}

JSON JsonExtensions::parseAsset (const std::string& content) {
    std::string cleaned;
    cleaned.reserve (content.size ());

    const auto length = content.size ();
    size_t i = 0;

    while (i < length) {
	const char c = content[i];

	if (c == '"') {
	    const auto start = i++;

	    while (i < length && content[i] != '"') {
		i += content[i] == '\\' ? 2 : 1;
	    }

	    i = std::min (i + 1, length);
	    cleaned.append (content, start, i - start);
	    continue;
	}

	if (c == '/' && i + 1 < length && (content[i + 1] == '/' || content[i + 1] == '*')) {
	    const auto start = i;
	    const bool block = content[i + 1] == '*';

	    i += 2;

	    if (block) {
		const auto end = content.find ("*/", i);
		i = end == std::string::npos ? length : end + 2;
	    } else {
		while (i < length && content[i] != '\n') {
		    i++;
		}
	    }

	    cleaned.append (content, start, i - start);
	    continue;
	}

	if (c == ',') {
	    auto next = i + 1;

	    while (next < length && std::isspace (static_cast<unsigned char> (content[next]))) {
		next++;
	    }

	    if (next < length && (content[next] == ']' || content[next] == '}')) {
		i++;
		continue;
	    }
	}

	cleaned.push_back (c);
	i++;
    }

    return JSON::parse (cleaned, nullptr, true, true);
}
