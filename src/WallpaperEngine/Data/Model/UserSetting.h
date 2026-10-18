#pragma once

#include <memory>
#include <optional>

#include "DynamicValue.h"
#include "Types.h"

namespace WallpaperEngine::Data::Model {
struct UserSetting {
    DynamicValueUniquePtr value;
    PropertySharedPtr property;
    std::optional<ConditionInfo> condition;
    // TODO: values from scripts without conditions
};
}