#include "Localization.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <regex>

using namespace WallpaperEngine::Data::Parsers;

namespace {
std::string lower (std::string value) {
    std::ranges::transform (value, value.begin (), [] (unsigned char c) { return std::tolower (c); });

    return value;
}

void replaceAll (std::string& text, const std::string& from, const std::string& to) {
    for (std::size_t at = text.find (from); at != std::string::npos; at = text.find (from, at + to.size ())) {
	text.replace (at, from.size (), to);
    }
}
} // namespace

Localization::Localization (const std::optional<JSON>& general, std::string locale) {
    if (general.has_value ()) {
	if (const auto localization = general->optional ("localization"); localization.has_value ()) {
	    for (const auto& language : localization->items ()) {
		if (!language.value ().is_object ()) {
		    continue;
		}

		Table table;

		for (const auto& entry : language.value ().items ()) {
		    if (entry.value ().is_string ()) {
			table.emplace (entry.key (), entry.value ().template get<std::string> ());
		    }
		}

		this->m_tables.emplace (lower (language.key ()), std::move (table));
	    }
	}
    }

    if (this->m_tables.empty ()) {
	return;
    }

    locale = lower (locale);
    const std::string language = locale.substr (0, locale.find ('-'));

    // the exact language, then any variant of it (zh-chs for zh_CN), then English, then whatever the project has
    const auto exact = this->m_tables.find (locale);
    const auto variant = std::ranges::find_if (this->m_tables, [&] (const auto& entry) {
	return entry.first.starts_with (language + "-") || entry.first == language;
    });
    const auto english = this->m_tables.find ("en-us");
    const auto anyEnglish = std::ranges::find_if (this->m_tables, [] (const auto& entry) {
	return entry.first.starts_with ("en");
    });

    if (english != this->m_tables.end ()) {
	this->m_english = &english->second;
    } else if (anyEnglish != this->m_tables.end ()) {
	this->m_english = &anyEnglish->second;
    }

    if (exact != this->m_tables.end ()) {
	this->m_preferred = &exact->second;
    } else if (variant != this->m_tables.end ()) {
	this->m_preferred = &variant->second;
    } else if (this->m_english != nullptr) {
	this->m_preferred = this->m_english;
    } else {
	this->m_preferred = &this->m_tables.begin ()->second;
    }

    if (this->m_english == nullptr) {
	this->m_english = this->m_preferred;
    }
}

std::string Localization::resolve (const std::string& text) const {
    if (text.empty ()) {
	return "";
    }

    for (const Table* table : { this->m_preferred, this->m_english }) {
	if (table == nullptr) {
	    continue;
	}

	if (const auto found = table->find (text); found != table->end ()) {
	    if (const auto cleaned = clean (found->second); !cleaned.empty ()) {
		return cleaned;
	    }
	}
    }

    std::string cleaned = clean (text);

    // a key nobody translated: "ui_music_type_0" reads better as "Music type 0" than as it is
    if (cleaned.starts_with ("ui_") && cleaned.find (' ') == std::string::npos) {
	cleaned = cleaned.substr (3);
	std::ranges::replace (cleaned, '_', ' ');

	if (!cleaned.empty ()) {
	    cleaned[0] = static_cast<char> (std::toupper (static_cast<unsigned char> (cleaned[0])));
	}
    }

    return cleaned;
}

std::string Localization::clean (const std::string& text) {
    static const std::regex smallPrint (R"(<small[^>]*>.*?</small>)", std::regex::icase);
    static const std::regex lineBreak (R"(<br\s*/?>)", std::regex::icase);
    static const std::regex tag (R"(<[^>]*>)");

    std::string result = std::regex_replace (text, smallPrint, "");
    result = std::regex_replace (result, lineBreak, " ");
    result = std::regex_replace (result, tag, " ");

    replaceAll (result, "&nbsp;", " ");
    replaceAll (result, "&lt;", "<");
    replaceAll (result, "&gt;", ">");
    replaceAll (result, "&quot;", "\"");
    replaceAll (result, "&#39;", "'");
    replaceAll (result, "&amp;", "&");

    // collapse whitespace, including the newlines some strings carry
    std::string collapsed;
    bool space = true;

    for (const char c : result) {
	if (std::isspace (static_cast<unsigned char> (c))) {
	    if (!space) {
		collapsed += ' ';
	    }

	    space = true;
	} else {
	    collapsed += c;
	    space = false;
	}
    }

    if (!collapsed.empty () && collapsed.back () == ' ') {
	collapsed.pop_back ();
    }

    return collapsed;
}

std::string Localization::systemLocale () {
    for (const char* variable : { "LC_ALL", "LC_MESSAGES", "LANG" }) {
	const char* value = std::getenv (variable);

	if (value == nullptr || *value == '\0') {
	    continue;
	}

	std::string locale = value;

	if (locale == "C" || locale == "POSIX") {
	    break;
	}

	// "de_DE.UTF-8@euro" -> "de-de"
	locale = locale.substr (0, locale.find_first_of (".@"));
	std::ranges::replace (locale, '_', '-');

	return lower (locale);
    }

    return "en-us";
}
