#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Types.h"

namespace WallpaperEngine::Data::Assets {
enum ContainerVersion {
    ContainerVersion_UNKNOWN = 0,
    ContainerVersion_TEXB0001 = 1,
    ContainerVersion_TEXB0002 = 2,
    ContainerVersion_TEXB0003 = 3,
    ContainerVersion_TEXB0004 = 4,
};

enum AnimatedVersion {
    AnimatedVersion_UNKNOWN = 0,
    AnimatedVersion_TEXS0001 = 1,
    AnimatedVersion_TEXS0002 = 2,
    AnimatedVersion_TEXS0003 = 3,
};

enum FIF {
    FIF_UNKNOWN = -1,
    FIF_BMP = 0,
    FIF_ICO = 1,
    FIF_JPEG = 2,
    FIF_JNG = 3,
    FIF_KOALA = 4,
    FIF_LBM = 5,
    FIF_IFF = FIF_LBM,
    FIF_MNG = 6,
    FIF_PBM = 7,
    FIF_PBMRAW = 8,
    FIF_PCD = 9,
    FIF_PCX = 10,
    FIF_PGM = 11,
    FIF_PGMRAW = 12,
    FIF_PNG = 13,
    FIF_PPM = 14,
    FIF_PPMRAW = 15,
    FIF_RAS = 16,
    FIF_TARGA = 17,
    FIF_TIFF = 18,
    FIF_WBMP = 19,
    FIF_PSD = 20,
    FIF_CUT = 21,
    FIF_XBM = 22,
    FIF_XPM = 23,
    FIF_DDS = 24,
    FIF_GIF = 25,
    FIF_HDR = 26,
    FIF_FAXG3 = 27,
    FIF_SGI = 28,
    FIF_EXR = 29,
    FIF_J2K = 30,
    FIF_JP2 = 31,
    FIF_PFM = 32,
    FIF_PICT = 33,
    FIF_RAW = 34,
    FIF_WEBP = 35,
    FIF_MP4 = FIF_WEBP,
    FIF_JXR = 36
};

enum TextureFormat {
    TextureFormat_UNKNOWN = 0xFFFFFFFF,
    TextureFormat_ARGB8888 = 0,
    TextureFormat_RGB888 = 1,
    TextureFormat_RGB565 = 2,
    TextureFormat_DXT5 = 4,
    TextureFormat_DXT3 = 6,
    TextureFormat_DXT1 = 7,
    TextureFormat_RG88 = 8,
    TextureFormat_R8 = 9,
    TextureFormat_RG1616f = 10,
    TextureFormat_R16f = 11,
    TextureFormat_BC7 = 12,
    TextureFormat_RGBa1010102 = 13,
    TextureFormat_RGBA16161616f = 14,
    TextureFormat_RGB161616f = 15,
    /** Not a .tex format: a depth only render target with 32 bit float depth (WE's shadow atlas, R32_TYPELESS) */
    TextureFormat_D32f = 100,
};

enum TextureFlags {
    TextureFlags_NoFlags = 0,
    TextureFlags_NoInterpolation = 1,
    TextureFlags_ClampUVs = 2,
    TextureFlags_IsGif = 4,
    TextureFlags_ClampUVsBorder = 8,
    TextureFlags_Video = 32,
    /** Volume texture (image filter LUTs), every mipmap carries a depth after its width and height */
    TextureFlags_Volume = 64,
    TextureFlags_AlphaChannelPriority = 524288, // Indicates RG88/R8 format where alpha is in G/R channel
    /** PBR masks: bit 0x100000 << i marks the sampler's i-th "components" entry as painted (metallic, roughness,
     *  reflection, emissive for generic shaders), which turns its combo on (wallpaper64.exe 2.8.42 sub_14016C800) */
    TextureFlags_MaskComponents = 0xF00000,
    TextureFlags_All = TextureFlags_NoInterpolation | TextureFlags_ClampUVs | TextureFlags_IsGif
	| TextureFlags_ClampUVsBorder | TextureFlags_Video | TextureFlags_Volume | TextureFlags_AlphaChannelPriority
	| TextureFlags_MaskComponents,
};

/**
 * A part of a mipmap swapped in while a user property condition holds (TEXB0004, wallpaper64.exe 2.8.42
 * sub_14015C8D0), e.g. an alternative outfit painted over the base image
 */
struct MipmapPatch {
    uint32_t key = 0;
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    /** FreeImage format of the data, -1 for raw pixels like the mipmap's own */
    int format = -1;
    std::string data {};
};

struct Mipmap {
    uint32_t width = 0;
    uint32_t height = 0;
    /** Only set on volume textures */
    uint32_t depth = 1;
    uint32_t compression = 0;
    int uncompressedSize = 0;
    int compressedSize = 0;
    std::unique_ptr<char[]> compressedData = nullptr;
    std::unique_ptr<char[]> uncompressedData = nullptr;
    std::vector<MipmapPatch> patches {};
    /** RGBA pixels with the patches of the conditions that hold already applied, used instead of decoding the data */
    std::vector<unsigned char> composedPixels {};
};

/** A TEXB0004 condition: while the user property holds, patches with this key get applied */
struct TextureCondition {
    /** Only the first condition that holds within a group counts */
    uint32_t group = 0;
    uint32_t key = 0;
    /** Bit 0 blends the patch by its alpha, bit 1 replaces the whole mipmap with the patch's raw data */
    uint32_t flags = 0;
    /** {"condition": "<bool property>"} or {"condition": {"name": "<property>", "condition": "<value>"}} */
    std::string json {};
};

struct Frame {
    uint32_t frameNumber = 0;
    float frametime = 0.0f;
    float x = 0.0f;
    float y = 0.0f;
    float width1 = 0.0f;
    float width2 = 0.0f;
    float height1 = 0.0f;
    float height2 = 0.0f;
};

struct Texture {
    ContainerVersion containerVersion = ContainerVersion_UNKNOWN;
    AnimatedVersion animatedVersion = AnimatedVersion_UNKNOWN;
    /** Bitmask of TextureFlags, stored as raw uint32_t */
    uint32_t flags = TextureFlags_NoFlags;
    uint32_t width = 0;
    uint32_t height = 0;
    /** Texture size in memory (power of 2), as opposed to real width/height above */
    uint32_t textureWidth = 0;
    uint32_t textureHeight = 0;
    /** Only set on volume textures */
    uint32_t depth = 1;
    uint32_t gifWidth = 0;
    uint32_t gifHeight = 0;
    TextureFormat format = TextureFormat_UNKNOWN;
    FIF freeImageFormat = FIF_UNKNOWN;
    bool isVideoMp4 = false;
    bool isAnimatedGif = false;
    uint32_t imageCount = 0;
    std::vector<TextureCondition> conditions {};
    std::map<uint32_t, MipmapList> images {};
    std::vector<FrameSharedPtr> frames {};

    [[nodiscard]] bool isAnimated () const { return (flags & TextureFlags_IsGif) == TextureFlags_IsGif; }
};
}