#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "WallpaperEngine/Assets/AssetLocator.h"
#include "WallpaperEngine/Data/JSON.h"
#include "WallpaperEngine/Data/Model/Effect.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Data/Parsers/EffectParser.h"
#include "WallpaperEngine/Data/Parsers/ProjectParser.h"
#include "WallpaperEngine/FileSystem/Container.h"

using namespace WallpaperEngine;
using WallpaperEngine::Data::JSON::JSON;
using WallpaperEngine::Data::Parsers::ProjectParser;

namespace {
struct ProjectDir {
    std::filesystem::path root;

    ProjectDir () {
	static int counter = 0;
	root = std::filesystem::canonical (std::filesystem::temp_directory_path ())
	    / ("lwe-project-test-" + std::to_string (getpid ()) + "-" + std::to_string (counter++));
	std::filesystem::create_directories (root);
    }

    ~ProjectDir () { std::filesystem::remove_all (root); }

    void write (const std::string& name, const std::string& text) const {
	std::filesystem::create_directories ((root / name).parent_path ());
	std::ofstream (root / name) << text;
    }

    Data::Model::ProjectUniquePtr load (const std::string& projectJson) const {
	auto container = std::make_unique<FileSystem::Container> ();
	container->mount (root, "/");

	return ProjectParser::parse (
	    JSON::parse (projectJson), std::make_unique<Assets::AssetLocator> (std::move (container))
	);
    }
};
}

TEST_CASE ("A script module given as a wallpaper is refused with its own message") {
    ProjectDir dir;
    dir.write ("script.json", R"({ "script": "export function update () { return 1; }" })");

    CHECK_THROWS_WITH (
	dir.load (R"({ "title": "module", "file": "script.json", "workshopid": "1" })"),
	Catch::Matchers::ContainsSubstring ("script module")
    );
}

TEST_CASE ("A scene without a camera section keeps the old message") {
    ProjectDir dir;
    dir.write ("scene.json", R"({ "general": {}, "objects": [] })");

    CHECK_THROWS_WITH (
	dir.load (R"({ "title": "scene", "file": "scene.json" })"), Catch::Matchers::ContainsSubstring ("camera")
    );
}

TEST_CASE ("An empty orthographic scene loads with its general settings") {
    ProjectDir dir;
    dir.write ("scene.json", R"({
	"camera": { "center": "0 0 0", "eye": "0 0 1", "up": "0 1 0", "paths": [] },
	"general": {
	    "orthogonalprojection": { "width": 1920, "height": 1080 },
	    "clearcolor": "0.1 0.2 0.3", "bloom": true, "bloomstrength": 3, "cameraparallax": true,
	    "fogdistance": true, "spritesheetrefreshsync": true
	},
	"objects": [],
	"version": 2
    })");

    const auto project = dir.load (R"({ "title": "scene", "file": "scene.json", "workshopid": 4242 })");

    REQUIRE (project != nullptr);
    CHECK (project->workshopId == "4242");
    CHECK (project->type == Data::Model::Project::Type_Scene);
    CHECK (project->sceneVersion == 2);
    REQUIRE (project->wallpaper != nullptr);
}

TEST_CASE ("Perspective scenes and a negative canvas") {
    ProjectDir dir;
    dir.write ("scene.json", R"({
	"camera": { "center": "0 0 0", "eye": "0 0 1", "up": "0 1 0" },
	"general": { "orthogonalprojection": null, "fov": 50 },
	"objects": []
    })");
    CHECK (dir.load (R"({ "file": "scene.json" })") != nullptr);

    dir.write ("scene.json", R"({
	"camera": { "center": "0 0 0", "eye": "0 0 1", "up": "0 1 0" },
	"general": { "orthogonalprojection": { "width": -4, "height": 10 } },
	"objects": []
    })");
    CHECK_THROWS (dir.load (R"({ "file": "scene.json" })"));

    dir.write ("scene.json", R"({
	"camera": { "center": "0 0 0", "eye": "0 0 1", "up": "0 1 0" },
	"general": { "orthogonalprojection": { "auto": true } }
    })");
    CHECK_THROWS (dir.load (R"({ "file": "scene.json" })"));
}

TEST_CASE ("A project without a main file or type is refused") {
    ProjectDir dir;
    dir.write ("scene.json", "{}");

    CHECK_THROWS (dir.load (R"({ "title": "no file" })"));
    CHECK_THROWS (dir.load (R"({ "file": "thing.unknown" })"));
}

namespace {
const char* SCENE_HEAD = R"({
    "camera": { "center": "0 0 0", "eye": "0 0 1", "up": "0 1 0" },
    "general": { "orthogonalprojection": { "width": 1920, "height": 1080 } },
    "objects": )";

Data::Model::ProjectUniquePtr loadObjects (const ProjectDir& dir, const std::string& objects) {
    dir.write ("scene.json", std::string (SCENE_HEAD) + objects + "\n}");

    return dir.load (R"({ "file": "scene.json", "workshopid": "7" })");
}

const Data::Model::SceneData& sceneOf (const Data::Model::ProjectUniquePtr& project) {
    return *static_cast<const Data::Model::Scene*> (project->wallpaper.get ());
}
}

TEST_CASE ("Every particle initializer, operator and emitter name parses") {
    ProjectDir dir;
    const auto project = loadObjects (dir, R"([{
	"id": 1, "name": "particles", "origin": "100 100 0",
	"particle": {
	    "maxcount": 50, "starttime": 0.5,
	    "emitter": [
		{ "name": "boxrandom", "rate": 10, "distancemin": "0 0 0", "distancemax": "100 100 0" },
		{ "name": "sphererandom", "rate": 5, "distancemin": 1, "distancemax": 50 },
		{ "name": "layerimage", "rate": 5, "origin": "0 0 0", "layer": 3 }
	    ],
	    "initializer": [
		{ "name": "colorrandom", "min": "0 0 0", "max": "255 255 255" },
		{ "name": "sizerandom", "min": 1, "max": 5 },
		{ "name": "alpharandom", "min": 0.2, "max": 1 },
		{ "name": "lifetimerandom", "min": 1, "max": 3 },
		{ "name": "velocityrandom", "min": "-10 -10 0", "max": "10 10 0" },
		{ "name": "rotationrandom", "min": 0, "max": 3 },
		{ "name": "angularvelocityrandom", "min": "0 0 -1", "max": "0 0 1" },
		{ "name": "turbulentvelocityrandom", "speedmin": 5, "speedmax": 10, "scale": 1, "timescale": 2 },
		{ "name": "inheritinitialvaluefromevent", "mode": "setcolor" },
		{ "name": "inheritcontrolpointvelocity", "controlpoint": 1, "min": 0.1, "max": 0.2 },
		{ "name": "mapsequencearoundcontrolpoint", "controlpoint": 0, "count": 4, "speed": "0 0 1" },
		{ "name": "hsvcolorrandom", "min": "0 0 0", "max": "1 1 1" },
		{ "name": "colorlist", "colors": [ "1 0 0", "0 1 0" ] },
		{ "name": "positionoffsetrandom", "min": "-5 -5 0", "max": "5 5 0" },
		{ "name": "mapsequencebetweencontrolpoints", "count": 3, "controlpoint": 0, "limit": true },
		{ "name": "remapinitialvalue", "input": "random", "output": "size" },
		{ "name": "unknowninitializer" }
	    ],
	    "operator": [
		{ "name": "movement", "gravity": "0 -50 0", "drag": 0.1 },
		{ "name": "angularmovement", "force": "0 0 1", "drag": 0.1 },
		{ "name": "alphafade", "fadeintime": 0.1, "fadeouttime": 0.5 },
		{ "name": "sizechange", "startvalue": 1, "endvalue": 0, "starttime": 0, "endtime": 1 },
		{ "name": "alphachange", "startvalue": 1, "endvalue": 0 },
		{ "name": "colorchange", "startvalue": "1 1 1", "endvalue": "0 0 0" },
		{ "name": "turbulence", "scale": 1, "speedmin": 1, "speedmax": 2, "timescale": 1 },
		{ "name": "vortex", "controlpoint": 0, "speedinner": 5, "speedouter": 1, "distanceinner": 10, "distanceouter": 100 },
		{ "name": "vortex_v2", "controlpoint": 0, "flags": 1 },
		{ "name": "inheritvaluefromevent", "mode": "setcoloropacity" },
		{ "name": "controlpointattract", "controlpoint": 0, "scale": 10, "threshold": 50 },
		{ "name": "oscillatealpha", "frequencymin": 1, "frequencymax": 2, "scalemin": 0, "scalemax": 1 },
		{ "name": "oscillatesize", "frequencymin": 1, "frequencymax": 2, "scalemin": 0.5, "scalemax": 1 },
		{ "name": "oscillateposition", "frequencymin": 1, "frequencymax": 2, "scalemin": 0, "scalemax": 10 },
		{ "name": "capvelocity", "maxspeed": 100 },
		{ "name": "boids", "separationdistance": 10, "alignmentdistance": 20, "cohesiondistance": 30 },
		{ "name": "remapvalue", "input": "age", "output": "alpha" },
		{ "name": "maintaindistancetocontrolpoint", "controlpoint": 0, "distance": 30 },
		{ "name": "maintaindistancebetweencontrolpoints", "controlpoint0": 0, "controlpoint1": 1, "distance": 30 },
		{ "name": "reducemovementnearcontrolpoint", "controlpoint": 0, "distance": 40 },
		{ "name": "collisionplane", "plane": "0 1 0", "distance": 0, "restitution": 0.5 },
		{ "name": "collisionsphere", "radius": 50, "restitution": 0.5 },
		{ "name": "collisionbox", "size": "10 10 10" },
		{ "name": "collisionbounds" },
		{ "name": "collisionquad" },
		{ "name": "collisionmodel", "model": "x" },
		{ "name": "unknownoperator" }
	    ],
	    "renderer": [
		{ "name": "sprite", "subdivision": 2 },
		{ "name": "spritetrail", "length": 0.5, "maxlength": 10 },
		{ "name": "rope", "subdivision": 4 },
		{ "name": "ropetrail", "length": 1 }
	    ],
	    "controlpoint": [
		{ "id": 0, "offset": "10 0 0", "flags": 1 },
		{ "id": 1, "offset": "0 10 0", "flags": 4 }
	    ],
	    "children": [
		{ "type": "static", "name": "particles/child.json", "maxcount": 5, "probability": 1 },
		{ "type": "eventfollow", "name": "particles/child.json", "origin": "1 2 3", "scale": "2 2 2", "angles": "0 0 1" },
		{ "type": "eventspawn", "name": "particles/child.json" },
		{ "type": "eventdeath", "name": "particles/child.json", "probability": 0.5 },
		{ "type": "bogus", "name": "particles/child.json" }
	    ],
	    "animationmode": "randomframe", "sequencemultiplier": 2, "flags": 1
	},
	"instanceoverride": { "rate": 2, "lifetime": 1.5, "size": 2, "alpha": 0.5, "count": 3, "speed": 1.5, "colorn": "1 0 0" }
    }])");

    REQUIRE (project != nullptr);
    REQUIRE (sceneOf (project).objects.size () == 1);
}

TEST_CASE ("Objects of every kind parse and keep their ids") {
    ProjectDir dir;
    dir.write ("models/box.json", R"({ "material": "materials/box.json", "autosize": true })");
    dir.write ("materials/box.json", R"({ "passes": [ { "shader": "flat", "textures": [ "box" ] } ] })");
    dir.write (
	"models/util/solidlayer.json", R"({ "material": "materials/util/solidlayer.json", "solidlayer": true })"
    );
    dir.write ("materials/util/solidlayer.json", R"({ "passes": [ { "shader": "flat" } ] })");
    dir.write ("particles/child.json", R"({ "emitter": [ { "name": "boxrandom", "rate": 1 } ] })");
    dir.write ("paths/camera.json", R"({ "paths": [
	{ "name": "p", "visible": true, "options": { "length": 100, "fps": 30, "mode": "mirror", "wraploop": true },
	  "center": { "c0": [ { "frame": 0, "value": 0 }, { "frame": 50, "value": 10 } ] },
	  "zoom": [ { "frame": 0, "value": 1 } ] },
	{ "options": { "length": -1, "fps": 30 } },
	{ "name": "bad" }
    ] })");

    const auto project = loadObjects (dir, R"([
	{ "id": 1, "name": "image", "image": "models/box.json", "origin": "960 540 0", "size": "100 100",
	  "alpha": 0.5, "color": "1 0 0", "visible": true, "alignment": "topleft", "brightness": 1.5,
	  "effects": [ { "file": "effects/none.json", "id": 5, "name": "fx", "visible": true, "passes": [
	      { "id": 1, "combos": { "A": 1 }, "constantshadervalues": { "x": 1.0 }, "textures": [ null, "t" ] } ] } ],
	  "animationlayers": [ { "id": 1, "animation": 1, "blend": 1.0, "rate": 1.0, "visible": true, "name": "a" } ],
	  "config": { "passthrough": true } },
	{ "id": 2, "name": "sound", "sound": [ "a.mp3", "b.mp3" ], "playbackmode": "random", "volume": 0.5,
	  "mintime": 1, "maxtime": 2, "origin": "0 0 0" },
	{ "id": 3, "name": "text", "text": "hello", "font": "fonts/x.ttf", "pointsize": 20, "size": "200 50",
	  "horizontalalign": "center", "verticalalign": "top", "padding": 4, "maxwidth": 100, "anchor": "topleft" },
	{ "id": 4, "name": "light", "light": "point", "radius": 100, "intensity": 2, "color": "1 1 1" },
	{ "id": 5, "name": "spot", "light": "lspot", "radius": 100, "innercone": 10, "outercone": 40, "castshadow": true },
	{ "id": 6, "name": "shape", "shape": "quad", "size": "10 10", "color": "1 1 1" },
	{ "id": 7, "name": "camera", "camera": "cam", "path": "paths/camera.json", "queuemode": "random" },
	{ "id": 8, "name": "mesh", "model": "models/none.mdl", "origin": "0 0 0" },
	{ "id": 9, "name": "group", "dependencies": [ 1, 2 ], "parent": 1, "attachment": "a" },
	{ "id": 10, "name": 5, "particle": "particles/child.json" },
	{ "id": 11, "name": "noparticle", "particle": "None" },
	{ "name": "noid" },
	{ "id": "x", "name": "badid", "image": 5 }
    ])");

    REQUIRE (project != nullptr);
    CHECK (sceneOf (project).objects.size () >= 9);
}

TEST_CASE ("Effect files parse their buffers, functions, conditions and commands") {
    ProjectDir dir;
    dir.write ("materials/fx.json", R"({ "passes": [ { "shader": "flat" } ] })");
    const auto project = loadObjects (dir, "[]");

    dir.write ("effects/fx.json", R"({
	"name": "fx", "description": "d", "group": "g", "preview": "p.gif",
	"dependencies": [ "materials/util/a.json", "b" ],
	"passes": [
	    { "material": "materials/fx.json", "bind": [ { "index": 0, "name": "_rt_a" }, { "index": 1, "name": "previous" } ],
	      "conditions": [ { "QUALITY": 1 }, { "BLUR": { "value": 2, "op": "ge" } }, { "X": { "value": 3, "op": "gt" } },
			      { "Y": { "value": 4, "op": "le" } }, { "Z": { "value": 5, "op": "lt" } }, { "W": { "value": 6 } },
			      { "V": "text" }, 5 ], "compose": true },
	    { "command": "copy", "source": "_rt_a", "target": "_rt_b" },
	    { "command": "swap", "source": "_rt_a", "target": "_rt_b" }
	],
	"fbos": [
	    { "name": "_rt_a", "format": "rgba8888", "scale": 2, "unique": true, "width": 256, "height": 128, "fit": 64,
	      "uvs": "repeat", "clear": "1 0 0 1", "conditions": [ { "A": 1 } ] },
	    { "name": "_rt_b", "format": "rgba8888", "scale": 1.5, "clear": "" },
	    { "name": "_rt_c", "format": "rgba8888", "clear": "0.5 0.5", "width": 5000 },
	    { "name": "_rt_d", "format": "rgba8888", "clear": "1" },
	    { "format": "x" }, { "name": "n" }, { "name": 5, "format": "x" }
	],
	"functions": {
	    "clearAll": { "action": "clear", "fbos": [ "_rt_a", "_rt_b", "nope", 7 ] },
	    "nothing": { "action": "clear", "fbos": [ "nope" ] },
	    "other": { "action": "swap", "fbos": [ "_rt_a" ] },
	    "nofbos": { "action": "clear" },
	    "": { "action": "clear", "fbos": [ "_rt_a" ] },
	    "bad": 5
	}
    })");

    const auto parsed = Data::Parsers::EffectParser::load (*project, "effects/fx.json");

    REQUIRE (parsed != nullptr);
    CHECK (parsed->name == "fx");
    CHECK (parsed->dependencies.size () == 2);
    CHECK (parsed->passes.size () == 3);
    CHECK (parsed->passes[0]->binds.size () == 2);
    CHECK (parsed->passes[0]->compose);
    CHECK (parsed->passes[0]->conditions.tests.size () == 6);
    CHECK (parsed->passes[1]->command == Data::Model::Command_Copy);
    CHECK (parsed->passes[2]->command == Data::Model::Command_Swap);
    REQUIRE (parsed->fbos.size () == 4);
    CHECK (parsed->fbos[0]->scale == 2.0f);
    CHECK (parsed->fbos[0]->unique);
    CHECK (parsed->fbos[0]->repeat);
    CHECK (parsed->fbos[0]->clearOnCreate);
    CHECK (parsed->fbos[1]->clearOnCreate);
    CHECK_FALSE (parsed->fbos[2]->clearOnCreate);
    CHECK_FALSE (parsed->fbos[2]->width.has_value ());
    CHECK_FALSE (parsed->fbos[3]->clearOnCreate);
    REQUIRE (parsed->functions.size () == 1);
    CHECK (parsed->functions[0].name == "clearAll");
    CHECK (parsed->functions[0].fbos.size () == 2);
}

TEST_CASE ("Effect conditions compare a combo with a number") {
    ProjectDir dir;
    dir.write ("effects/c.json", R"({ "passes": [ { "conditions": [ { "A": 1 }, { "B": { "value": 3, "op": "ge" } },
	{ "C": { "value": 3, "op": "gt" } }, { "D": { "value": 3, "op": "le" } }, { "E": { "value": 3, "op": "lt" } } ] },
	{ "conditions": 5 } ] })");
    const auto project = loadObjects (dir, "[]");
    const auto effect = Data::Parsers::EffectParser::load (*project, "effects/c.json");
    const auto& conditions = effect->passes[0]->conditions;
    Data::Model::ComboMap combos;
    combos["A"] = 1;
    combos["B"] = 3;
    combos["C"] = 4;
    combos["D"] = 3;
    combos["E"] = 2;
    CHECK (conditions.holds (combos));

    combos["C"] = 3;
    CHECK_FALSE (conditions.holds (combos));
    combos["C"] = 4;
    combos["E"] = 3;
    CHECK_FALSE (conditions.holds (combos));
    CHECK (effect->passes[1]->conditions.tests.empty ());
}

TEST_CASE ("Passthrough layers nested deeper than three lose their parent, cycles and forward parents load") {
    ProjectDir dir;
    dir.write ("models/pass.json", R"({ "material": "materials/p.json", "passthrough": true })");
    dir.write ("materials/p.json", R"({ "passes": [ { "shader": "flat" } ] })");
    dir.write (
	"models/util/solidlayer.json", R"({ "material": "materials/util/solidlayer.json", "solidlayer": true })"
    );
    dir.write ("materials/util/solidlayer.json", R"({ "passes": [ { "shader": "flat" } ] })");

    const auto project = loadObjects (dir, R"([
	{ "id": 1, "name": "a", "image": "models/pass.json" },
	{ "id": 2, "name": "b", "image": "models/pass.json", "parent": 1 },
	{ "id": 3, "name": "c", "image": "models/pass.json", "parent": 2 },
	{ "id": 4, "name": "d", "image": "models/pass.json", "parent": 3 },
	{ "id": 5, "name": "e", "image": "models/pass.json", "parent": 4 },
	{ "id": 6, "name": "late child", "image": "models/pass.json", "parent": 9 },
	{ "id": 7, "name": "cycle a", "image": "models/util/solidlayer.json", "parent": 8 },
	{ "id": 8, "name": "cycle b", "image": "models/util/solidlayer.json", "parent": 7 },
	{ "id": 9, "name": "late parent", "image": "models/util/solidlayer.json" },
	{ "id": 10, "name": "self", "image": "models/util/solidlayer.json", "parent": 10 },
	{ "id": 11, "name": "orphan", "image": "models/util/solidlayer.json", "parent": 99 },
	{ "id": 12, "name": "deep late", "image": "models/pass.json", "parent": 13 },
	{ "id": 13, "name": "deep late 2", "image": "models/pass.json", "parent": 14 },
	{ "id": 14, "name": "deep late 3", "image": "models/pass.json", "parent": 15 },
	{ "id": 15, "name": "deep late 4", "image": "models/pass.json" }
    ])");

    REQUIRE (project != nullptr);
    const auto& objects = sceneOf (project).objects;
    REQUIRE (objects.size () == 15);
    CHECK (objects[2]->parent == std::optional<int> (2));
    CHECK_FALSE (objects[3]->parent.has_value ());
    CHECK (objects[4]->parent == std::optional<int> (4));
}

TEST_CASE ("Camera path files, light counts and engine properties come from the scene file") {
    ProjectDir dir;
    dir.write ("paths/a.json", R"({ "paths": [
	{ "duration": 4, "transforms": [ { "eye": "0 0 5", "center": "0 0 0", "up": "0 1 0", "zoom": 2, "timestamp": 0.5 },
					 { "eye": "1 1 1" }, { "disabled": true }, 5, { "eye": "2 2 2", "zoom": "x" } ] },
	{ "disabled": true, "transforms": [ {} ] },
	{ "duration": 1, "transforms": [ {} ] },
	{ "transforms": [] },
	{ "duration": 9, "transforms": [ {} ] }
    ] })");
    dir.write ("paths/bad.json", "not json");
    dir.write ("paths/nolist.json", R"({ "paths": 5 })");
    dir.write ("scene.json", R"({
	"camera": { "center": "0 0 0", "eye": "0 0 1", "up": "0 1 0", "paths": [ "paths/a.json", "paths/bad.json", "paths/nolist.json", "paths/none.json", 5 ] },
	"general": {
	    "orthogonalprojection": { "width": 1920, "height": 1080 },
	    "cameraparallax": { "value": true }, "cameraparallaxmouseinfluence": 0.5,
	    "lightconfig": { "point": 3, "spot": 20, "tube": 1, "directional": 2, "spotcookie": 7, "pointshadow": 1 },
	    "fogdistance": true, "camerashake": true, "hdr": true, "bloom": true
	},
	"objects": [ { "id": 1, "name": "voice", "sound": [ "a.mp3" ] }, 5, { "id": 2, "name": "model sound", "model": "m.mdl", "sound": [ "b.mp3" ] } ]
    })");

    const auto project = dir.load (R"({ "file": "scene.json" })");

    REQUIRE (project != nullptr);
    const auto& scene = sceneOf (project);
    CHECK (scene.engineProperties.volume);
    CHECK (scene.engineProperties.cameraParallax);
    CHECK (scene.lightConfig.point == 3);
    CHECK (scene.lightConfig.spot == 4);
    CHECK (scene.lightConfig.spotCookie == 3);
}

TEST_CASE ("Every property type of a project describes itself") {
    ProjectDir dir;
    dir.write ("scene.json", R"({
	"camera": { "center": "0 0 0", "eye": "0 0 1", "up": "0 1 0" },
	"general": { "orthogonalprojection": { "width": 1920, "height": 1080 } }, "objects": [] })");

    const auto project = dir.load (R"({ "file": "scene.json", "general": { "properties": {
	"schemecolor": { "type": "color", "value": "0.1 0.2 0.3", "text": "Color", "order": 1 },
	"toggle": { "type": "bool", "value": true, "text": "Toggle" },
	"amount": { "type": "slider", "value": 5, "min": 0, "max": 10, "step": 1, "text": "Amount", "fraction": false },
	"fraction": { "type": "slider", "value": 0.5, "min": 0, "max": 1, "fraction": true, "precision": 2 },
	"choice": { "type": "combo", "value": "b", "options": [ { "label": "A", "value": "a" }, { "label": "B", "value": "b" } ] },
	"label": { "type": "text", "text": "Hello" },
	"input": { "type": "textinput", "value": "typed" },
	"texture": { "type": "scenetexture", "value": "materials/x.tex" },
	"picture": { "type": "file", "value": "", "fileType": "image" },
	"folder": { "type": "directory", "value": "" },
	"shortcut": { "type": "usershortcut", "value": "" },
	"unknown": { "type": "mystery", "value": 1 },
	"notanobject": 5
    } } })");

    REQUIRE (project != nullptr);
    CHECK (project->properties.size () >= 10);

    for (const auto& [name, property] : project->properties) {
	CAPTURE (name);
	CHECK_FALSE (property->dump ().empty ());
	CHECK_NOTHROW (static_cast<void> (property->toString ()));
    }
}
