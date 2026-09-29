#include "ScriptableObjectAdapter.h"

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ranges>
#include <utility>

#include "WallpaperEngine/Data/Model/DynamicValue.h"
#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Data/Utils/ScopeGuard.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/Objects/CImage.h"
#include "WallpaperEngine/Scripting/ScriptEngine.h"
#include "WallpaperEngine/Scripting/ScriptableObject.h"
#include "WallpaperEngine/VideoPlayback/MPV/GLPlayer.h"

using namespace WallpaperEngine::Data::Model;
using namespace WallpaperEngine::Data::Utils;
using namespace WallpaperEngine::Scripting::Adapters;

#define SCRIPTABLE_OPAQUE_MAGIC 0xdeadbeef

struct OpaqueScriptableObjectAdapter {
    unsigned int magic;
    ScriptableObjectAdapter& adapter;
    WallpaperEngine::Scripting::ScriptableObject& object;
};

// the object's address rides along as function data, safe because every ScriptableObject outlives the script context
JSValue scriptableobject_playback_call (
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValueConst* func_data
) {
    int64_t address = 0;
    JS_ToInt64 (ctx, &address, func_data[0]);
    auto* object = reinterpret_cast<WallpaperEngine::Scripting::ScriptableObject*> (static_cast<intptr_t> (address));

    if (magic == 2) {
	return JS_NewBool (ctx, object->isPlaying ());
    }

    using Playback = WallpaperEngine::Scripting::ScriptableObject::Playback;

    object->setPlayback (magic == 0 ? Playback::Playing : magic == 1 ? Playback::Paused : Playback::Stopped);

    return JS_UNDEFINED;
}

// only scriptable layers can be handed to scripts, a plain group parent comes back as null
JSValue scriptableobject_hierarchy_call (
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValueConst* func_data
) {
    int64_t objectAddress = 0;
    int64_t engineAddress = 0;
    JS_ToInt64 (ctx, &objectAddress, func_data[0]);
    JS_ToInt64 (ctx, &engineAddress, func_data[1]);
    auto* object
	= reinterpret_cast<WallpaperEngine::Scripting::ScriptableObject*> (static_cast<intptr_t> (objectAddress));
    auto* engine = reinterpret_cast<WallpaperEngine::Scripting::ScriptEngine*> (static_cast<intptr_t> (engineAddress));
    const auto& scene = engine->getScene ();

    if (magic == 0) {
	const auto& parentId = object->getObject ().parent;

	if (!parentId.has_value ()) {
	    return JS_NULL;
	}

	const auto* parent = scene.getObject (*parentId);

	if (parent == nullptr || !parent->is<WallpaperEngine::Scripting::ScriptableObject> ()) {
	    return JS_NULL;
	}

	return engine->getAdapters ().object->instantiate (
	    const_cast<WallpaperEngine::Scripting::ScriptableObject&> (
		*parent->as<WallpaperEngine::Scripting::ScriptableObject> ()
	    )
	);
    }

    JSValue children = JS_NewArray (ctx);
    uint32_t index = 0;

    for (const auto* candidate : scene.getObjectsByRenderOrder ()) {
	const auto& candidateParent = candidate->getObject ().parent;

	if (!candidateParent.has_value () || *candidateParent != object->getObject ().id
	    || !candidate->is<WallpaperEngine::Scripting::ScriptableObject> ()) {
	    continue;
	}

	JS_SetPropertyUint32 (
	    ctx, children, index++,
	    engine->getAdapters ().object->instantiate (
		const_cast<WallpaperEngine::Scripting::ScriptableObject&> (
		    *candidate->as<WallpaperEngine::Scripting::ScriptableObject> ()
		)
	    )
	);
    }

    return children;
}

// effect handles keep the effect's address, safe for the same reason as the object address above
JSValue scriptableeffect_visible_call (
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValueConst* func_data
) {
    int64_t effectAddress = 0;
    int64_t engineAddress = 0;
    JS_ToInt64 (ctx, &effectAddress, func_data[0]);
    JS_ToInt64 (ctx, &engineAddress, func_data[1]);
    auto* effect = reinterpret_cast<ImageEffect*> (static_cast<intptr_t> (effectAddress));
    auto* engine = reinterpret_cast<WallpaperEngine::Scripting::ScriptEngine*> (static_cast<intptr_t> (engineAddress));

    if (magic == 0) {
	return JS_NewBool (ctx, effect->visible->value->getBool ());
    }

    if (argc > 0) {
	engine->assignJsValue (argv[0], *effect->visible->value);
    }

    return JS_UNDEFINED;
}

const std::vector<ImageEffectUniquePtr>* effectsOf (const Object& object) {
    if (object.is<Image> ()) {
	return &object.as<Image> ()->effects;
    }

    if (object.is<Text> ()) {
	return &object.as<Text> ()->effects;
    }

    return nullptr;
}

JSValue
scriptableeffect_instantiate (JSContext* ctx, ImageEffect& effect, WallpaperEngine::Scripting::ScriptEngine& engine) {
    JSValue handle = JS_NewObject (ctx);
    JSValue data[] = {
	JS_NewInt64 (ctx, static_cast<int64_t> (reinterpret_cast<intptr_t> (&effect))),
	JS_NewInt64 (ctx, static_cast<int64_t> (reinterpret_cast<intptr_t> (&engine))),
    };
    const JSAtom visible = JS_NewAtom (ctx, "visible");

    JS_DefinePropertyGetSet (
	ctx, handle, visible, JS_NewCFunctionData (ctx, scriptableeffect_visible_call, 0, 0, 2, data),
	JS_NewCFunctionData (ctx, scriptableeffect_visible_call, 1, 1, 2, data), JS_PROP_ENUMERABLE
    );
    JS_FreeAtom (ctx, visible);
    JS_SetPropertyStr (ctx, handle, "name", JS_NewString (ctx, effect.name.c_str ()));

    return handle;
}

JSValue scriptableobject_effect_call (
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValueConst* func_data
) {
    int64_t objectAddress = 0;
    int64_t engineAddress = 0;
    JS_ToInt64 (ctx, &objectAddress, func_data[0]);
    JS_ToInt64 (ctx, &engineAddress, func_data[1]);
    auto* object
	= reinterpret_cast<WallpaperEngine::Scripting::ScriptableObject*> (static_cast<intptr_t> (objectAddress));
    auto* engine = reinterpret_cast<WallpaperEngine::Scripting::ScriptEngine*> (static_cast<intptr_t> (engineAddress));
    const auto* effects = effectsOf (object->getObject ());

    if (magic == 0) {
	return JS_NewInt32 (ctx, effects == nullptr ? 0 : static_cast<int> (effects->size ()));
    }

    if (effects == nullptr || argc < 1) {
	return JS_UNDEFINED;
    }

    if (JS_IsNumber (argv[0])) {
	int index = 0;
	JS_ToInt32 (ctx, &index, argv[0]);

	if (index < 0 || static_cast<size_t> (index) >= effects->size ()) {
	    return JS_UNDEFINED;
	}

	return scriptableeffect_instantiate (ctx, *(*effects)[index], *engine);
    }

    const char* name = JS_ToCString (ctx, argv[0]);

    if (name == nullptr) {
	return JS_UNDEFINED;
    }

    ScopeGuard guard ([=] { JS_FreeCString (ctx, name); });

    for (const auto& effect : *effects) {
	if (effect->name == name) {
	    return scriptableeffect_instantiate (ctx, *effect, *engine);
	}
    }

    return JS_UNDEFINED;
}

// video textures are only ever reached through the player's address, kept alive by the layer's texture for as long as
// the scene exists
JSValue videotexture_call (
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValueConst* func_data
) {
    int64_t playerAddress = 0;
    int64_t engineAddress = 0;
    JS_ToInt64 (ctx, &playerAddress, func_data[0]);
    JS_ToInt64 (ctx, &engineAddress, func_data[1]);
    auto* player
	= reinterpret_cast<WallpaperEngine::VideoPlayback::MPV::GLPlayer*> (static_cast<intptr_t> (playerAddress));
    auto* engine = reinterpret_cast<WallpaperEngine::Scripting::ScriptEngine*> (static_cast<intptr_t> (engineAddress));

    switch (magic) {
	case 0:
	    if (player->hasEnded ()) {
		player->seek (0.0);
	    }

	    player->clearPaused ();
	    return JS_UNDEFINED;
	case 1:
	    player->setPaused ();
	    return JS_UNDEFINED;
	case 2:
	    player->setPaused ();
	    player->seek (0.0);
	    return JS_UNDEFINED;
	case 3:
	    return JS_NewBool (ctx, !player->isPaused ());
	case 4:
	    return JS_NewBool (ctx, player->isPaused ());
	case 5:
	    return JS_NewBool (ctx, player->isPaused () && player->getPlaybackPosition () < 0.001);
	case 6:
	    return JS_NewFloat64 (ctx, player->getPlaybackPosition ());
	case 7:
	    {
		double seconds = 0.0;

		if (argc > 0 && JS_ToFloat64 (ctx, &seconds, argv[0]) == 0) {
		    player->seek (std::max (seconds, 0.0));
		}

		return JS_UNDEFINED;
	    }
	case 8:
	    if (argc > 0) {
		engine->addVideoEndedCallback (player, argv[0]);
	    }

	    return JS_UNDEFINED;
	case 9:
	    return JS_NewBool (ctx, player->isLooping ());
	case 10:
	    if (argc > 0) {
		player->setLoop (JS_ToBool (ctx, argv[0]) > 0);
	    }

	    return JS_UNDEFINED;
	case 11:
	    return JS_NewFloat64 (ctx, player->getDuration ());
	case 12:
	    return JS_NewFloat64 (ctx, player->getSpeed ());
	case 13:
	    {
		double rate = 0.0;

		if (argc > 0 && JS_ToFloat64 (ctx, &rate, argv[0]) == 0 && rate > 0.0) {
		    player->setSpeed (rate);
		}

		return JS_UNDEFINED;
	    }
	default:
	    return JS_UNDEFINED;
    }
}

JSValue videotexture_instantiate (
    JSContext* ctx, WallpaperEngine::VideoPlayback::MPV::GLPlayer& player,
    WallpaperEngine::Scripting::ScriptEngine& engine
) {
    JSValue handle = JS_NewObject (ctx);
    JSValue data[] = {
	JS_NewInt64 (ctx, static_cast<int64_t> (reinterpret_cast<intptr_t> (&player))),
	JS_NewInt64 (ctx, static_cast<int64_t> (reinterpret_cast<intptr_t> (&engine))),
    };
    static constexpr struct {
	const char* name;
	int magic;
	int length;
    } calls[] = { { "play", 0, 0 },           { "pause", 1, 0 },          { "stop", 2, 0 },
		  { "isPlaying", 3, 0 },      { "isPaused", 4, 0 },       { "isStopped", 5, 0 },
		  { "getCurrentTime", 6, 0 }, { "setCurrentTime", 7, 1 }, { "addEndedCallback", 8, 1 } };

    for (const auto& call : calls) {
	JS_SetPropertyStr (
	    ctx, handle, call.name, JS_NewCFunctionData (ctx, videotexture_call, call.length, call.magic, 2, data)
	);
    }

    static constexpr struct {
	const char* name;
	int getter;
	int setter;
    } properties[] = { { "loop", 9, 10 }, { "duration", 11, 14 }, { "rate", 12, 13 } };

    // "duration" gets an ignoring setter (14) since strict mode scripts throw on a property with none
    for (const auto& property : properties) {
	const JSAtom atom = JS_NewAtom (ctx, property.name);

	JS_DefinePropertyGetSet (
	    ctx, handle, atom, JS_NewCFunctionData (ctx, videotexture_call, 0, property.getter, 2, data),
	    JS_NewCFunctionData (ctx, videotexture_call, 1, property.setter, 2, data), JS_PROP_ENUMERABLE
	);
	JS_FreeAtom (ctx, atom);
    }

    return handle;
}

// scripts call this on image layers whose material is a video (mp4 texture), everything else gets null
JSValue scriptableobject_video_texture_call (
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValueConst* func_data
) {
    int64_t objectAddress = 0;
    int64_t engineAddress = 0;
    JS_ToInt64 (ctx, &objectAddress, func_data[0]);
    JS_ToInt64 (ctx, &engineAddress, func_data[1]);
    auto* object
	= reinterpret_cast<WallpaperEngine::Scripting::ScriptableObject*> (static_cast<intptr_t> (objectAddress));
    auto* engine = reinterpret_cast<WallpaperEngine::Scripting::ScriptEngine*> (static_cast<intptr_t> (engineAddress));

    if (!object->is<WallpaperEngine::Render::Objects::CImage> ()) {
	return JS_NULL;
    }

    const auto texture = object->as<WallpaperEngine::Render::Objects::CImage> ()->getTexture ();

    if (texture == nullptr || texture->getPlayer () == nullptr) {
	return JS_NULL;
    }

    return videotexture_instantiate (ctx, *texture->getPlayer (), *engine);
}

namespace {
enum BoneCall {
    GetBoneCount,
    GetBoneTransform,
    SetBoneTransform,
    GetLocalBoneTransform,
    SetLocalBoneTransform,
    GetLocalBoneAngles,
    SetLocalBoneAngles,
    GetLocalBoneOrigin,
    SetLocalBoneOrigin,
    GetBoneIndex,
    GetBoneParentIndex,
    ApplyBonePhysicsImpulse,
    ResetBonePhysicsSimulation,
};

// WE's own Mat4 from baseclasses.js: m[i] is float i of the native matrix (scenescript64 sub_1816214A0)
JSValue makeMat4 (JSContext* ctx, const WallpaperEngine::Scripting::ScriptEngine& engine, const glm::mat4& matrix) {
    JSValue prototype = JS_GetPropertyStr (ctx, engine.getGlobalThis (), "_Mat4");
    JSValue result = JS_IsObject (prototype) ? JS_NewObjectProto (ctx, prototype) : JS_NewObject (ctx);
    JS_FreeValue (ctx, prototype);

    JSValue values = JS_NewArray (ctx);
    const float* floats = glm::value_ptr (matrix);
    for (uint32_t i = 0; i < 16; i++) {
	JS_SetPropertyUint32 (ctx, values, i, JS_NewFloat64 (ctx, floats[i]));
    }
    JS_SetPropertyStr (ctx, result, "m", values);

    return result;
}

// elements that aren't numbers keep the identity's value, like sub_1816214A0
glm::mat4 readMat4 (JSContext* ctx, JSValueConst value) {
    glm::mat4 result (1.0f);

    if (!JS_IsObject (value)) {
	return result;
    }

    JSValue values = JS_GetPropertyStr (ctx, value, "m");
    float* floats = glm::value_ptr (result);

    if (JS_IsObject (values)) {
	for (uint32_t i = 0; i < 16; i++) {
	    JSValue element = JS_GetPropertyUint32 (ctx, values, i);
	    double number = 0.0;

	    if (JS_IsNumber (element) && JS_ToFloat64 (ctx, &number, element) == 0) {
		floats[i] = static_cast<float> (number);
	    }

	    JS_FreeValue (ctx, element);
	}
    }

    JS_FreeValue (ctx, values);
    return result;
}

glm::vec3 readVec3 (JSContext* ctx, JSValueConst value) {
    glm::vec3 result (0.0f);

    if (!JS_IsObject (value)) {
	return result;
    }

    const char* names[] = { "x", "y", "z" };
    for (int i = 0; i < 3; i++) {
	JSValue component = JS_GetPropertyStr (ctx, value, names[i]);
	double number = 0.0;

	if (JS_IsNumber (component) && JS_ToFloat64 (ctx, &number, component) == 0) {
	    result[i] = static_cast<float> (number);
	}

	JS_FreeValue (ctx, component);
    }

    return result;
}

// getLocalBoneAngles (sub_14020FA10), on WE's row-major floats
glm::vec3 localBoneAngles (const glm::mat4& local) {
    const float* m = glm::value_ptr (local);
    const float z = std::atan2 (m[1], m[0]);
    const float y = std::atan2 (-m[2], std::sqrt (m[6] * m[6] + m[10] * m[10]));
    const float sz = std::sin (z);
    const float cz = std::cos (z);
    const float x = std::atan2 (sz * m[8] - cz * m[9], cz * m[5] - sz * m[4]);

    return { x, y, z };
}

// setLocalBoneAngles (sub_14020FCE0): the rotation rows are replaced, scale dropped, the origin kept
void setLocalBoneAngles (glm::mat4& local, const glm::vec3& angles) {
    const float cx = std::cos (angles.x), sx = std::sin (angles.x);
    const float cy = std::cos (angles.y), sy = std::sin (angles.y);
    const float cz = std::cos (angles.z), sz = std::sin (angles.z);
    float* m = glm::value_ptr (local);

    m[0] = cy * cz;
    m[1] = cy * sz;
    m[2] = -sy;
    m[3] = 0.0f;
    m[4] = sy * cz * sx - cx * sz;
    m[5] = sy * sz * sx + cx * cz;
    m[6] = sx * cy;
    m[7] = 0.0f;
    m[8] = cx * cz * sy + sx * sz;
    m[9] = cx * sz * sy - sx * cz;
    m[10] = cx * cy;
    m[11] = 0.0f;
}
} // namespace

// thisLayer bone calls (wallpaper64 2.8.42 image methods, sub_140211070). A bone is a number (index) or a string
// (name); anything else does nothing, like a failed lookup
JSValue scriptableobject_bone_call (
    JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValueConst* func_data
) {
    using WallpaperEngine::Render::Objects::CImage;

    int64_t objectAddress = 0;
    int64_t engineAddress = 0;
    JS_ToInt64 (ctx, &objectAddress, func_data[0]);
    JS_ToInt64 (ctx, &engineAddress, func_data[1]);
    auto* object
	= reinterpret_cast<WallpaperEngine::Scripting::ScriptableObject*> (static_cast<intptr_t> (objectAddress));
    auto* engine = reinterpret_cast<WallpaperEngine::Scripting::ScriptEngine*> (static_cast<intptr_t> (engineAddress));

    // both index lookups write -1 before looking at the layer
    const JSValue fallback = magic == GetBoneIndex || magic == GetBoneParentIndex ? JS_NewInt32 (ctx, -1) : JS_UNDEFINED;

    if (!object->is<CImage> ()) {
	return fallback;
    }

    auto* image = object->as<CImage> ();
    const auto& bones = image->getPuppetBones ();
    const int count = static_cast<int> (bones.size ());

    if (count == 0) {
	return fallback;
    }

    if (magic == GetBoneCount) {
	return JS_NewInt32 (ctx, count);
    }

    const JSValueConst boneArgument = argc > 0 ? argv[0] : JS_UNDEFINED;
    std::optional<int> index;
    std::optional<std::string> name;

    if (JS_IsNumber (boneArgument)) {
	int32_t value = 0;
	JS_ToInt32 (ctx, &value, boneArgument);
	index = value;
    } else if (JS_IsString (boneArgument)) {
	const char* value = JS_ToCString (ctx, boneArgument);
	if (value != nullptr) {
	    name = value;
	    JS_FreeCString (ctx, value);
	}
    }

    if (!index.has_value () && !name.has_value ()) {
	return fallback;
    }

    if (magic == GetBoneIndex) {
	// names only, an index argument or an empty name finds nothing
	if (!name.has_value () || name->empty ()) {
	    return fallback;
	}

	return JS_NewInt32 (ctx, image->findPuppetBone (*name));
    }

    if (magic == GetBoneParentIndex) {
	// by name the first bone of that name that has a parent (sub_140210860)
	for (int bone = 0; bone < count; bone++) {
	    const bool matches = index.has_value () ? bone == *index : !name->empty () && bones[bone].name == *name;

	    if (matches && bones[bone].parent != -1) {
		return JS_NewInt32 (ctx, bones[bone].parent);
	    }
	}

	return fallback;
    }

    // an empty name is the first bone here (sub_140210990, sub_140210E10), the other calls need a match
    int bone = -1;
    if (index.has_value ()) {
	bone = *index;
    } else if (magic == ApplyBonePhysicsImpulse || magic == ResetBonePhysicsSimulation) {
	bone = name->empty () ? 0 : image->findPuppetBone (*name);
    } else if (!name->empty ()) {
	bone = image->findPuppetBone (*name);
    }

    if (bone < 0 || bone >= count) {
	return fallback;
    }

    if (magic == ApplyBonePhysicsImpulse) {
	image->applyPuppetBonePhysicsImpulse (
	    bone, readVec3 (ctx, argc > 1 ? argv[1] : JS_UNDEFINED), readVec3 (ctx, argc > 2 ? argv[2] : JS_UNDEFINED)
	);
	return JS_UNDEFINED;
    }

    if (magic == ResetBonePhysicsSimulation) {
	image->resetPuppetBonePhysics (bone);
	return JS_UNDEFINED;
    }

    // the matrices exist from the layer's first update on
    if (!image->hasPuppetPose ()) {
	return fallback;
    }

    const JSValueConst value = argc > 1 ? argv[1] : JS_UNDEFINED;
    const auto makeVec3 = [engine] (const glm::vec3& vector) {
	WallpaperEngine::Data::Model::DynamicValue dynamic (vector);
	return engine->getAdapters ().vec3->instantiate (dynamic);
    };

    switch (magic) {
	case GetBoneTransform:
	    return makeMat4 (ctx, *engine, image->getPuppetBoneTransform (bone));
	case SetBoneTransform:
	    image->setPuppetBoneTransform (bone, readMat4 (ctx, value));
	    break;
	case GetLocalBoneTransform:
	    return makeMat4 (ctx, *engine, image->getPuppetLocalBoneTransform (bone));
	case SetLocalBoneTransform:
	    image->setPuppetLocalBoneTransform (bone, readMat4 (ctx, value));
	    break;
	case GetLocalBoneAngles:
	    return makeVec3 (localBoneAngles (image->getPuppetLocalBoneTransform (bone)));
	case SetLocalBoneAngles: {
	    glm::mat4 local = image->getPuppetLocalBoneTransform (bone);
	    setLocalBoneAngles (local, readVec3 (ctx, value));
	    image->setPuppetLocalBoneTransform (bone, local);
	    break;
	}
	case GetLocalBoneOrigin:
	    return makeVec3 (glm::vec3 (image->getPuppetLocalBoneTransform (bone)[3]));
	case SetLocalBoneOrigin: {
	    // sub_140210250: floats 12..14, the rest stays
	    glm::mat4 local = image->getPuppetLocalBoneTransform (bone);
	    local[3] = glm::vec4 (readVec3 (ctx, value), local[3].w);
	    image->setPuppetLocalBoneTransform (bone, local);
	    break;
	}
	default:
	    break;
    }

    return JS_UNDEFINED;
}

JSValue scriptableobject_property_get (JSContext* ctx, JSValueConst obj_val, JSAtom atom, JSValueConst receiver) {
    JSClassID classId = 0;

    auto* container = static_cast<OpaqueScriptableObjectAdapter*> (JS_GetAnyOpaque (obj_val, &classId));

    if (!container || container->magic != SCRIPTABLE_OPAQUE_MAGIC) {
	return JS_ThrowTypeError (ctx, "scriptableobject_property_get: not a layer");
    }

    const char* name = JS_AtomToCString (ctx, atom);

    if (name == nullptr) {
	return JS_ThrowTypeError (ctx, "scriptableobject_property_get: invalid property name");
    }

    ScopeGuard guard ([=] { JS_FreeCString (ctx, name); });

    if (auto* property = container->object.tryGetProperty (name); property != nullptr) {
	return container->adapter.getEngine ().propertyToJs (*property, name);
    }

    static constexpr struct {
	const char* name;
	int magic;
    } playbackCalls[] = { { "play", 0 }, { "pause", 1 }, { "stop", 3 }, { "isPlaying", 2 } };

    for (const auto& call : playbackCalls) {
	if (std::strcmp (name, call.name) == 0) {
	    JSValue address[]
		= { JS_NewInt64 (ctx, static_cast<int64_t> (reinterpret_cast<intptr_t> (&container->object))) };

	    return JS_NewCFunctionData (ctx, scriptableobject_playback_call, 0, call.magic, 1, address);
	}
    }

    static constexpr struct {
	const char* name;
	int magic;
    } hierarchyCalls[] = { { "getParent", 0 }, { "getChildren", 1 } };

    for (const auto& call : hierarchyCalls) {
	if (std::strcmp (name, call.name) == 0) {
	    JSValue data[] = {
		JS_NewInt64 (ctx, static_cast<int64_t> (reinterpret_cast<intptr_t> (&container->object))),
		JS_NewInt64 (ctx, static_cast<int64_t> (reinterpret_cast<intptr_t> (&container->adapter.getEngine ()))),
	    };

	    return JS_NewCFunctionData (ctx, scriptableobject_hierarchy_call, 0, call.magic, 2, data);
	}
    }

    static constexpr struct {
	const char* name;
	int magic;
    } effectCalls[] = { { "getEffectCount", 0 }, { "getEffect", 1 } };

    for (const auto& call : effectCalls) {
	if (std::strcmp (name, call.name) == 0) {
	    JSValue data[] = {
		JS_NewInt64 (ctx, static_cast<int64_t> (reinterpret_cast<intptr_t> (&container->object))),
		JS_NewInt64 (ctx, static_cast<int64_t> (reinterpret_cast<intptr_t> (&container->adapter.getEngine ()))),
	    };

	    return JS_NewCFunctionData (ctx, scriptableobject_effect_call, 1, call.magic, 2, data);
	}
    }

    static constexpr struct {
	const char* name;
	int magic;
	int length;
    } boneCalls[] = {
	{ "getBoneCount", GetBoneCount, 0 },
	{ "getBoneTransform", GetBoneTransform, 1 },
	{ "setBoneTransform", SetBoneTransform, 2 },
	{ "getLocalBoneTransform", GetLocalBoneTransform, 1 },
	{ "setLocalBoneTransform", SetLocalBoneTransform, 2 },
	{ "getLocalBoneAngles", GetLocalBoneAngles, 1 },
	{ "setLocalBoneAngles", SetLocalBoneAngles, 2 },
	{ "getLocalBoneOrigin", GetLocalBoneOrigin, 1 },
	{ "setLocalBoneOrigin", SetLocalBoneOrigin, 2 },
	{ "getBoneIndex", GetBoneIndex, 1 },
	{ "getBoneParentIndex", GetBoneParentIndex, 1 },
	{ "applyBonePhysicsImpulse", ApplyBonePhysicsImpulse, 3 },
	{ "resetBonePhysicsSimulation", ResetBonePhysicsSimulation, 1 },
    };

    for (const auto& call : boneCalls) {
	if (std::strcmp (name, call.name) == 0) {
	    JSValue data[] = {
		JS_NewInt64 (ctx, static_cast<int64_t> (reinterpret_cast<intptr_t> (&container->object))),
		JS_NewInt64 (ctx, static_cast<int64_t> (reinterpret_cast<intptr_t> (&container->adapter.getEngine ()))),
	    };

	    return JS_NewCFunctionData (ctx, scriptableobject_bone_call, call.length, call.magic, 2, data);
	}
    }

    if (std::strcmp (name, "getVideoTexture") == 0) {
	JSValue data[] = {
	    JS_NewInt64 (ctx, static_cast<int64_t> (reinterpret_cast<intptr_t> (&container->object))),
	    JS_NewInt64 (ctx, static_cast<int64_t> (reinterpret_cast<intptr_t> (&container->adapter.getEngine ()))),
	};

	return JS_NewCFunctionData (ctx, scriptableobject_video_texture_call, 0, 0, 2, data);
    }

    if (std::strcmp (name, "name") == 0) {
	return JS_NewString (ctx, container->object.getObject ().name.c_str ());
    }

    if (std::strcmp (name, "id") == 0) {
	return JS_NewInt32 (ctx, container->object.getObject ().id);
    }

    // "size" isn't a DynamicValue-backed property, but thisLayer.size is a commonly used part
    // of the WE scripting API, so it's special-cased here. Checked against the data model
    // (Image/Text) rather than the render object (CImage/CText) because scripted properties can
    // run their first update() from inside the base ScriptableObject constructor, before the
    // derived render object has finished constructing - a dynamic_cast to it at that point would
    // incorrectly report "not yet that type".
    if (std::strcmp (name, "size") == 0) {
	const auto& modelObject = container->object.getObject ();
	const glm::vec2* size = nullptr;

	if (modelObject.is<Image> ()) {
	    size = &modelObject.as<Image> ()->size;
	} else if (modelObject.is<Text> ()) {
	    size = &modelObject.as<Text> ()->size;
	}

	if (size != nullptr) {
	    const DynamicValue sizeValue (*size);

	    return container->adapter.getEngine ().getAdapters ().vec2->instantiate (
		const_cast<DynamicValue&> (sizeValue), true
	    );
	}
    }

    return JS_UNDEFINED;
}

int scriptableobject_property_set (
    JSContext* ctx, JSValueConst obj_val, JSAtom atom, JSValueConst val, JSValueConst receiver, int flags
) {
    JSClassID classId = 0;

    auto* container = static_cast<OpaqueScriptableObjectAdapter*> (JS_GetAnyOpaque (obj_val, &classId));

    if (!container || container->magic != SCRIPTABLE_OPAQUE_MAGIC) {
	return -1;
    }

    const char* name = JS_AtomToCString (ctx, atom);

    if (name == nullptr) {
	return -1;
    }

    ScopeGuard guard ([=] { JS_FreeCString (ctx, name); });

    // Write through to the real property so `thisLayer.visible = ...` etc. actually takes
    // effect. Properties not backed by a DynamicValue (e.g. "horizontalalign", plain model
    // strings) fall through and return 0 rather than -1: since scripts run as strict-mode ES
    // modules, returning -1 here would throw and abort the whole script over an unsupported
    // property, so silently accepting the write is the safer default.
    if (auto* property = container->object.tryGetProperty (name); property != nullptr) {
	container->adapter.getEngine ().assignPropertyJsValue (val, *property, name);
	return 0;
    }

    if (std::strcmp (name, "name") == 0 || std::strcmp (name, "id") == 0 || std::strcmp (name, "size") == 0) {
	return 0;
    }

    // WE's layer objects are ordinary V8 objects, scripts hang their own state off them
    // (3378399626 keeps each widget's base origin on the layer). Own properties are found
    // before the exotic handlers, so later reads and writes never come back here.
    return JS_DefinePropertyValue (ctx, receiver, atom, JS_DupValue (ctx, val), JS_PROP_C_W_E);
}

ScriptableObjectAdapter::ScriptableObjectAdapter (ScriptEngine& engine, std::string name) :
    ObjectAdapter (engine),
    m_exoticMethods ({ .get_property = scriptableobject_property_get, .set_property = scriptableobject_property_set }),
    m_name (std::move (name)) {
    this->registerType (
	{
	    .class_name = m_name.c_str (),
	    .exotic = &m_exoticMethods,
	}
    );
}

JSValue ScriptableObjectAdapter::instantiate (ScriptableObject& object) {
    JSContext* ctx = this->getEngine ().getContext ();

    if (const auto it = this->m_instances.find (&object); it != this->m_instances.end ()) {
	return JS_DupValue (ctx, it->second);
    }

    JSValue result = this->ObjectAdapter::instantiate (object);
    JS_SetOpaque (
	result,
	new OpaqueScriptableObjectAdapter { .magic = SCRIPTABLE_OPAQUE_MAGIC, .adapter = *this, .object = object }
    );

    this->m_instances.emplace (&object, JS_DupValue (ctx, result));
    return result;
}

void ScriptableObjectAdapter::forget (const ScriptableObject& object) {
    if (const auto it = this->m_instances.find (&object); it != this->m_instances.end ()) {
	JS_FreeValue (this->getEngine ().getContext (), it->second);
	this->m_instances.erase (it);
    }
}

void ScriptableObjectAdapter::clear () {
    for (const auto& value : this->m_instances | std::views::values) {
	JS_FreeValue (this->getEngine ().getContext (), value);
    }
    this->m_instances.clear ();
}

JSValue ScriptableObjectAdapter::instantiate (DynamicValue& value) {
    throw std::runtime_error ("Cannot create a ScriptableObject instance from a DynamicValue");
}

WallpaperEngine::Scripting::ScriptableObject* ScriptableObjectAdapter::getObject (JSValueConst value) {
    JSClassID classId = 0;
    auto* container = static_cast<OpaqueScriptableObjectAdapter*> (JS_GetAnyOpaque (value, &classId));

    if (container == nullptr || container->magic != SCRIPTABLE_OPAQUE_MAGIC) {
	return nullptr;
    }

    return &container->object;
}