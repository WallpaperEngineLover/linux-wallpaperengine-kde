#pragma once
#include <glm/vec4.hpp>

namespace WallpaperEngine::Data::Model {
// RGBA in 0-1
struct Color : private glm::vec4 {
    using glm::vec4::vec4;
    using glm::vec4::operator=;
    using glm::vec4::operator+=;
    using glm::vec4::operator-=;
    using glm::vec4::operator*=;
    using glm::vec4::operator/=;
    explicit Color (const glm::vec4& v) : glm::vec4 (v) { }
    using glm::vec4::a;
    using glm::vec4::b;
    using glm::vec4::g;
    using glm::vec4::r;
};
}