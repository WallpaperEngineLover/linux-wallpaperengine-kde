# MDL Files
These seem to be simple model files containing the required information to render 3D objects.
They're also used in some 2D backgrounds (like 2879436369) for bones.

**NOTE**: This documentation page is not completed yet as there's still work left to do on the reverse engineering of this file format. For now just a structure, describing the file format is here

```
//
// 010 editor template
//
//
typedef struct {
    float x <fgcolor=cRed>;
    float y <fgcolor=cGreen>;
    float z <fgcolor=cBlue>;
} VECTOR3;

typedef struct {
    float x <fgcolor=cRed>;
    float y <fgcolor=cGreen>;
    float z <fgcolor=cBlue>;
    float w <fgcolor=cPurple>;
} VECTOR4;

typedef struct {
    float x <fgcolor=cRed>;
    float y <fgcolor=cGreen>;
} VECTOR2;

typedef struct {
    DWORD x <fgcolor=cRed>;
    DWORD y <fgcolor=cGreen>;
    DWORD z <fgcolor=cBlue>;
    DWORD w <fgcolor=cPurple>;
} BLENDINDICES;

typedef struct {
    VECTOR3 vertex <bgcolor=cRed>;
    BLENDINDICES blendindices;
    VECTOR4 blendweight;
    VECTOR2 uv <bgcolor=cBlue>;
} VERTEX;

typedef struct {
    WORD x;
    WORD y;
    WORD z;
} VERTEXINDICE;

typedef struct {
    CHAR header[];
    DWORD first;
    DWORD second;
    DWORD third;
    CHAR json[];
    DWORD fourth;
    DWORD fifth;
    DWORD vertexByteLength;
    VERTEX vertices[vertexByteLength / sizeof(VERTEX)];
    DWORD indicesByteLength;
    VERTEXINDICE indices[indicesByteLength / sizeof (VERTEXINDICE)] <bgcolor=cYellow>;
} MDLVHEADER;

typedef struct {
    BYTE tmp;
    DWORD type;
    DWORD unk1;
} BONEENTRYHEADER;

typedef struct {
    BONEENTRYHEADER header;
    DWORD entryByteLength;
    float v[entryByteLength / sizeof (float)];
    CHAR info[];
} BONEENTRY;

typedef struct {
    BONEENTRYHEADER header;
} BONE2ENTRY;

typedef struct {
    CHAR header[];
    DWORD mightBeByteLength;
    DWORD numberOfBones;
    BONEENTRY bones[numberOfBones]<optimize=false>;
    BONE2ENTRY unk[numberOfBones]<optimize=false>;
} MDLSHEADER;

typedef struct {
    CHAR header[];
} MDLAHEADER;

typedef struct {
    MDLVHEADER mdlv;
    MDLSHEADER mdls;
} MDLFILE;

LittleEndian ();
MDLFILE file;

// mdlv => vertices information
// mdls => skinning information
// mdla => animation information
```

## MDLV header and meshes (from wallpaper64.exe's loader)

The template above only fits single-mesh puppets. The real layout, as `sub_1401D9860` reads it:

```
CHAR   magic[]            // "MDLV0023", the number after MDLV is the version
DWORD  defaultFormat      // vertex format for files older than 15
DWORD  materialsPerMesh   // strings in front of every mesh, 1 in every sample seen
DWORD  meshCount
MESH   meshes[meshCount]
```
```
typedef struct {
    CHAR   material[][materialsPerMesh]; // "materials/models/foo.json"
    DWORD  flags;          // version >= 4. Bit 1: 32 bit indices (else 16 bit). Bit 2: another DWORD follows
    FLOAT  bboxMin[3];     // version >= 17
    FLOAT  bboxMax[3];
    DWORD  format;         // version >= 15, see below
    DWORD  vertexByteLength;
    BYTE   vertices[vertexByteLength];
    DWORD  indexByteLength;
    BYTE   indices[indexByteLength];  // triangle list
    // version 21+: BYTE f1, [DWORD, DWORD n, BYTE[n] if f1], BYTE f2, [DWORD n, BYTE[n] if f2] (puppets: per bone
    // index ranges, 16 bytes each). Version 23+: DWORD records, each QWORD, CHAR name[], DWORD flags, DWORD n,
    // DWORD[n], DWORD m, DWORD[m]. Static models write six zero bytes here (wallpaper64.exe 2.8.42 sub_140261880)
} MESH;
```

The vertex format is a bitmask, and the vertex is the enabled components packed in this order (table at
`0x140369CE0`/`0x140369D50`):

| bit | size | | bit | size |
| --- | --- | --- | --- | --- |
| `0x1` position | 12 | | `0x10`/`0x20` texcoord0 as vec3/vec4 | 12/16 |
| `0x10000` position4? | 16 | | `0x40`/`0x80`/`0x100` texcoord1 vec2/3/4 | 8/12/16 |
| `0x2` normal | 12 | | `0x200`/`0x400`/`0x800` texcoord2 | 8/12/16 |
| `0x4` tangent4 | 16 | | `0x1000`/`0x2000`/`0x4000` texcoord3 | 8/12/16 |
| `0x800000` blend indices | 16 | | `0x20000`/`0x40000`/`0x80000` | 8/12/16 |
| `0x1000000` blend weights | 16 | | `0x100000`/`0x200000`/`0x400000` | 8/12/16 |
| `0x8` texcoord0 vec2 | 8 | | `0x8000` | 16 |

Static 3D models are `0xf` (48 bytes), skinned ones and the "wide" puppets `0x180000f` (80 bytes), the narrow puppets
`0x1800009` (52 bytes). One puppet (3771392318) uses `0x181000e`, no `0x1` but the 16 byte `0x10000` in its place,
most likely a vec4 position. After the last mesh a version 13+ file names its next section (`MDLS...`), an empty string
when there is none.

## Vertex blend indices/weights

`VERTEX.blendindices`/`blendweight` are always the 32 bytes immediately before the trailing UV pair, regardless of
the overall vertex stride:

```
blendIndicesOffset = vertexStride - 40   // 4x DWORD, values are bone indices into the MDLS bone array
blendWeightsOffset = vertexStride - 24   // 4x float, sums to 1.0
uvOffset            = vertexStride - 8
```

The "narrow" (52-byte) stride is exactly `position(12) + blendindices(16) + blendweight(16) + uv(8)`. The "wide"
(80-byte, and similar) stride inserts a normal + tangent4 between position and the blend data:
`position(12) + normal(12) + tangent4(16) + blendindices(16) + blendweight(16) + uv(8)`. This is not a second bone
slot as previously assumed - puppet meshes only ever carry one set of up to 4 blend influences.

## MDLS bone array (first array only)

```
CHAR   header[9]        // "MDLSxxxx\0"
DWORD  mdlaOffset        // absolute file offset where MDLA begins (matches the MDLA search marker)
DWORD  boneCount
BONE   bones[boneCount]
```
```
typedef struct {
    BYTE   tmp;
    DWORD  type;
    INT32  parent;        // index into bones[], -1 for a root bone
    DWORD  matrixByteLength;  // always 64 (16 floats) in every sample seen
    FLOAT  bindLocalMatrix[16]; // row-major 4x4, row-vector convention (matches the engine's `mul(v,M)=v*M`
                                 // HLSL convention elsewhere); rotation/scale block has always been identity in
                                 // every sample seen, translation sits in row 3 (elements 12/13 = tx/ty, 14 = tz=0)
    CHAR   name[];         // null-terminated, always empty in every sample seen
} BONE;
```

From MDLS version 2 on, a u16 count of extra records (name, two DWORDs, a 4x4 matrix each) follows, then a flag byte.
When it is set, a 4x4 matrix per bone and then one per extra record follow: the rest pose. The vertices and the
inverse bind matrices stay in the space of the bone records above (wallpaper64.exe 2.8.42 `sub_1401FBAE0`), but the
pose (animation base, additive layers, physics) starts from these matrices when they are present (`sub_1401FDF90`),
so a puppet whose parts sit apart in its texture's layout gets put together by them (3227072870, Arona's and
Makima's body puppets carry them too). Constraint, group and chain records and the per bone collision capsules come
after it, see `parsePuppetBones`. `mdlaOffset` gives the end of the whole MDLS section.

## MDLA (baked animation clips)

```
CHAR   header[9]     // "MDLAxxxx\0"
DWORD  A             // close to file size; exact meaning/use unclear, not needed for playback
DWORD  clipCount
DWORD  C             // for single-clip files this equals the scene.json object's
                      // animationlayers[].animation id; ambiguous with >1 clip - match clips by
                      // name instead (scene.json's animationlayers[].name), not by this field
DWORD  D             // always 0 in every sample seen
CLIP   clips[clipCount]
```
```
typedef struct {
    CHAR   name[];        // null-terminated, matches scene.json's animationlayers[].name
    CHAR   mode[];         // null-terminated, "loop" observed; other values unconfirmed
    FLOAT  fps;
    DWORD  frameCount;
    DWORD  flag;           // 0 in every installed clip; bit 0 adds the record described below
    DWORD  boneCount;      // should match the MDLS bone count
    BONETRACK tracks[boneCount];  // same order as the MDLS bone array
} CLIP;
```
```
typedef struct {
    DWORD  zero;           // always 0 in every sample seen
    DWORD  trackByteLength; // == (frameCount + 1) * 36
    SAMPLE samples[frameCount + 1]; // fixed-rate, one every 1/fps seconds; sample[frameCount] ==
                                     // sample[0] for a "loop" clip (closes the cycle exactly)
} BONETRACK;
```
```
typedef struct {
    FLOAT  posX, posY, posZ;
    FLOAT  rotX, rotY, rotZ;  // Euler angles in radians; rotX/rotY are 0 in every 2D puppet sample seen
    FLOAT  scaleX, scaleY, scaleZ;
} SAMPLE;
```

A clip whose `flag` has bit 0 set carries one more record right before its event list (2.8.42 `sub_140261880`):
`WORD clip, DWORD startFrame, DWORD endFrame, DWORD startOffset, DWORD rootBone`. `clip` has to be an earlier clip.
Models (not puppet images) play that clip's frames from `startFrame` on unless bit 0x400 is set. Bits 0x800/0x1000/
0x2000 (move along x/y/z) and 0x8000 (turn) are root motion: the root bone is pinned to the first frame and the
model object moves instead (`sub_140225900`, `Objects/PuppetRootMotion`).

There is a small (a few dozen to ~900 bytes, seen to vary per clip), still-undecoded trailer between one clip's
last bone track and the next clip's name string (or EOF for the last clip). It doesn't matter for playback of a
single matched clip; a parser reading multiple clips sequentially needs to resynchronize past it (e.g. by
scanning forward for the next plausible clip header) rather than assuming a fixed size.

## MDAT (attachment points)

An optional section between MDLS and MDLA, present on puppets that have named points other objects can follow
via scene.json's `"attachment"` field (e.g. an orb or a weapon rigidly stuck to a hand bone). When absent, the
MDLS jump field goes straight to MDLA instead.

```
CHAR   header[9]     // "MDAT0001\0"
DWORD  nextSection    // absolute file offset of the next section
WORD   pointCount
POINT  points[pointCount]
```
```
typedef struct {
    WORD   boneIndex;       // the bone the point sits on
    CHAR   name[];          // null-terminated, matches scene.json's "attachment" value on a child object
    FLOAT  localMatrix[16]; // same layout as BONE.bindLocalMatrix: the point relative to its bone
} POINT;
```

That is how wallpaper64.exe 2.8.42 reads it (`sub_140261880`). It looks up a child's `"attachment"` by exact name
(`sub_1402248C0`) and multiplies the child's local matrix by the posed bone (model space) times `localMatrix`
before the parent's world matrix (`sub_140224970` for models, `sub_1401FD5C0` for puppet images).

## Sections after MDLV and morph targets (MDMP)

Every section after the meshes is `"TAGnnnn\0"` followed by the absolute file offset of the next one; 2.8.42 walks
them in any order until an empty tag or the end of the file: MDLS, MDAT, MDLA, MDMP (morph targets), MDLE. One
puppet stores them as MDLS, MDAT, MDLA, MDMP, MDLE.

```
CHAR   header[9]       // "MDMP0001\0"
DWORD  nextSection
// for every mesh of the MDLV section, in file order:
WORD   targetCount
// only with targets:
FLOAT  scale           // the deltas are multiplied by it (g_MorphWeights[0])
DWORD  vertexCount
TARGET targets[targetCount]
```
```
typedef struct {
    QWORD  id;
    CHAR   name[];
    DWORD  bytes; SHORT positions[bytes / 2];   // 3 per vertex, signed normalized (R16G16B16A16_SNORM texture)
    // mesh flags 0x400: the same again for normals; 0x800 and 0x1000: one more sized block each;
    // 0x2000: DWORD, DWORD, FLOAT, FLOAT
} TARGET;
```

The engine packs every target of a mesh one after another into a square texture (positions only, or positions and
normals interleaved per vertex with flag 0x400) that the vertex shader reads with `gl_VertexID`. MDLA version 4+
clips can drive the weights: after the other tracks a flag byte, then per mesh a DWORD whose bit 0 enables tracks,
a FLOAT (1.0), a WORD count and per track a WORD target index and a sized block of one float per sample.
