#pragma once

#include "../Builders/ColorBuilder.h"
#include "../Utils/TypeCaster.h"
#include "DynamicValue.h"
#include "WallpaperEngine/Logging/Log.h"

#include <functional>
#include <map>
#include <ranges>
#include <string>
#include <utility>

namespace WallpaperEngine::Data::Model {
using namespace WallpaperEngine::Data::Utils;
using namespace WallpaperEngine::Data::Builders;

struct PropertyData {
    std::string name;
    std::string text;
};

struct SliderData {
    float min;
    float max;
    float step;
};

struct ComboData {
    std::map<std::string, std::string> values;
};

class Property : public DynamicValue, public TypeCaster, public PropertyData {
public:
    explicit Property (PropertyData data) : DynamicValue (), TypeCaster (), PropertyData (std::move (data)) { }

    using DynamicValue::update;
    virtual void update (const std::string& value, UpdateSource source) = 0;
    [[nodiscard]] virtual std::string dump () const = 0;
};

class PropertySlider final : public Property, SliderData {
public:
    PropertySlider (PropertyData data, SliderData sliderData, const float value) :
	Property (std::move (data)), SliderData (sliderData) {
	this->Property::update (value, UpdateSource::Initialization);
    }

    using Property::update;
    void update (const std::string& value, UpdateSource source) override { this->update (std::stof (value), source); }

    [[nodiscard]] std::string dump () const override {
	std::stringstream ss;

	ss << this->name << " - slider" << std::endl
	   << "\tText: " << this->text << std::endl
	   << "\tMin: " << this->min << std::endl
	   << "\tMax: " << this->max << std::endl
	   << "\tStep: " << this->step << std::endl
	   << "\tValue: " << this->toString () << std::endl;

	return ss.str ();
    }
};

class PropertyBoolean final : public Property {
public:
    explicit PropertyBoolean (PropertyData data, const bool value) : Property (std::move (data)) {
	this->Property::update (value, UpdateSource::Initialization);
    }

    using Property::update;
    void update (const std::string& value, UpdateSource source) override {
	this->update (value == "true" || value == "1", source);
    }

    [[nodiscard]] std::string dump () const override {
	std::stringstream ss;

	ss << this->name << " - boolean" << std::endl
	   << "\tText: " << this->text << std::endl
	   << "\tValue: " << this->toString () << std::endl;

	return ss.str ();
    }
};

class PropertyColor final : public Property {
public:
    explicit PropertyColor (PropertyData data, const std::string& value) : Property (std::move (data)) {
	this->PropertyColor::update (value, UpdateSource::Initialization);
    }

    using Property::update;
    void update (const std::string& value, UpdateSource source) override {
	this->update (ColorBuilder::parse (value), source);
    }

    [[nodiscard]] std::string dump () const override {
	std::stringstream ss;

	ss << this->name << " - color" << std::endl
	   << "\tText: " << this->text << std::endl
	   << "\tValue: " << this->toString () << std::endl;

	return ss.str ();
    }
};

class PropertyCombo final : public Property, ComboData {
public:
    PropertyCombo (PropertyData data, ComboData comboData, const std::string& value) :
	Property (std::move (data)), ComboData (std::move (comboData)) {
	this->PropertyCombo::update (value, UpdateSource::Initialization);
    }

    /** Replaces every option label, used to swap the localization keys projects store for readable text */
    void relabel (const std::function<std::string (const std::string&)>& translate) {
	for (auto& label : this->values | std::views::values) {
	    label = translate (label);
	}
    }

    /**
     * Presets can carry combo values the wallpaper never listed as an option (a 30 second slideshow interval stored
     * as 0.5, say), Wallpaper Engine hands those to the page as they are so this makes room for one
     */
    void allowValue (const std::string& value) { this->values.emplace (value, value); }

    using Property::update;
    void update (const std::string& value, UpdateSource source) override {
	if (this->values.contains (value) == false) {
	    sLog.error ("Combo value not found in combo options: ", value);
	    return;
	}

	this->DynamicValue::update (value, source);
    }

    [[nodiscard]] std::string dump () const override {
	std::stringstream ss;

	ss << this->name << " - combo" << std::endl
	   << "\tText: " << this->text << std::endl
	   << "\tValue: " << this->toString () << std::endl
	   << "Values: " << std::endl;

	for (const auto& [key, value] : this->values) {
	    ss << "\t\t" << key << " = " << value << std::endl;
	}

	return ss.str ();
    }
};

class PropertyText final : public Property {
public:
    explicit PropertyText (PropertyData data) : Property (std::move (data)) { }

    using Property::update;
    void update (const std::string& value, UpdateSource source) override { this->DynamicValue::update (value, source); }

    [[nodiscard]] std::string toString () const override { return this->text; }

    [[nodiscard]] std::string dump () const override {
	std::stringstream ss;

	ss << this->name << " - text" << std::endl
	   << "\tText: " << this->text << std::endl
	   << "\tValue: " << this->toString () << std::endl;

	return ss.str ();
    }
};

class PropertySceneTexture final : public Property {
public:
    explicit PropertySceneTexture (PropertyData data, const std::string& value) : Property (std::move (data)) {
	this->PropertySceneTexture::update (value, UpdateSource::Initialization);
    }

    void update (const std::string& value, UpdateSource source) override { this->DynamicValue::update (value, source); }

    [[nodiscard]] std::string dump () const override {
	std::stringstream ss;

	ss << this->name << " - scene texture" << std::endl
	   << "\tText: " << this->text << std::endl
	   << "\tValue: " << this->m_value << std::endl;

	return ss.str ();
    }

private:
    std::string m_value;
};

class PropertyFile final : public Property {
public:
    /** fileType is what the project restricts the property to ("video", or empty for images), only relevant for directories */
    explicit PropertyFile (
	PropertyData data, const std::string& value, bool directory = false, std::string fileType = ""
    ) : Property (std::move (data)), m_directory (directory), m_fileType (std::move (fileType)) {
	this->PropertyFile::update (value, UpdateSource::Initialization);
    }

    /** Directories are handed to web wallpapers as a list of the files inside, files as the path itself */
    [[nodiscard]] bool isDirectory () const { return this->m_directory; }
    [[nodiscard]] const std::string& getFileType () const { return this->m_fileType; }

    void update (const std::string& value, UpdateSource source) override { this->DynamicValue::update (value, source); }

    [[nodiscard]] std::string dump () const override {
	std::stringstream ss;

	ss << this->name << " - file" << std::endl
	   << "\tText: " << this->text << std::endl
	   << "\tValue: " << this->m_value << std::endl;

	return ss.str ();
    }

private:
    bool m_directory;
    std::string m_fileType;
    std::string m_value;
};

/**
 * Starts out unassigned whatever the wallpaper ships, only a value the user set (--set-property or a hotswap)
 * points it anywhere, see Desktop::UserShortcut
 */
class PropertyUserShortcut final : public Property {
public:
    explicit PropertyUserShortcut (PropertyData data) : Property (std::move (data)) {
	this->DynamicValue::update (std::string (), UpdateSource::Initialization);
    }

    void update (const std::string& value, UpdateSource source) override {
	if (source == UpdateSource::User) {
	    this->DynamicValue::update (value, source);
	}
    }

    [[nodiscard]] std::string dump () const override {
	std::stringstream ss;

	ss << this->name << " - usershortcut" << std::endl
	   << "\tText: " << this->text << std::endl
	   << "\tValue: " << this->getString () << std::endl;

	return ss.str ();
    }
};

class PropertyTextInput final : public Property {
public:
    explicit PropertyTextInput (PropertyData data, const std::string& value) : Property (std::move (data)) {
	this->PropertyTextInput::update (value, UpdateSource::Initialization);
    }

    void update (const std::string& value, UpdateSource source) override { this->DynamicValue::update (value, source); }

    [[nodiscard]] std::string dump () const override {
	std::stringstream ss;

	ss << this->name << " - textinput" << std::endl
	   << "\tText: " << this->text << std::endl
	   << "\tValue: " << this->m_value << std::endl;

	return ss.str ();
    }

private:
    std::string m_value;
};
}