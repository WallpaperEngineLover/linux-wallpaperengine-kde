#pragma once

#include <cstddef>
#include <stb_image.h>

namespace WallpaperEngine::Render {
/**
 * Decodes an encoded image (jpeg, png...) to RGBA8 the way WE's resourceutil64 does it: FreeImage with
 * JPEG_EXIFROTATE, so jpegs come out in their EXIF orientation. Free the result with stbi_image_free.
 */
stbi_uc* decodeImageRGBA (const void* data, size_t size, int& width, int& height);

// decoded size without decoding
bool decodedImageSize (const void* data, size_t size, int& width, int& height);

/** GL_MAX_TEXTURE_SIZE, checked before decoding so a bogus header can't allocate gigabytes. 0 = no limit */
void setMaxImageSide (int side);
[[nodiscard]] int maxImageSide ();
[[nodiscard]] bool exceedsMaxImageSide (int width, int height);
} // namespace WallpaperEngine::Render
