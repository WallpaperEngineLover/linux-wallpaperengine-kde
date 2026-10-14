#pragma once

#include <memory>
#include <optional>

#include "DynamicValue.h"
#include "Types.h"

namespace WallpaperEngine::Data::Model {
struct UserSetting {
    // a property-bound, default or static value
    DynamicValueUniquePtr value;
    /** The property this setting takes the value from (if specified) */
    PropertySharedPtr property;
    /** Condition required for this setting, this should be possible to run in JS' V8 */
    std::optional<ConditionInfo> condition;
    // TODO: values from scripts without conditions
};
} // namespace WallpaperEngine::Data::Model