#pragma once

#include <sstream>
#include <string>

#include "WallpaperEngine/Data/Model/Types.h"

namespace WallpaperEngine::Data::Dumpers {
using namespace WallpaperEngine::Data::Model;

class StringPrinter {
public:
    explicit StringPrinter (std::string indentationCharacter = "\t");
    ~StringPrinter () = default;

    std::string str () const;

    void printWallpaper (const Wallpaper& wallpaper);
    void printObject (const Object& object);
    void printImage (const Image& image);
    void printSound (const Sound& sound);
    void printModel (const ModelStruct& model);
    void printImageEffect (const ImageEffect& imageEffect);
    void printImageEffectPassOverride (const ImageEffectPassOverride& imageEffectPass);
    void printFBO (const FBO& fbo);
    void printMaterial (const Material& material);
    void printMaterialPass (const MaterialPass& materialPass);
    void printEffect (const Effect& effect);
    void printEffectPass (const EffectPass& effectPass);

private:
    void indentation ();
    void lineEnd ();

    /** Also prints a new line up to the new level, not just level bookkeeping */
    void increaseIndentation ();
    /** Also prints a new line up to the new level, not just level bookkeeping */
    void decreaseIndentation ();

    int m_level = 0;
    std::stringbuf m_buffer = {};
    std::ostream m_out;
    std::string m_indentationCharacter;
};
} // namespace WallpaperEngine::Data::Dumpers
