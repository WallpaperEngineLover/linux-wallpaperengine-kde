#include "SceneObject.h"

#include "Adapters/ScriptableObjectAdapter.h"
#include "JS.h"
#include "ScriptEngine.h"
#include "ScriptableObject.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"

#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>

using namespace WallpaperEngine::Scripting;
using JSON = WallpaperEngine::Data::JSON::JSON;

namespace {
SceneObject& sceneOf (const v8::FunctionCallbackInfo<v8::Value>& info) {
    return ScriptEngine::from (info.GetIsolate ()).getSceneObject ();
}

// the index of the property in the order SceneObject's constructor defines them
UserSetting* scene_setting (const Scene& scene, int index) {
    switch (index) {
	case 0:
	    return scene.camera.bloom.enabled.get ();
	case 1:
	    return scene.camera.bloom.strength.get ();
	case 2:
	    return scene.camera.bloom.threshold.get ();
	case 4:
	    return scene.colors.clear.get ();
	case 5:
	    return scene.colors.ambient.get ();
	case 6:
	    return scene.colors.skylight.get ();
	case 7:
	    return scene.camera.projection.fov.get ();
	case 8:
	    return scene.camera.projection.nearz.get ();
	case 9:
	    return scene.camera.projection.farz.get ();
	case 10:
	    return scene.camera.fade.get ();
	case 11:
	    return scene.camera.shake.enabled.get ();
	case 12:
	    return scene.camera.shake.speed.get ();
	case 13:
	    return scene.camera.shake.amplitude.get ();
	case 14:
	    return scene.camera.shake.roughness.get ();
	case 15:
	    return scene.camera.parallax.enabled.get ();
	case 16:
	    return scene.camera.parallax.amount.get ();
	case 17:
	    return scene.camera.parallax.delay.get ();
	case 18:
	    return scene.camera.parallax.mouseInfluence.get ();
	default:
	    return nullptr;
    }
}

constexpr const char* SceneSettings[] = {
    "bloom",
    "bloomstrength",
    "bloomthreshold",
    "clearenabled",
    "clearcolor",
    "ambientcolor",
    "skylightcolor",
    "fov",
    "nearz",
    "farz",
    "camerafade",
    "camerashake",
    "camerashakespeed",
    "camerashakeamplitude",
    "camerashakeroughness",
    "cameraparallax",
    "cameraparallaxamount",
    "cameraparallaxdelay",
    "cameraparallaxmouseinfluence",
};

void scene_get_value (const v8::FunctionCallbackInfo<v8::Value>& info, int index) {
    auto& container = sceneOf (info);
    // clearenabled has no backing value, it reads bloom's
    const auto* setting = scene_setting (container.getScene ().getScene (), index == 3 ? 0 : index);

    switch (index) {
	case 0:
	case 3:
	case 10:
	case 11:
	case 15:
	    info.GetReturnValue ().Set (setting->value->getBool ());
	    return;
	case 4:
	case 5:
	case 6:
	    info.GetReturnValue ().Set (container.getEngine ().getAdapters ().vec3->instantiate (*setting->value));
	    return;
	default:
	    info.GetReturnValue ().Set (static_cast<double> (setting->value->getFloat ()));
	    return;
    }
}

void scene_set_value (const v8::FunctionCallbackInfo<v8::Value>& info, int index) {
    auto& container = sceneOf (info);
    auto* setting = scene_setting (container.getScene ().getScene (), index);

    // clearenabled has no backing value, the write is dropped like any other unsupported scene setting
    if (info.Length () < 1 || setting == nullptr || setting->value == nullptr) {
	return;
    }

    container.getEngine ().assignJsValue (info[0], *setting->value);
}

template <int index> void bindSceneSetting (SceneObject& scene, v8::Local<v8::Object> instance) {
    auto* isolate = scene.getEngine ().getIsolate ();
    const auto context = scene.getEngine ().getContext ();

    instance->SetAccessorProperty (
	JS::name (isolate, SceneSettings[index]), JS::function (context, JS::bind<scene_get_value, index>),
	JS::function (context, JS::bind<scene_set_value, index>, {}, 1)
    );
}

template <int... indices>
void bindSceneSettings (SceneObject& scene, v8::Local<v8::Object> instance, std::integer_sequence<int, indices...>) {
    (bindSceneSetting<indices> (scene, instance), ...);
}

// Sound objects aren't ScriptableObjects, so getLayer() hands scripts a handle with just the playback calls;
// it goes through the scene by id because sounds may not exist yet while init() runs. data is [id, startsilent]
void sound_layer_call (const v8::FunctionCallbackInfo<v8::Value>& info, int call) {
    auto& scene = sceneOf (info).getScene ();
    const auto context = info.GetIsolate ()->GetCurrentContext ();
    const int id = JS::dataAt (info, 0)->Int32Value (context).FromMaybe (0);
    const bool startsilent = JS::dataAt (info, 1)->IsTrue ();

    switch (call) {
	case 0:
	    scene.setSoundPlaying (id, true);
	    return;
	case 1:
	    scene.setSoundPlaying (id, false);
	    return;
	case 3:
	    scene.pauseSound (id);
	    return;
	default:
	    info.GetReturnValue ().Set (scene.isSoundPlaying (id, startsilent));
	    return;
    }
}

v8::Local<v8::Value> instantiate_sound_layer (ScriptEngine& engine, const Sound& sound) {
    auto* isolate = engine.getIsolate ();
    const auto context = engine.getContext ();
    const v8::Local<v8::Object> handle = v8::Object::New (isolate);
    const auto data = JS::data (
	isolate,
	{ v8::Integer::New (isolate, sound.id), v8::Boolean::New (isolate, sound.startsilent->value->getBool ()) }
    );

    JS::set (context, handle, "play", JS::function (context, JS::bind<sound_layer_call, 0>, data));
    JS::set (context, handle, "stop", JS::function (context, JS::bind<sound_layer_call, 1>, data));
    JS::set (context, handle, "pause", JS::function (context, JS::bind<sound_layer_call, 3>, data));
    JS::set (context, handle, "isPlaying", JS::function (context, JS::bind<sound_layer_call, 2>, data));

    return handle;
}

v8::Local<v8::Value> instantiate_layer (ScriptEngine& engine, WallpaperEngine::Render::CObject* object) {
    if (object == nullptr || !object->is<ScriptableObject> ()) {
	return v8::Undefined (engine.getIsolate ());
    }

    return engine.getAdapters ().object->instantiate (*object->as<ScriptableObject> ());
}

v8::Local<v8::Value> get_layer_by_id (SceneObject& container, int id) {
    return instantiate_layer (container.getEngine (), container.getScene ().getObject (id));
}

// real WE: a number is a render-order index, a string is a name and falls back to an id,
// a layer is handed back as is, anything else is undefined
void get_layer (const v8::FunctionCallbackInfo<v8::Value>& info) {
    info.GetReturnValue ().SetNull ();

    if (info.Length () < 1) {
	return;
    }

    auto& container = sceneOf (info);
    auto& engine = container.getEngine ();
    const auto layer = info[0];

    if (layer->IsNumber ()) {
	const int index = layer->Int32Value (engine.getContext ()).FromMaybe (0);
	const auto layers = container.getScene ().getLayers ();

	if (index < 0 || index >= static_cast<int> (layers.size ())
	    || container.getScene ().isPendingDestroy (*layers[index])) {
	    return;
	}

	info.GetReturnValue ().Set (instantiate_layer (engine, layers[index]));
	return;
    }

    if (layer->IsString ()) {
	const std::string name = JS::toString (info.GetIsolate (), layer);

	for (const auto& data : container.getScene ().getScene ().objects) {
	    if (data->name == name && data->is<Sound> ()) {
		info.GetReturnValue ().Set (instantiate_sound_layer (engine, *data->as<Sound> ()));
		return;
	    }
	}

	for (auto* object : container.getScene ().getObjectsByRenderOrder ()) {
	    if (object->getObject ().name == name && object->is<ScriptableObject> ()
		&& !container.getScene ().isPendingDestroy (*object)) {
		info.GetReturnValue ().Set (instantiate_layer (engine, object));
		return;
	    }
	}

	char* end = nullptr;
	const long id = std::strtol (name.c_str (), &end, 10);

	if (end != name.c_str () && *end == '\0') {
	    info.GetReturnValue ().Set (get_layer_by_id (container, static_cast<int> (id)));
	}

	return;
    }

    if (engine.getAdapters ().object->getObject (layer) != nullptr) {
	info.GetReturnValue ().Set (layer);
    }
}

void get_layer_by_id_call (const v8::FunctionCallbackInfo<v8::Value>& info) {
    if (info.Length () < 1) {
	return;
    }

    int id = 0;

    if (!info[0]->Int32Value (info.GetIsolate ()->GetCurrentContext ()).To (&id)) {
	return;
    }

    info.GetReturnValue ().Set (get_layer_by_id (sceneOf (info), id));
}

void get_layer_count (const v8::FunctionCallbackInfo<v8::Value>& info) {
    // pending ones are already off the count (sub_14018B730)
    const auto& scene = sceneOf (info).getScene ();
    info.GetReturnValue ().Set (static_cast<int32_t> (scene.getLayers ().size () - scene.getPendingDestroyCount ()));
}

void enumerate_layers (const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto& container = sceneOf (info);
    const auto context = info.GetIsolate ()->GetCurrentContext ();
    const v8::Local<v8::Array> result = v8::Array::New (info.GetIsolate ());
    uint32_t count = 0;

    for (auto* object : container.getScene ().getLayers ()) {
	if (object->is<ScriptableObject> ()) {
	    result->Set (context, count++, instantiate_layer (container.getEngine (), object)).Check ();
	}
    }

    info.GetReturnValue ().Set (result);
}

void get_layer_index (const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto& container = sceneOf (info);
    auto* layer = info.Length () == 1 ? container.getEngine ().getAdapters ().object->getObject (info[0]) : nullptr;

    info.GetReturnValue ().Set (layer == nullptr ? -1 : container.getScene ().getObjectIndex (layer));
}

// WE turns a configuration object into scene.json object form with _Internal.stringifyConfig from baseclasses.js
std::optional<JSON> stringify_layer_config (v8::Isolate* isolate, v8::Local<v8::Value> config) {
    const auto context = isolate->GetCurrentContext ();
    const v8::TryCatch tryCatch (isolate);
    const auto internal = JS::get (context, context->Global (), "_Internal");
    const auto stringify = JS::get (context, internal, "stringifyConfig");
    v8::Local<v8::Value> result;

    if (!stringify->IsFunction ()
	|| !stringify.As<v8::Function> ()->Call (context, internal, 1, &config).ToLocal (&result)
	|| !result->IsString ()) {
	return std::nullopt;
    }

    JSON parsed = JSON::parse (JS::toString (isolate, result), nullptr, false);

    if (!parsed.is_object ()) {
	return std::nullopt;
    }

    return parsed;
}

// IModelData, scenescript64 2.8.42: config parsing sub_18162FD40 / sub_181630720, createModelData sub_1816361F0,
// applyData / replaceData sub_181636B60 / sub_181636EF0, destroyModelData sub_181636890. Every error is a SyntaxError
namespace ModelDataScript {
    using namespace WallpaperEngine::Render;

    template <typename T> bool readTypedArray (v8::Local<v8::Value> value, bool matches, std::vector<T>& out) {
	if (!matches) {
	    return false;
	}

	const auto view = value.As<v8::ArrayBufferView> ();
	out.resize (view->ByteLength () / sizeof (T));

	if (!out.empty ()) {
	    view->CopyContents (out.data (), out.size () * sizeof (T));
	}

	return true;
    }

    /** false with a pending exception */
    bool parseShape (v8::Isolate* isolate, v8::Local<v8::Value> value, bool create, ModelData::ShapeConfig& shape) {
	static const std::map<std::string, uint32_t> formats = {
	    { "position", ModelData::FORMAT_POSITION },
	    { "normal", ModelData::FORMAT_NORMAL },
	    { "tangentSigned", ModelData::FORMAT_TANGENT_SIGNED },
	    { "uv", ModelData::FORMAT_UV },
	    { "color", ModelData::FORMAT_COLOR },
	};

	const auto context = isolate->GetCurrentContext ();

	if (!value->IsObject ()) {
	    shape.deleteShape = value->IsNull ();
	    return true;
	}

	const auto vertexBuffer = JS::get (context, value, "vertexBuffer");
	shape.hasVertices = readTypedArray (vertexBuffer, vertexBuffer->IsFloat32Array (), shape.vertices);

	if (!shape.hasVertices && create) {
	    JS::throwSyntaxError (isolate, "Vertex buffer missing.");
	    return false;
	}

	const auto indexBuffer = JS::get (context, value, "indexBuffer");

	if (readTypedArray (indexBuffer, indexBuffer->IsUint16Array (), shape.indices16)) {
	    shape.hasIndices16 = true;
	} else if (readTypedArray (indexBuffer, indexBuffer->IsUint32Array (), shape.indices32)) {
	    shape.hasIndices32 = true;
	} else {
	    shape.deleteIndices = indexBuffer->IsNull ();
	}

	if (const auto vertexFormat = JS::get (context, value, "vertexFormat"); vertexFormat->IsArray ()) {
	    const auto array = vertexFormat.As<v8::Array> ();

	    for (uint32_t i = 0; i < array->Length (); i++) {
		const auto entry = JS::get (context, array, i);

		if (!entry->IsString ()) {
		    break;
		}

		if (const auto format = formats.find (JS::toString (isolate, entry)); format != formats.end ()) {
		    shape.format |= format->second;
		}
	    }
	}

	if (create && shape.format == 0) {
	    JS::throwSyntaxError (isolate, "Vertex format missing.");
	    return false;
	}

	if (const auto material = JS::get (context, value, "material"); material->IsString ()) {
	    // written into a 256 byte buffer
	    shape.material = JS::toString (isolate, material).substr (0, 255);
	} else if (create) {
	    JS::throwSyntaxError (isolate, "Material missing.");
	    return false;
	}

	for (const auto& [name, target] :
	     { std::pair<const char*, bool*> { "isVertexBufferDynamic", &shape.vertexDynamic },
	       std::pair<const char*, bool*> { "isIndexBufferDynamic", &shape.indexDynamic } }) {
	    if (const auto flag = JS::get (context, value, name); flag->IsBoolean ()) {
		*target = flag->IsTrue ();
	    }
	}

	return true;
    }

    std::optional<glm::vec3> readVec3 (v8::Isolate* isolate, v8::Local<v8::Value> object, const char* name) {
	const auto context = isolate->GetCurrentContext ();
	const auto value = JS::get (context, object, name);

	if (!value->IsObject ()) {
	    return std::nullopt;
	}

	glm::vec3 vector (0.0f);

	for (int axis = 0; axis < 3; axis++) {
	    vector[axis] = static_cast<float> (
		JS::toNumber (context, JS::get (context, value, std::array { "x", "y", "z" }[axis]))
	    );
	}

	return vector;
    }

    bool parseConfig (v8::Isolate* isolate, v8::Local<v8::Value> value, bool create, ModelData::Config& config) {
	const auto context = isolate->GetCurrentContext ();
	const auto shapes = JS::get (context, value, "shapes");
	bool ok = true;

	// an object without a shapes array is a single shape
	if (shapes->IsArray ()) {
	    const auto array = shapes.As<v8::Array> ();

	    for (uint32_t i = 0; i < array->Length () && ok; i++) {
		ok = parseShape (isolate, JS::get (context, array, i), create, config.shapes.emplace_back ());
	    }
	} else {
	    ok = parseShape (isolate, value, create, config.shapes.emplace_back ());
	}

	if (!ok) {
	    return false;
	}

	const auto boundsMin = readVec3 (isolate, value, "boundingBoxMins");
	const auto boundsMax = readVec3 (isolate, value, "boundingBoxMaxs");

	if (boundsMin.has_value () && boundsMax.has_value ()) {
	    config.boundsMin = boundsMin;
	    config.boundsMax = boundsMax;
	}

	if (config.shapes.empty ()) {
	    JS::throwSyntaxError (isolate, "Shapes missing.");
	    return false;
	}

	return true;
    }

    void throwError (v8::Isolate* isolate, ModelData::Error error) {
	JS::throwSyntaxError (isolate, ModelData::errorMessage (error));
    }

    void applyOrReplace (const v8::FunctionCallbackInfo<v8::Value>& info, int replace) {
	auto* isolate = info.GetIsolate ();
	auto& container = sceneOf (info);
	const auto token = container.modelDataToken (info.This ());

	if (!token.has_value () || info.Length () < 1) {
	    return;
	}

	if (replace != 0 && container.getEngine ().isRunningUpdate ()) {
	    JS::throwSyntaxError (isolate, "IModelData.replace cannot be called in update.");
	    return;
	}

	ModelData::Config config;

	if (!parseConfig (isolate, info[0], false, config)) {
	    return;
	}

	ModelData::Error error = ModelData::Error::None;
	container.getScene ().applyModelData (*token, config, replace != 0, error);

	if (error != ModelData::Error::None) {
	    throwError (isolate, error);
	}
    }
} // namespace ModelDataScript

// scenescript64 sub_1816372D0: a string is a name (the first layer with it), falling back to strtol as an id, a
// number is an index, a layer object is itself
WallpaperEngine::Render::CObject* resolve_layer_object_any (SceneObject& container, v8::Local<v8::Value> value) {
    auto* isolate = container.getEngine ().getIsolate ();
    const auto layers = container.getScene ().getLayers ();

    if (value->IsString ()) {
	const std::string text = JS::toString (isolate, value);

	for (auto* layer : layers) {
	    if (layer->getObject ().name == text) {
		return layer;
	    }
	}

	const long id = std::strtol (text.c_str (), nullptr, 10);

	for (auto* layer : layers) {
	    if (layer->getId () == id) {
		return layer;
	    }
	}

	return nullptr;
    }

    if (value->IsNumber ()) {
	const int32_t index = value->Int32Value (container.getEngine ().getContext ()).FromMaybe (0);

	return static_cast<uint32_t> (index) < layers.size () ? layers[static_cast<uint32_t> (index)] : nullptr;
    }

    return container.getEngine ().getAdapters ().object->getObject (value);
}

// skips layers destroyed this frame (sub_1401966D0)
WallpaperEngine::Render::CObject* resolve_layer_object (SceneObject& container, v8::Local<v8::Value> value) {
    auto* object = resolve_layer_object_any (container, value);

    return object != nullptr && container.getScene ().isPendingDestroy (*object) ? nullptr : object;
}

// sub_181632FC0: removed after this frame's scripts, not from a module's top level
void destroy_layer (const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto* isolate = info.GetIsolate ();
    auto& container = sceneOf (info);

    if (container.getEngine ().isEvaluatingModuleBody ()) {
	JS::throwSyntaxError (isolate, "destroyLayer cannot be called from global scope.");
	return;
    }

    auto* object = info.Length () < 1 ? nullptr : resolve_layer_object (container, info[0]);

    info.GetReturnValue ().Set (object != nullptr && container.getScene ().queueDestroyLayer (*object));
}

// sub_181634980: the object's creation JSON without its id, parsed again. The global scope message really names
// destroyLayer in WE
void get_initial_layer_config (const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto* isolate = info.GetIsolate ();
    auto& container = sceneOf (info);

    if (container.getEngine ().isEvaluatingModuleBody ()) {
	JS::throwSyntaxError (isolate, "destroyLayer cannot be called from global scope.");
	return;
    }

    const auto* object = info.Length () < 1 ? nullptr : resolve_layer_object (container, info[0]);

    info.GetReturnValue ().SetNull ();

    if (object == nullptr) {
	return;
    }

    const v8::TryCatch tryCatch (isolate);
    v8::Local<v8::Value> parsed;

    if (v8::JSON::Parse (isolate->GetCurrentContext (), JS::string (isolate, object->getObject ().initialConfig))
	    .ToLocal (&parsed)) {
	info.GetReturnValue ().Set (parsed);
    }
}

// sub_181635540: the static scene camera as {eye, center, up, zoom}, a plain object with Vec3s
void get_camera_transforms (const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto* isolate = info.GetIsolate ();
    auto& container = sceneOf (info);

    if (container.getEngine ().isEvaluatingModuleBody ()) {
	JS::throwSyntaxError (isolate, "getCameraTransforms cannot be called from global scope.");
	return;
    }

    const auto context = isolate->GetCurrentContext ();
    const auto& camera = container.getScene ().getStaticCamera ();
    const auto& adapters = container.getEngine ().getAdapters ();
    const v8::Local<v8::Object> result = v8::Object::New (isolate);

    JS::set (context, result, "eye", adapters.vec3->create (camera.eye));
    JS::set (context, result, "center", adapters.vec3->create (camera.center));
    JS::set (context, result, "up", adapters.vec3->create (camera.up));
    JS::set (context, result, "zoom", v8::Number::New (isolate, camera.zoom));

    info.GetReturnValue ().Set (result);
}

// sub_181635EE0: an animation of any layer by animation or property name (engine slot 15, sub_14018DB00), null when
// there is none or the argument isn't a string
void get_animation (const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto* isolate = info.GetIsolate ();
    auto& container = sceneOf (info);

    if (container.getEngine ().isEvaluatingModuleBody ()) {
	JS::throwSyntaxError (isolate, "getAnimation cannot be called from global scope.");
	return;
    }

    info.GetReturnValue ().SetNull ();

    if (info.Length () == 0 || !info[0]->IsString ()) {
	return;
    }

    v8::Local<v8::Value> animation;

    if (container.getEngine ().findAnimation (JS::toString (isolate, info[0]), std::nullopt).ToLocal (&animation)) {
	info.GetReturnValue ().Set (animation);
    }
}

// sub_1816359E0: sets whichever of eye, center, up (objects) and zoom (a number) are there, true when given an object
void set_camera_transforms (const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto* isolate = info.GetIsolate ();
    auto& container = sceneOf (info);

    if (container.getEngine ().isEvaluatingModuleBody ()) {
	JS::throwSyntaxError (isolate, "setCameraTransforms cannot be called from global scope.");
	return;
    }

    if (info.Length () < 1 || !info[0]->IsObject ()) {
	info.GetReturnValue ().Set (false);
	return;
    }

    auto& camera = container.getScene ().getStaticCamera ();

    for (const auto& [name, target] : { std::pair<const char*, glm::vec3*> { "eye", &camera.eye },
					std::pair<const char*, glm::vec3*> { "center", &camera.center },
					std::pair<const char*, glm::vec3*> { "up", &camera.up } }) {
	if (const auto value = ModelDataScript::readVec3 (isolate, info[0], name); value.has_value ()) {
	    *target = *value;
	}
    }

    if (const auto zoom = JS::get (isolate->GetCurrentContext (), info[0], "zoom"); zoom->IsNumber ()) {
	camera.zoom = static_cast<float> (zoom.As<v8::Number> ()->Value ());
    }

    info.GetReturnValue ().Set (true);
}

void create_model_data (const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto* isolate = info.GetIsolate ();
    auto& container = sceneOf (info);

    if (container.getEngine ().isEvaluatingModuleBody ()) {
	JS::throwSyntaxError (isolate, "createModelData cannot be called from global scope.");
	return;
    }

    // no configuration object gives null (sub_1816361F0 returns the isolate's null root)
    info.GetReturnValue ().SetNull ();

    if (info.Length () < 1 || !info[0]->IsObject ()) {
	return;
    }

    WallpaperEngine::Render::ModelData::Config config;

    if (!ModelDataScript::parseConfig (isolate, info[0], true, config)) {
	return;
    }

    auto error = WallpaperEngine::Render::ModelData::Error::None;
    const uint32_t token = container.getScene ().createModelData (config, error);

    if (error != WallpaperEngine::Render::ModelData::Error::None) {
	ModelDataScript::throwError (isolate, error);
	return;
    }

    info.GetReturnValue ().Set (container.newModelData (token));
}

// takes the token as a number (V8 IsUint32 + Int32Value), anything else does nothing
void destroy_model_data (const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto& container = sceneOf (info);

    if (container.getEngine ().isEvaluatingModuleBody ()) {
	JS::throwSyntaxError (info.GetIsolate (), "destroyModelData cannot be called from global scope.");
	return;
    }

    if (info.Length () < 1 || !info[0]->IsUint32 ()) {
	return;
    }

    container.getScene ().destroyModelData (info[0].As<v8::Uint32> ()->Value ());
}

void create_layer (const v8::FunctionCallbackInfo<v8::Value>& info) {
    if (info.Length () != 1) {
	return;
    }

    auto* isolate = info.GetIsolate ();
    auto& container = sceneOf (info);
    auto& engine = container.getEngine ();

    if (info[0]->IsObject ()) {
	v8::Local<v8::Value> configuration = info[0];

	// an IModelData on its own is a model layer of it (scenescript64 sub_181633290 checks the handle's tag)
	if (container.modelDataToken (info[0]).has_value ()) {
	    const v8::Local<v8::Object> wrapper = v8::Object::New (isolate);
	    JS::set (engine.getContext (), wrapper, "model", info[0]);
	    configuration = wrapper;
	}

	const auto config = stringify_layer_config (isolate, configuration);

	if (!config.has_value ()) {
	    return;
	}

	auto* object = container.getScene ().createLayerFromConfig (*config);

	if (object != nullptr && object->is<ScriptableObject> ()) {
	    info.GetReturnValue ().Set (engine.getAdapters ().object->instantiate (*object->as<ScriptableObject> ()));
	}

	return;
    }

    if (!info[0]->IsString ()) {
	return;
    }

    auto* object = container.getScene ().createLayer (JS::toString (isolate, info[0]));

    if (object != nullptr && object->is<ScriptableObject> ()) {
	info.GetReturnValue ().Set (engine.getAdapters ().object->instantiate (*object->as<ScriptableObject> ()));
    }
}

void sort_layer (const v8::FunctionCallbackInfo<v8::Value>& info) {
    if (info.Length () != 2) {
	return;
    }

    auto& container = sceneOf (info);
    auto* layer = container.getEngine ().getAdapters ().object->getObject (info[0]);

    if (layer == nullptr) {
	return;
    }

    container.getScene ().sortLayer (
	layer, info[1]->Int32Value (info.GetIsolate ()->GetCurrentContext ()).FromMaybe (0)
    );
}
} // namespace

SceneObject::SceneObject (ScriptEngine& engine, Render::Wallpapers::CScene& scene) :
    m_scene (scene), m_engine (engine) {
    auto* isolate = engine.getIsolate ();
    const auto context = engine.getContext ();
    const v8::Local<v8::Object> instance = v8::Object::New (isolate);
    const auto method = [&] (const char* name, v8::FunctionCallback callback, int length) {
	JS::define (context, instance, name, JS::function (context, callback, {}, length));
    };

    bindSceneSettings (*this, instance, std::make_integer_sequence<int, std::size (SceneSettings)> ());
    method ("getLayer", get_layer, 1);
    method ("getLayerByID", get_layer_by_id_call, 1);
    method ("getLayerCount", get_layer_count, 0);
    method ("enumerateLayers", enumerate_layers, 0);
    method ("getLayerIndex", get_layer_index, 1);
    method ("createLayer", create_layer, 1);
    method ("sortLayer", sort_layer, 2);
    method ("getInitialLayerConfig", get_initial_layer_config, 1);
    method ("destroyLayer", destroy_layer, 1);
    method ("getCameraTransforms", get_camera_transforms, 0);
    method ("setCameraTransforms", set_camera_transforms, 1);
    method ("getAnimation", get_animation, 0);
    method ("createModelData", create_model_data, 1);
    method ("destroyModelData", destroy_model_data, 1);

    this->m_instance.Reset (isolate, instance);
    this->m_modelDataKey.Reset (isolate, v8::Private::New (isolate, JS::string (isolate, "IModelData")));
}

v8::Local<v8::Object> SceneObject::getInstance () const { return this->m_instance.Get (this->m_engine.getIsolate ()); }

v8::Local<v8::Object> SceneObject::newModelData (uint32_t token) {
    auto* isolate = this->m_engine.getIsolate ();
    const auto context = this->m_engine.getContext ();
    const v8::Local<v8::Object> object = v8::Object::New (isolate);

    // prototype: baseclasses.js IModelData (constants, toConfigString returning __modelDataToken)
    if (const auto prototype = JS::get (context, JS::get (context, context->Global (), "IModelData"), "prototype");
	prototype->IsObject ()) {
	JS::setPrototype (context, object, prototype);
    }

    object->SetPrivate (context, this->m_modelDataKey.Get (isolate), v8::Integer::NewFromUnsigned (isolate, token))
	.Check ();
    JS::define (
	context, object, "applyData", JS::function (context, JS::bind<ModelDataScript::applyOrReplace, 0>, {}, 1)
    );
    JS::define (
	context, object, "replaceData", JS::function (context, JS::bind<ModelDataScript::applyOrReplace, 1>, {}, 1)
    );
    object
	->DefineOwnProperty (
	    context, JS::name (isolate, "__modelDataToken"), v8::Integer::NewFromUnsigned (isolate, token),
	    static_cast<v8::PropertyAttribute> (v8::ReadOnly | v8::DontEnum | v8::DontDelete)
	)
	.Check ();

    return object;
}

std::optional<uint32_t> SceneObject::modelDataToken (v8::Local<v8::Value> value) const {
    if (!value->IsObject ()) {
	return std::nullopt;
    }

    auto* isolate = this->m_engine.getIsolate ();
    v8::Local<v8::Value> token;

    if (!value.As<v8::Object> ()
	     ->GetPrivate (this->m_engine.getContext (), this->m_modelDataKey.Get (isolate))
	     .ToLocal (&token)
	|| !token->IsUint32 ()) {
	return std::nullopt;
    }

    return token.As<v8::Uint32> ()->Value ();
}
