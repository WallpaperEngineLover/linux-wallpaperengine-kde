#pragma once

#include <cstring>
#include <glm/detail/qualifier.hpp>
#include <glm/detail/type_vec1.hpp>
#include <string>

#include "WallpaperEngine/Data/Utils/SFINAE.h"
#include "WallpaperEngine/Logging/Log.h"

namespace WallpaperEngine::Data::Builders {
using namespace WallpaperEngine::Data::Utils::SFINAE;

class VectorBuilder {
    /** Calls the proper std::strto* function based on the incoming type */
    template <typename type> static type convert (const char* str);

public:
    /**
     * Returns the vector size (1-4) encoded in a space-separated string.
     * TODO: move/rename, doesn't really belong here
     */
    static int preparseSize (const std::string& str) {
	const char* p = str.c_str ();
	const char* first = strchr (p, ' ');
	const char* second = first ? strchr (first + 1, ' ') : nullptr;
	const char* third = second ? strchr (second + 1, ' ') : nullptr;

	if (first == nullptr) {
	    return 1;
	}

	if (second == nullptr) {
	    return 2;
	}

	if (third == nullptr) {
	    return 3;
	}

	return 4;
    }

    /** Parses a space-separated string into a glm::vec using std::strto* functions */
    template <int length, typename type, glm::qualifier qualifier>
    [[nodiscard]] static glm::vec<length, type, qualifier> parse (const std::string& str) {
	static_assert (length >= 1 && length <= 4, "Invalid vector length");

	const char* p = str.c_str ();

	const char* first = strchr (p, ' ');
	const char* second = first ? strchr (first + 1, ' ') : nullptr;
	const char* third = second ? strchr (second + 1, ' ') : nullptr;

	if constexpr (length == 1) {
	    if (first != nullptr) {
		sLog.exception ("Invalid vector format: " + str + " (too many values, expected: ", length, ")");
	    }
	} else if constexpr (length == 2) {
	    if (first == nullptr) {
		sLog.exception ("Invalid vector format: " + str + " (too few values, expected: ", length, ")");
	    }

	    if (second != nullptr) {
		sLog.exception ("Invalid vector format: " + str + " (too many values, expected: ", length, ")");
	    }
	} else if constexpr (length == 3) {
	    if (first == nullptr || second == nullptr) {
		sLog.exception ("Invalid vector format: " + str + " (too few values, expected: ", length, ")");
	    }
	    if (third != nullptr) {
		sLog.exception ("Invalid vector format: " + str + " (too many values, expected: ", length, ")");
	    }
	} else if constexpr (length == 4) {
	    if (first == nullptr || second == nullptr || third == nullptr) {
		sLog.exception ("Invalid vector format: " + str + " (too few values, expected: ", length, ")");
	    }
	}

	// lengths already validated above
	if constexpr (length == 1) {
	    return { convert<type> (p) };
	} else if constexpr (length == 2) {
	    return { convert<type> (p), convert<type> (first + 1) };
	} else if constexpr (length == 3) {
	    return { convert<type> (p), convert<type> (first + 1), convert<type> (second + 1) };
	} else if constexpr (length == 4) {
	    return { convert<type> (p), convert<type> (first + 1), convert<type> (second + 1),
		     convert<type> (third + 1) };
	}
    }
    template <typename T, typename std::enable_if_t<is_glm_vec<T>::value, int> = 0>
    [[nodiscard]] static T parse (const std::string& str) {
	constexpr int length = GlmVecTraits<T>::length;
	constexpr glm::qualifier qualifier = GlmVecTraits<T>::qualifier;

	return parse<length, typename GlmVecTraits<T>::type, qualifier> (str);
    }
};

template <> inline float VectorBuilder::convert<float> (const char* str) { return std::strtof (str, nullptr); }

template <> inline int VectorBuilder::convert<int> (const char* str) { return std::stoi (str); }

template <> inline unsigned int VectorBuilder::convert<unsigned int> (const char* str) {
    return std::strtoul (str, nullptr, 10);
}

template <> inline double VectorBuilder::convert<double> (const char* str) { return std::strtod (str, nullptr); }

template <> inline uint8_t VectorBuilder::convert<uint8_t> (const char* str) { return std::strtoul (str, nullptr, 10); }

template <> inline uint16_t VectorBuilder::convert<uint16_t> (const char* str) {
    return std::strtoul (str, nullptr, 10);
}

template <> inline uint64_t VectorBuilder::convert<uint64_t> (const char* str) {
    return std::strtoull (str, nullptr, 10);
}

template <> inline int8_t VectorBuilder::convert<int8_t> (const char* str) { return std::strtol (str, nullptr, 10); }

template <> inline int16_t VectorBuilder::convert<int16_t> (const char* str) { return std::strtol (str, nullptr, 10); }

template <> inline int64_t VectorBuilder::convert<int64_t> (const char* str) { return std::strtoll (str, nullptr, 10); }

template <> inline bool VectorBuilder::convert<bool> (const char* str) { return std::strtoul (str, nullptr, 10) > 0; }

} // namespace WallpaperEngine::Data::Parsers
