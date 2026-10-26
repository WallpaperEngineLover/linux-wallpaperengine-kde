# Texture storage format

Wallpaper Engine converts textures with `resourcecompiler.exe` into `.tex` files. The reader is `Data/Parsers/TextureParser.cpp`, a
port of `wallpaper64.exe` 2.8.42 `sub_14015E580`. Every value is little endian, strings are NUL terminated.

## File
`TEXV0004` is a version 0 TEXI section directly followed by a version 0 TEXB section. `TEXV0005` is a list of tagged sections in any order,
the last four digits of a tag are the section's version; reading stops at the first unknown tag.

| Tag | Content |
|---|---|
| `TEXI` | texture header |
| `TEXB` | image container (mipmaps) |
| `TEXS` | sprite sheet frames (animated textures) |

## TEXI (`sub_14015C760`)
| Field | Size | Meaning |
|---|---|---|
| format | 4 | 0 ARGB8888, 1 RGB888, 2 RGB565, 4 DXT5, 6 DXT3, 7 DXT1, 8 RG88, 9 R8, 10 RG1616f, 11 R16f, 12 BC7, 13 RGBA1010102, 14 RGBA16161616f, 15 RGB161616f; anything else uploads as RGBA8 |
| flags | 4 | 1 no interpolation, 2 clamp UVs, 4 is animated (sprite sheet), 8 clamp to border, 32 video payload, 64 volume texture (3D, adds a depth), 0x80000 alpha in the G/R channel of RG88/R8, 0xF00000 mask component bits (a painted PBR component each) |
| texture width, height | 4 + 4 | storage size (power of two padded unless the payload is an encoded image) |
| image width, height | 4 + 4 | the picture's size |
| depth | 4 | only with the volume flag |
| extra word | 4 | only from version 1, unused |

## TEXB (`sub_14015C8D0`)
Version 1 adds the image count, version 2 a compression word per mipmap, version 3 the FreeImage format, version 4 the user property conditions.

| Field | Size | Meaning |
|---|---|---|
| image count | 4 | from version 1, else 1 |
| FreeImage format | 4 | from version 3: `FREE_IMAGE_FORMAT` of the payload, -1 for raw pixels; an encoded image (PNG, JPEG, MP4 for video textures) is decoded after the mipmap is read |
| condition count | 4 | from version 4, then per condition: group 4, key 4, flags 4, a NUL terminated JSON string (the user property that picks it) |

Then per image: a mipmap count, then per mipmap:

| Field | Size | Meaning |
|---|---|---|
| width, height | 4 + 4 | (depth 4 for volume textures) |
| compression | 4 | from version 2, bit 0 = LZ4 |
| uncompressed size | 4 | from version 2 |
| data size | 4 | bytes in the file |
| data | | pixels in the TEXI format, rows tightly packed |
| patches | | only when the container has conditions (even empty): a group count, per group a patch count and per patch 8 words (unused, key, x, y, width, height, format, size) plus the data |

## TEXS (`sub_14015E1D0`)
Frame count, from version 3 the sprite sheet's frame size (width, height), then per frame: frame number, frame time (float), x, y, width1, width2, height2, height1
(integers in version 1, floats after). Used for animated textures (`TextureFlags_IsGif`).
