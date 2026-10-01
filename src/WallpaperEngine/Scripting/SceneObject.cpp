#include "SceneObject.h"

#include "Adapters/ScriptableObjectAdapter.h"
#include "ScriptEngine.h"
#include "ScriptableObject.h"
#include "WallpaperEngine/Data/Utils/ScopeGuard.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"

#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>

using namespace WallpaperEngine::Scripting;
using JSON = WallpaperEngine::Data::JSON::JSON;

SceneObject* get_opaque (JSValueConst this_val) {
    JSClassID classId;
    return static_cast<SceneObject*> (JS_GetAnyOpaque (this_val, &classId));
}

JSValue get_bloom (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return JS_NewBool (ctx, container->getScene ().getScene ().camera.bloom.enabled->value->getBool ());
}

JSValue get_bloomstrength (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return JS_NewFloat64 (ctx, container->getScene ().getScene ().camera.bloom.strength->value->getFloat ());
}

JSValue get_bloomthreshold (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return JS_NewFloat64 (ctx, container->getScene ().getScene ().camera.bloom.threshold->value->getFloat ());
}

JSValue get_clearenabled (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return JS_NewBool (ctx, container->getScene ().getScene ().camera.bloom.enabled->value->getBool ());
}

JSValue get_clearcolor (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return container->getEngine ().getAdapters ().vec3->instantiate (
	*container->getScene ().getScene ().colors.clear->value
    );
}

JSValue get_ambientcolor (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return container->getEngine ().getAdapters ().vec3->instantiate (
	*container->getScene ().getScene ().colors.ambient->value
    );
}

JSValue get_skylightcolor (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return container->getEngine ().getAdapters ().vec3->instantiate (
	*container->getScene ().getScene ().colors.skylight->value
    );
}

JSValue get_fov (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return JS_NewFloat64 (ctx, container->getScene ().getScene ().camera.projection.fov->value->getFloat ());
}

JSValue get_nearz (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return JS_NewFloat64 (ctx, container->getScene ().getScene ().camera.projection.nearz->value->getFloat ());
}

JSValue get_farz (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return JS_NewFloat64 (ctx, container->getScene ().getScene ().camera.projection.farz->value->getFloat ());
}

JSValue get_camerafade (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return JS_NewBool (ctx, container->getScene ().getScene ().camera.fade->value->getBool ());
}

JSValue get_camerashake (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return JS_NewBool (ctx, container->getScene ().getScene ().camera.shake.enabled->value->getBool ());
}

JSValue get_camerashakespeed (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return JS_NewFloat64 (ctx, container->getScene ().getScene ().camera.shake.speed->value->getFloat ());
}

JSValue get_camerashakeamplitude (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return JS_NewFloat64 (ctx, container->getScene ().getScene ().camera.shake.amplitude->value->getFloat ());
}

JSValue get_camerashakeroughness (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return JS_NewFloat64 (ctx, container->getScene ().getScene ().camera.shake.roughness->value->getFloat ());
}

JSValue get_cameraparallax (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return JS_NewBool (ctx, container->getScene ().getScene ().camera.parallax.enabled->value->getBool ());
}

JSValue get_cameraparallaxamount (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return JS_NewFloat64 (ctx, container->getScene ().getScene ().camera.parallax.amount->value->getFloat ());
}

JSValue get_cameraparallaxdelay (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return JS_NewFloat64 (ctx, container->getScene ().getScene ().camera.parallax.delay->value->getFloat ());
}

JSValue get_cameraparallaxmouseinfluence (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    return JS_NewFloat64 (ctx, container->getScene ().getScene ().camera.parallax.mouseInfluence->value->getFloat ());
}

// Sound objects aren't ScriptableObjects, so getLayer() hands scripts a handle with just the playback calls;
// it goes through the scene by id because sounds may not exist yet while init() runs
JSValue sound_layer_call (
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValueConst* func_data
) {
    int64_t address = 0;
    int id = 0;
    int startsilent = 0;
    JS_ToInt64 (ctx, &address, func_data[0]);
    JS_ToInt32 (ctx, &id, func_data[1]);
    JS_ToInt32 (ctx, &startsilent, func_data[2]);
    auto* scene = reinterpret_cast<WallpaperEngine::Render::Wallpapers::CScene*> (static_cast<intptr_t> (address));

    switch (magic) {
	case 0:
	    scene->setSoundPlaying (id, true);
	    return JS_UNDEFINED;
	case 1:
	    scene->setSoundPlaying (id, false);
	    return JS_UNDEFINED;
	case 3:
	    scene->pauseSound (id);
	    return JS_UNDEFINED;
	default:
	    return JS_NewBool (ctx, scene->isSoundPlaying (id, startsilent != 0));
    }
}

JSValue
instantiate_sound_layer (JSContext* ctx, WallpaperEngine::Render::Wallpapers::CScene& scene, const Sound& sound) {
    JSValue handle = JS_NewObject (ctx);
    JSValue data[] = { JS_NewInt64 (ctx, static_cast<int64_t> (reinterpret_cast<intptr_t> (&scene))),
		       JS_NewInt32 (ctx, sound.id), JS_NewInt32 (ctx, sound.startsilent.value_or (false) ? 1 : 0) };
    static constexpr struct {
	const char* name;
	int magic;
    } calls[] = { { "play", 0 }, { "stop", 1 }, { "pause", 3 }, { "isPlaying", 2 } };

    for (const auto& call : calls) {
	JS_SetPropertyStr (ctx, handle, call.name, JS_NewCFunctionData (ctx, sound_layer_call, 0, call.magic, 3, data));
    }

    return handle;
}

JSValue instantiate_layer (SceneObject& container, WallpaperEngine::Render::CObject* object) {
    if (object == nullptr || !object->is<ScriptableObject> ()) {
	return JS_UNDEFINED;
    }

    return container.getEngine ().getAdapters ().object->instantiate (*object->as<ScriptableObject> ());
}

JSValue get_layer_by_id (SceneObject& container, int id) {
    return instantiate_layer (container, container.getScene ().getObject (id));
}

// real WE: a number is a render-order index, a string is a name and falls back to an id,
// a layer is handed back as is, anything else is undefined
JSValue get_layer (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 1) {
	return JS_UNDEFINED;
    }

    auto* container = get_opaque (this_val);
    JSValue layer = argv[0];

    if (JS_IsNumber (layer)) {
	int index = 0;
	JS_ToInt32 (ctx, &index, layer);

	const auto layers = container->getScene ().getLayers ();

	if (index < 0 || index >= static_cast<int> (layers.size ())) {
	    return JS_UNDEFINED;
	}

	return instantiate_layer (*container, layers[index]);
    }

    if (JS_IsString (layer)) {
	const char* result = JS_ToCString (ctx, layer);

	if (result == nullptr) {
	    return JS_UNDEFINED;
	}

	ScopeGuard guard ([=] { JS_FreeCString (ctx, result); });

	for (const auto& data : container->getScene ().getScene ().objects) {
	    if (data->name == result && data->is<Sound> ()) {
		return instantiate_sound_layer (ctx, container->getScene (), *data->as<Sound> ());
	    }
	}

	for (auto object : container->getScene ().getObjectsByRenderOrder ()) {
	    if (object->getObject ().name == result && object->is<ScriptableObject> ()) {
		return instantiate_layer (*container, object);
	    }
	}

	char* end = nullptr;
	const long id = std::strtol (result, &end, 10);

	if (end != result && *end == '\0') {
	    return get_layer_by_id (*container, static_cast<int> (id));
	}

	return JS_UNDEFINED;
    }

    if (WallpaperEngine::Scripting::Adapters::ScriptableObjectAdapter::getObject (layer) != nullptr) {
	return JS_DupValue (ctx, layer);
    }

    return JS_UNDEFINED;
}

JSValue get_layer_by_id_call (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 1) {
	return JS_UNDEFINED;
    }

    int id = 0;

    if (JS_ToInt32 (ctx, &id, argv[0]) != 0) {
	return JS_EXCEPTION;
    }

    return get_layer_by_id (*get_opaque (this_val), id);
}

JSValue get_layer_count (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return JS_NewInt32 (ctx, static_cast<int> (get_opaque (this_val)->getScene ().getLayers ().size ()));
}

JSValue enumerate_layers (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);
    JSValue result = JS_NewArray (ctx);
    uint32_t count = 0;

    for (auto* object : container->getScene ().getLayers ()) {
	if (object->is<ScriptableObject> ()) {
	    JS_SetPropertyUint32 (ctx, result, count++, instantiate_layer (*container, object));
	}
    }

    return result;
}

JSValue get_layer_index (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc != 1) {
	return JS_NewInt32 (ctx, -1);
    }

    auto* container = get_opaque (this_val);
    auto* layer = WallpaperEngine::Scripting::Adapters::ScriptableObjectAdapter::getObject (argv[0]);

    if (layer == nullptr) {
	return JS_NewInt32 (ctx, -1);
    }

    return JS_NewInt32 (ctx, container->getScene ().getObjectIndex (layer));
}

// WE turns a configuration object into scene.json object form with _Internal.stringifyConfig from baseclasses.js
std::optional<JSON> stringify_layer_config (JSContext* ctx, JSValueConst config) {
    const JSValue global = JS_GetGlobalObject (ctx);
    const JSValue internal = JS_GetPropertyStr (ctx, global, "_Internal");
    const JSValue stringify = JS_GetPropertyStr (ctx, internal, "stringifyConfig");
    JSValue result = JS_UNDEFINED;

    if (JS_IsFunction (ctx, stringify)) {
	result = JS_Call (ctx, stringify, internal, 1, &config);
    }

    JS_FreeValue (ctx, stringify);
    JS_FreeValue (ctx, internal);
    JS_FreeValue (ctx, global);

    if (JS_IsException (result)) {
	JS_FreeValue (ctx, JS_GetException (ctx));
	return std::nullopt;
    }

    const char* text = JS_IsString (result) ? JS_ToCString (ctx, result) : nullptr;
    JS_FreeValue (ctx, result);

    if (text == nullptr) {
	return std::nullopt;
    }

    ScopeGuard guard ([=] { JS_FreeCString (ctx, text); });
    JSON parsed = JSON::parse (text, nullptr, false);

    if (!parsed.is_object ()) {
	return std::nullopt;
    }

    return parsed;
}

// IModelData, scenescript64 2.8.42: config parsing sub_18162FD40 / sub_181630720, createModelData sub_1816361F0,
// applyData / replaceData sub_181636B60 / sub_181636EF0, destroyModelData sub_181636890. Every error is a SyntaxError
namespace ModelDataScript {
using namespace WallpaperEngine::Render;

struct Handle {
    SceneObject* scene;
    uint32_t token;
};

JSValue property (JSContext* ctx, JSValueConst object, const char* name) {
    return JS_GetPropertyStr (ctx, object, name);
}

template <typename T> bool readTypedArray (JSContext* ctx, JSValueConst value, int type, std::vector<T>& out) {
    if (JS_GetTypedArrayType (value) != type) {
	return false;
    }

    size_t offset = 0;
    size_t length = 0;
    size_t elementSize = 0;
    JSValue buffer = JS_GetTypedArrayBuffer (ctx, value, &offset, &length, &elementSize);
    size_t size = 0;
    const uint8_t* data = JS_GetArrayBuffer (ctx, &size, buffer);

    out.resize (length / sizeof (T));

    if (data != nullptr && !out.empty () && offset + length <= size) {
	std::memcpy (out.data (), data + offset, out.size () * sizeof (T));
    }

    JS_FreeValue (ctx, buffer);
    return true;
}

/** false with a pending exception */
bool parseShape (JSContext* ctx, JSValueConst value, bool create, ModelData::ShapeConfig& shape) {
    static const std::map<std::string, uint32_t> formats = {
	{ "position", ModelData::FORMAT_POSITION },
	{ "normal", ModelData::FORMAT_NORMAL },
	{ "tangentSigned", ModelData::FORMAT_TANGENT_SIGNED },
	{ "uv", ModelData::FORMAT_UV },
	{ "color", ModelData::FORMAT_COLOR },
    };

    if (!JS_IsObject (value)) {
	shape.deleteShape = JS_IsNull (value);
	return true;
    }

    JSValue vertexBuffer = property (ctx, value, "vertexBuffer");
    shape.hasVertices = readTypedArray (ctx, vertexBuffer, JS_TYPED_ARRAY_FLOAT32, shape.vertices);
    JS_FreeValue (ctx, vertexBuffer);

    if (!shape.hasVertices && create) {
	JS_ThrowSyntaxError (ctx, "Vertex buffer missing.");
	return false;
    }

    JSValue indexBuffer = property (ctx, value, "indexBuffer");

    if (readTypedArray (ctx, indexBuffer, JS_TYPED_ARRAY_UINT16, shape.indices16)) {
	shape.hasIndices16 = true;
    } else if (readTypedArray (ctx, indexBuffer, JS_TYPED_ARRAY_UINT32, shape.indices32)) {
	shape.hasIndices32 = true;
    } else {
	shape.deleteIndices = JS_IsNull (indexBuffer);
    }

    JS_FreeValue (ctx, indexBuffer);

    JSValue vertexFormat = property (ctx, value, "vertexFormat");

    if (JS_IsArray (vertexFormat)) {
	int64_t length = 0;
	JS_GetLength (ctx, vertexFormat, &length);

	for (int64_t i = 0; i < length; i++) {
	    JSValue entry = JS_GetPropertyInt64 (ctx, vertexFormat, i);

	    if (!JS_IsString (entry)) {
		JS_FreeValue (ctx, entry);
		break;
	    }

	    const char* name = JS_ToCString (ctx, entry);

	    if (const auto format = formats.find (name != nullptr ? name : ""); format != formats.end ()) {
		shape.format |= format->second;
	    }

	    JS_FreeCString (ctx, name);
	    JS_FreeValue (ctx, entry);
	}
    }

    JS_FreeValue (ctx, vertexFormat);

    if (create && shape.format == 0) {
	JS_ThrowSyntaxError (ctx, "Vertex format missing.");
	return false;
    }

    JSValue material = property (ctx, value, "material");

    if (JS_IsString (material)) {
	const char* path = JS_ToCString (ctx, material);
	// written into a 256 byte buffer
	shape.material = std::string (path != nullptr ? path : "").substr (0, 255);
	JS_FreeCString (ctx, path);
    } else if (create) {
	JS_FreeValue (ctx, material);
	JS_ThrowSyntaxError (ctx, "Material missing.");
	return false;
    }

    JS_FreeValue (ctx, material);

    for (const auto& [name, target] :
	 { std::pair<const char*, bool*> { "isVertexBufferDynamic", &shape.vertexDynamic },
	   std::pair<const char*, bool*> { "isIndexBufferDynamic", &shape.indexDynamic } }) {
	JSValue flag = property (ctx, value, name);

	if (JS_IsBool (flag)) {
	    *target = JS_ToBool (ctx, flag);
	}

	JS_FreeValue (ctx, flag);
    }

    return true;
}

std::optional<glm::vec3> readVec3 (JSContext* ctx, JSValueConst object, const char* name) {
    JSValue value = property (ctx, object, name);
    std::optional<glm::vec3> result;

    if (JS_IsObject (value)) {
	glm::vec3 vector (0.0f);

	for (int axis = 0; axis < 3; axis++) {
	    JSValue component = property (ctx, value, std::array { "x", "y", "z" }[axis]);
	    double number = 0.0;

	    JS_ToFloat64 (ctx, &number, component);
	    vector[axis] = static_cast<float> (number);
	    JS_FreeValue (ctx, component);
	}

	result = vector;
    }

    JS_FreeValue (ctx, value);
    return result;
}

bool parseConfig (JSContext* ctx, JSValueConst value, bool create, ModelData::Config& config) {
    JSValue shapes = property (ctx, value, "shapes");
    bool ok = true;

    // an object without a shapes array is a single shape
    if (JS_IsArray (shapes)) {
	int64_t length = 0;
	JS_GetLength (ctx, shapes, &length);

	for (int64_t i = 0; i < length && ok; i++) {
	    JSValue entry = JS_GetPropertyInt64 (ctx, shapes, i);
	    ok = parseShape (ctx, entry, create, config.shapes.emplace_back ());
	    JS_FreeValue (ctx, entry);
	}
    } else {
	ok = parseShape (ctx, value, create, config.shapes.emplace_back ());
    }

    JS_FreeValue (ctx, shapes);

    if (!ok) {
	return false;
    }

    const auto boundsMin = readVec3 (ctx, value, "boundingBoxMins");
    const auto boundsMax = readVec3 (ctx, value, "boundingBoxMaxs");

    if (boundsMin.has_value () && boundsMax.has_value ()) {
	config.boundsMin = boundsMin;
	config.boundsMax = boundsMax;
    }

    if (config.shapes.empty ()) {
	JS_ThrowSyntaxError (ctx, "Shapes missing.");
	return false;
    }

    return true;
}

JSValue throwError (JSContext* ctx, ModelData::Error error) {
    return JS_ThrowSyntaxError (ctx, "%s", ModelData::errorMessage (error));
}

JSValue applyOrReplace (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    JSClassID classId = 0;
    auto* handle = static_cast<Handle*> (JS_GetAnyOpaque (this_val, &classId));

    if (handle == nullptr || classId != handle->scene->getModelDataClassId () || argc < 1) {
	return JS_UNDEFINED;
    }

    const bool replace = magic == 1;

    if (replace && handle->scene->getEngine ().isRunningUpdate ()) {
	return JS_ThrowSyntaxError (ctx, "IModelData.replace cannot be called in update.");
    }

    ModelData::Config config;

    if (!parseConfig (ctx, argv[0], false, config)) {
	return JS_EXCEPTION;
    }

    ModelData::Error error = ModelData::Error::None;
    handle->scene->getScene ().applyModelData (handle->token, config, replace, error);

    return error == ModelData::Error::None ? JS_UNDEFINED : throwError (ctx, error);
}

JSValue instantiate (JSContext* ctx, SceneObject& scene, uint32_t token) {
    // prototype: baseclasses.js IModelData (constants, toConfigString returning __modelDataToken)
    JSValue global = JS_GetGlobalObject (ctx);
    JSValue constructor = JS_GetPropertyStr (ctx, global, "IModelData");
    JSValue prototype = JS_IsObject (constructor) ? JS_GetPropertyStr (ctx, constructor, "prototype") : JS_NULL;
    JSValue object = JS_NewObjectProtoClass (ctx, prototype, scene.getModelDataClassId ());

    JS_FreeValue (ctx, prototype);
    JS_FreeValue (ctx, constructor);
    JS_FreeValue (ctx, global);

    JS_SetOpaque (object, new Handle { &scene, token });
    JS_DefinePropertyValueStr (
	ctx, object, "applyData", JS_NewCFunctionMagic (ctx, applyOrReplace, "applyData", 1, JS_CFUNC_generic_magic, 0),
	JS_PROP_C_W_E
    );
    JS_DefinePropertyValueStr (
	ctx, object, "replaceData",
	JS_NewCFunctionMagic (ctx, applyOrReplace, "replaceData", 1, JS_CFUNC_generic_magic, 1), JS_PROP_C_W_E
    );
    // ReadOnly | DontEnum | DontDelete
    JS_DefinePropertyValueStr (ctx, object, "__modelDataToken", JS_NewUint32 (ctx, token), 0);

    return object;
}
} // namespace ModelDataScript

// scenescript64 sub_1816372D0: a string is a name (the first layer with it), falling back to strtol as an id, a
// number is an index, a layer object is itself
WallpaperEngine::Render::CObject* resolve_layer_object (JSContext* ctx, SceneObject& container, JSValueConst value) {
    const auto layers = container.getScene ().getLayers ();

    if (JS_IsString (value)) {
	const char* text = JS_ToCString (ctx, value);

	if (text == nullptr) {
	    return nullptr;
	}

	ScopeGuard guard ([=] { JS_FreeCString (ctx, text); });

	for (auto* layer : layers) {
	    if (layer->getObject ().name == text) {
		return layer;
	    }
	}

	const long id = std::strtol (text, nullptr, 10);

	for (auto* layer : layers) {
	    if (layer->getId () == id) {
		return layer;
	    }
	}

	return nullptr;
    }

    if (JS_IsNumber (value)) {
	int32_t index = 0;
	JS_ToInt32 (ctx, &index, value);

	return static_cast<uint32_t> (index) < layers.size () ? layers[static_cast<uint32_t> (index)] : nullptr;
    }

    if (auto* object = WallpaperEngine::Scripting::Adapters::ScriptableObjectAdapter::getObject (value);
	object != nullptr) {
	return object;
    }

    return nullptr;
}

// sub_181634980: the object's creation JSON without its id, parsed again. The global scope message really names
// destroyLayer in WE
JSValue get_initial_layer_config (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    if (container->getEngine ().isEvaluatingModuleBody ()) {
	return JS_ThrowSyntaxError (ctx, "destroyLayer cannot be called from global scope.");
    }

    const auto* object = argc < 1 ? nullptr : resolve_layer_object (ctx, *container, argv[0]);

    if (object == nullptr) {
	return JS_NULL;
    }

    const std::string& config = object->getObject ().initialConfig;
    JSValue parsed = JS_ParseJSON (ctx, config.c_str (), config.size (), "<config>");

    if (JS_IsException (parsed)) {
	JS_FreeValue (ctx, JS_GetException (ctx));
	return JS_NULL;
    }

    return parsed;
}

// sub_181635540: the static scene camera as {eye, center, up, zoom}, a plain object with Vec3s
JSValue get_camera_transforms (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    if (container->getEngine ().isEvaluatingModuleBody ()) {
	return JS_ThrowSyntaxError (ctx, "getCameraTransforms cannot be called from global scope.");
    }

    const auto& camera = container->getScene ().getStaticCamera ();
    const auto& adapters = container->getEngine ().getAdapters ();
    const auto vector = [&adapters] (const glm::vec3& value) {
	WallpaperEngine::Data::Model::DynamicValue dynamic (value);

	return adapters.vec3->instantiate (dynamic);
    };
    JSValue result = JS_NewObject (ctx);

    JS_SetPropertyStr (ctx, result, "eye", vector (camera.eye));
    JS_SetPropertyStr (ctx, result, "center", vector (camera.center));
    JS_SetPropertyStr (ctx, result, "up", vector (camera.up));
    JS_SetPropertyStr (ctx, result, "zoom", JS_NewFloat64 (ctx, static_cast<double> (camera.zoom)));

    return result;
}

// sub_1816359E0: sets whichever of eye, center, up (objects) and zoom (a number) are there, true when given an object
JSValue set_camera_transforms (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    if (container->getEngine ().isEvaluatingModuleBody ()) {
	return JS_ThrowSyntaxError (ctx, "setCameraTransforms cannot be called from global scope.");
    }

    if (argc < 1 || !JS_IsObject (argv[0])) {
	return JS_FALSE;
    }

    auto& camera = container->getScene ().getStaticCamera ();

    for (const auto& [name, target] : { std::pair<const char*, glm::vec3*> { "eye", &camera.eye },
					std::pair<const char*, glm::vec3*> { "center", &camera.center },
					std::pair<const char*, glm::vec3*> { "up", &camera.up } }) {
	if (const auto value = ModelDataScript::readVec3 (ctx, argv[0], name); value.has_value ()) {
	    *target = *value;
	}
    }

    JSValue zoom = JS_GetPropertyStr (ctx, argv[0], "zoom");
    double number = 0.0;

    if (JS_IsNumber (zoom) && JS_ToFloat64 (ctx, &number, zoom) == 0) {
	camera.zoom = static_cast<float> (number);
    }

    JS_FreeValue (ctx, zoom);
    return JS_TRUE;
}

JSValue create_model_data (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    if (container->getEngine ().isEvaluatingModuleBody ()) {
	return JS_ThrowSyntaxError (ctx, "createModelData cannot be called from global scope.");
    }

    // no configuration object gives null (sub_1816361F0 returns the isolate's null root)
    if (argc < 1 || !JS_IsObject (argv[0])) {
	return JS_NULL;
    }

    WallpaperEngine::Render::ModelData::Config config;

    if (!ModelDataScript::parseConfig (ctx, argv[0], true, config)) {
	return JS_EXCEPTION;
    }

    auto error = WallpaperEngine::Render::ModelData::Error::None;
    const uint32_t token = container->getScene ().createModelData (config, error);

    if (error != WallpaperEngine::Render::ModelData::Error::None) {
	return ModelDataScript::throwError (ctx, error);
    }

    return ModelDataScript::instantiate (ctx, *container, token);
}

// takes the token as a number (V8 IsUint32 + Int32Value), anything else does nothing
JSValue destroy_model_data (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* container = get_opaque (this_val);

    if (container->getEngine ().isEvaluatingModuleBody ()) {
	return JS_ThrowSyntaxError (ctx, "destroyModelData cannot be called from global scope.");
    }

    double number = 0.0;

    if (argc < 1 || !JS_IsNumber (argv[0]) || JS_ToFloat64 (ctx, &number, argv[0]) != 0 || number < 0.0
	|| number > 4294967295.0 || number != std::floor (number)) {
	return JS_UNDEFINED;
    }

    container->getScene ().destroyModelData (static_cast<uint32_t> (static_cast<int64_t> (number)));
    return JS_UNDEFINED;
}

JSValue create_layer (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc != 1) {
	return JS_UNDEFINED;
    }

    auto* container = get_opaque (this_val);

    if (JS_IsObject (argv[0])) {
	JSValue configuration = JS_DupValue (ctx, argv[0]);

	// an IModelData on its own is a model layer of it (scenescript64 sub_181633290 checks the handle's tag)
	if (JS_GetOpaque (argv[0], container->getModelDataClassId ()) != nullptr) {
	    JS_FreeValue (ctx, configuration);
	    configuration = JS_NewObject (ctx);
	    JS_SetPropertyStr (ctx, configuration, "model", JS_DupValue (ctx, argv[0]));
	}

	const auto config = stringify_layer_config (ctx, configuration);
	JS_FreeValue (ctx, configuration);

	if (!config.has_value ()) {
	    return JS_UNDEFINED;
	}

	auto* object = container->getScene ().createLayerFromConfig (*config);

	if (object == nullptr || !object->is<ScriptableObject> ()) {
	    return JS_UNDEFINED;
	}

	return container->getEngine ().getAdapters ().object->instantiate (*object->as<ScriptableObject> ());
    }

    if (!JS_IsString (argv[0])) {
	return JS_UNDEFINED;
    }

    const char* path = JS_ToCString (ctx, argv[0]);

    if (path == nullptr) {
	return JS_UNDEFINED;
    }

    ScopeGuard guard ([=] { JS_FreeCString (ctx, path); });

    auto* object = container->getScene ().createLayer (path);

    if (object == nullptr || !object->is<ScriptableObject> ()) {
	return JS_UNDEFINED;
    }

    return container->getEngine ().getAdapters ().object->instantiate (*object->as<ScriptableObject> ());
}

JSValue sort_layer (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc != 2) {
	return JS_UNDEFINED;
    }

    auto* container = get_opaque (this_val);
    auto* layer = WallpaperEngine::Scripting::Adapters::ScriptableObjectAdapter::getObject (argv[0]);

    if (layer == nullptr) {
	return JS_UNDEFINED;
    }

    int index = 0;
    JS_ToInt32 (ctx, &index, argv[1]);

    container->getScene ().sortLayer (layer, index);

    return JS_UNDEFINED;
}

// magic is the property's index in the order SceneObject's constructor defines them
UserSetting* scene_setting (const Scene& scene, int magic) {
    switch (magic) {
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

JSValue scene_set_value (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    auto* container = get_opaque (this_val);
    auto* setting = scene_setting (container->getScene ().getScene (), magic);

    // clearenabled has no backing value, the write is dropped like any other unsupported scene setting
    if (argc < 1 || setting == nullptr || setting->value == nullptr) {
	return JS_UNDEFINED;
    }

    container->getEngine ().assignJsValue (argv[0], *setting->value);

    return JS_UNDEFINED;
}

SceneObject::SceneObject (ScriptEngine& engine, Render::Wallpapers::CScene& scene) :
    m_scene (scene), m_engine (engine), m_classId (0) {
    this->m_definition = { .class_name = "IScene" };
    JS_NewClassID (this->m_engine.getRuntime (), &this->m_classId);
    JS_NewClass (this->m_engine.getRuntime (), this->m_classId, &this->m_definition);
    this->m_instance = JS_NewObjectClass (this->m_engine.getContext (), this->m_classId);

    this->m_modelDataDefinition = {
	.class_name = "IModelData",
	.finalizer =
	    [] (JSRuntime*, JSValueConst value) {
		JSClassID classId = 0;
		delete static_cast<ModelDataScript::Handle*> (JS_GetAnyOpaque (value, &classId));
	    },
    };
    JS_NewClassID (this->m_engine.getRuntime (), &this->m_modelDataClassId);
    JS_NewClass (this->m_engine.getRuntime (), this->m_modelDataClassId, &this->m_modelDataDefinition);

    JS_DupValue (this->m_engine.getContext (), this->m_instance);

    JS_SetOpaque (this->m_instance, this);
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "bloom"),
	JS_NewCFunction (this->m_engine.getContext (), get_bloom, "get", 0),
	JS_NewCFunctionMagic (this->m_engine.getContext (), scene_set_value, "set", 1, JS_CFUNC_generic_magic, 0),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "bloomstrength"),
	JS_NewCFunction (this->m_engine.getContext (), get_bloomstrength, "get", 0),
	JS_NewCFunctionMagic (this->m_engine.getContext (), scene_set_value, "set", 1, JS_CFUNC_generic_magic, 1),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "bloomthreshold"),
	JS_NewCFunction (this->m_engine.getContext (), get_bloomthreshold, "get", 0),
	JS_NewCFunctionMagic (this->m_engine.getContext (), scene_set_value, "set", 1, JS_CFUNC_generic_magic, 2),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "clearenabled"),
	JS_NewCFunction (this->m_engine.getContext (), get_clearenabled, "get", 0),
	JS_NewCFunctionMagic (this->m_engine.getContext (), scene_set_value, "set", 1, JS_CFUNC_generic_magic, 3),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "clearcolor"),
	JS_NewCFunction (this->m_engine.getContext (), get_clearcolor, "get", 0),
	JS_NewCFunctionMagic (this->m_engine.getContext (), scene_set_value, "set", 1, JS_CFUNC_generic_magic, 4),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "ambientcolor"),
	JS_NewCFunction (this->m_engine.getContext (), get_ambientcolor, "get", 0),
	JS_NewCFunctionMagic (this->m_engine.getContext (), scene_set_value, "set", 1, JS_CFUNC_generic_magic, 5),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "skylightcolor"),
	JS_NewCFunction (this->m_engine.getContext (), get_skylightcolor, "get", 0),
	JS_NewCFunctionMagic (this->m_engine.getContext (), scene_set_value, "set", 1, JS_CFUNC_generic_magic, 6),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "fov"),
	JS_NewCFunction (this->m_engine.getContext (), get_fov, "get", 0),
	JS_NewCFunctionMagic (this->m_engine.getContext (), scene_set_value, "set", 1, JS_CFUNC_generic_magic, 7),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "nearz"),
	JS_NewCFunction (this->m_engine.getContext (), get_nearz, "get", 0),
	JS_NewCFunctionMagic (this->m_engine.getContext (), scene_set_value, "set", 1, JS_CFUNC_generic_magic, 8),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "farz"),
	JS_NewCFunction (this->m_engine.getContext (), get_farz, "get", 0),
	JS_NewCFunctionMagic (this->m_engine.getContext (), scene_set_value, "set", 1, JS_CFUNC_generic_magic, 9),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "camerafade"),
	JS_NewCFunction (this->m_engine.getContext (), get_camerafade, "get", 0),
	JS_NewCFunctionMagic (this->m_engine.getContext (), scene_set_value, "set", 1, JS_CFUNC_generic_magic, 10),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "camerashake"),
	JS_NewCFunction (this->m_engine.getContext (), get_camerashake, "get", 0),
	JS_NewCFunctionMagic (this->m_engine.getContext (), scene_set_value, "set", 1, JS_CFUNC_generic_magic, 11),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "camerashakespeed"),
	JS_NewCFunction (this->m_engine.getContext (), get_camerashakespeed, "get", 0),
	JS_NewCFunctionMagic (this->m_engine.getContext (), scene_set_value, "set", 1, JS_CFUNC_generic_magic, 12),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance,
	JS_NewAtom (this->m_engine.getContext (), "camerashakeamplitude"),
	JS_NewCFunction (this->m_engine.getContext (), get_camerashakeamplitude, "get", 0),
	JS_NewCFunctionMagic (this->m_engine.getContext (), scene_set_value, "set", 1, JS_CFUNC_generic_magic, 13),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance,
	JS_NewAtom (this->m_engine.getContext (), "camerashakeroughness"),
	JS_NewCFunction (this->m_engine.getContext (), get_camerashakeroughness, "get", 0),
	JS_NewCFunctionMagic (this->m_engine.getContext (), scene_set_value, "set", 1, JS_CFUNC_generic_magic, 14),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "cameraparallax"),
	JS_NewCFunction (this->m_engine.getContext (), get_cameraparallax, "get", 0),
	JS_NewCFunctionMagic (this->m_engine.getContext (), scene_set_value, "set", 1, JS_CFUNC_generic_magic, 15),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance,
	JS_NewAtom (this->m_engine.getContext (), "cameraparallaxamount"),
	JS_NewCFunction (this->m_engine.getContext (), get_cameraparallaxamount, "get", 0),
	JS_NewCFunctionMagic (this->m_engine.getContext (), scene_set_value, "set", 1, JS_CFUNC_generic_magic, 16),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance,
	JS_NewAtom (this->m_engine.getContext (), "cameraparallaxdelay"),
	JS_NewCFunction (this->m_engine.getContext (), get_cameraparallaxdelay, "get", 0),
	JS_NewCFunctionMagic (this->m_engine.getContext (), scene_set_value, "set", 1, JS_CFUNC_generic_magic, 17),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance,
	JS_NewAtom (this->m_engine.getContext (), "cameraparallaxmouseinfluence"),
	JS_NewCFunction (this->m_engine.getContext (), get_cameraparallaxmouseinfluence, "get", 0),
	JS_NewCFunctionMagic (this->m_engine.getContext (), scene_set_value, "set", 1, JS_CFUNC_generic_magic, 18),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "getLayer",
	JS_NewCFunction (this->m_engine.getContext (), get_layer, "getLayer", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "getLayerByID",
	JS_NewCFunction (this->m_engine.getContext (), get_layer_by_id_call, "getLayerByID", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "getLayerCount",
	JS_NewCFunction (this->m_engine.getContext (), get_layer_count, "getLayerCount", 0), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "enumerateLayers",
	JS_NewCFunction (this->m_engine.getContext (), enumerate_layers, "enumerateLayers", 0), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "getLayerIndex",
	JS_NewCFunction (this->m_engine.getContext (), get_layer_index, "getLayerIndex", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "createLayer",
	JS_NewCFunction (this->m_engine.getContext (), create_layer, "createLayer", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "sortLayer",
	JS_NewCFunction (this->m_engine.getContext (), sort_layer, "sortLayer", 2), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "getInitialLayerConfig",
	JS_NewCFunction (this->m_engine.getContext (), get_initial_layer_config, "getInitialLayerConfig", 1),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "getCameraTransforms",
	JS_NewCFunction (this->m_engine.getContext (), get_camera_transforms, "getCameraTransforms", 0),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "setCameraTransforms",
	JS_NewCFunction (this->m_engine.getContext (), set_camera_transforms, "setCameraTransforms", 1),
	JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "createModelData",
	JS_NewCFunction (this->m_engine.getContext (), create_model_data, "createModelData", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyValueStr (
	this->m_engine.getContext (), this->m_instance, "destroyModelData",
	JS_NewCFunction (this->m_engine.getContext (), destroy_model_data, "destroyModelData", 1), JS_PROP_ENUMERABLE
    );
}

SceneObject::~SceneObject () { JS_FreeValue (this->m_engine.getContext (), this->m_instance); }