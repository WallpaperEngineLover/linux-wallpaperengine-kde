# Objects

`objects` of `scene.json` is a list drawn in array order (`parent` only nests transforms and visibility). Every object has `id`, `name`, `origin`, `angles`,
`scale`, `visible`, `parent`, `dependencies` and `ledsource` where it makes sense; any property can be a plain value, a user property binding
(`{"user": "name", "value": ...}`) and carry a `script` and animations. Parsing is in `Data/Parsers/ObjectParser.cpp`.

| Key that selects the type | Object |
|---|---|
| `image` | image layer, a quad with a material, effects and optionally a puppet (see IMAGE_OBJECTS.md, MDL_FILES.md); `shape` instead of a texture makes a shape layer |
| `particle` | particle system, `instanceoverride` rescales it |
| `text` | text layer (`font`, `pointsize`, `anchor`, ...) |
| `sound` | sound object, `sound` lists the files |
| `model` | 3D model or model data layer |
| `light` | `point`, `lpoint`, `lspot`, `ltube`, `ldirectional` |
| `camera` | scene camera object |
| none | group or locator |

`ledsource` marks a layer as the source of the RGB lighting output (`--leds`).
