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

A second array (`numberOfBones` more entries, previously named `BONE2ENTRY` in this doc) follows and has *not*
been decoded - it isn't needed for skinning since the per-bone inverse-bind matrix can be derived directly from
the local bind matrices above by walking the parent chain. `mdlaOffset` gives an exact byte length for the whole
MDLS section, so the second array can safely be skipped wholesale rather than parsed.

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
    DWORD  flag;           // always 0 in every sample seen
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

There is a small (a few dozen to ~900 bytes, seen to vary per clip), still-undecoded trailer between one clip's
last bone track and the next clip's name string (or EOF for the last clip). It doesn't matter for playback of a
single matched clip; a parser reading multiple clips sequentially needs to resynchronize past it (e.g. by
scanning forward for the next plausible clip header) rather than assuming a fixed size.

## MDAT (attachment points)

An optional section between MDLS and MDLA, present on puppets that have named points other objects can follow
via scene.json's `"attachment"` field (e.g. an orb or a weapon rigidly stuck to a hand bone). When absent, the
MDLS jump field goes straight to MDLA instead.

```
CHAR   header[9]     // "MDATxxxx\0"
DWORD  mdlaOffset     // absolute file offset where MDLA begins, same role as MDLS's jump field
WORD   pointCount
WORD   boneIndex0     // point[0]'s bone index - see note below, this is NOT padding
POINT  points[pointCount]
```
```
typedef struct {
    CHAR   name[];         // null-terminated, matches scene.json's "attachment" value on a child object
    FLOAT  localMatrix[16]; // row-major 4x4, same convention as BONE.bindLocalMatrix - the point's transform
                             // relative to this point's bone index
    WORD   nextBoneIndex;   // bone index for points[i+1], NOT for this point - see note below. Absent
                             // entirely on the last point (nothing follows it to index)
} POINT;
```

The bone index for `points[i]` is written one slot early: `points[0]`'s index is the WORD immediately after
`pointCount` (previously assumed to be an unused/padding field), and `points[i]`'s own trailing WORD is actually
`points[i+1]`'s index. The last point has no trailing WORD at all. Reading a WORD after every point's matrix
(including the last) overruns two bytes past the end of the MDAT section, landing on the next section's magic
bytes (`MDLA`/`MDAT` read back as a byte-swapped "implausible" bone index); the shifted reading above consumes
the section's declared byte length exactly, confirmed against `mikasaback_puppet.mdl` (2 points, "hair" and
"eye" - MDAT section byte length matches exactly under the shifted reading, both resolved bone indices fall
comfortably inside that puppet's 73-bone skeleton, and the "hair" attachment already renders correctly using its
half of this scheme).

The point's live offset from its bind pose is `(animatedBoneWorld[boneIndex] * localMatrix).translation -
(bindBoneWorld[boneIndex] * localMatrix).translation`; a child object with a matching `"attachment"` adds that
offset to its own local origin instead of (or on top of) following a plain `"parent"` relationship.

Only cross-checked against `mikasaback_puppet.mdl` and (for the general shape) `spiritblossomahribase_puppet.mdl`,
which declares 3 points where the 3rd previously failed every plausibility check under the old (unshifted)
reading - worth re-checking against the shifted reading above, since that file hasn't been re-verified since this
was found. Parsing still stops at the first implausible entry and keeps whatever points were read successfully
rather than risking garbage data.

Reverse-engineered by cross-referencing multiple real puppet `.mdl` files pulled from Workshop content
(`ahriarm_puppet.mdl`, `ahritailbottom_puppet.mdl`, `spiritblossomahribase_puppet.mdl`) - matching decoded bind
translations/keyframe values against scene.json, and validating that decoded byte offsets exactly consume every
byte up to the known MDLA/EOF boundary. No official documentation or decompilation was available for this part
of the format.