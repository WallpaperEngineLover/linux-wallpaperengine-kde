#include "ScriptableObjectAdapter.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

#include "WallpaperEngine/Data/JSON.h"
#include "WallpaperEngine/Data/Model/DynamicValue.h"
#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/Objects/CCamera.h"
#include "WallpaperEngine/Render/Objects/CImage.h"
#include "WallpaperEngine/Render/Objects/CLight.h"
#include "WallpaperEngine/Render/Objects/CMesh.h"
#include "WallpaperEngine/Render/Objects/CParticle.h"
#include "WallpaperEngine/Render/Objects/CText.h"
#include "WallpaperEngine/Scripting/AnimationSystem.h"
#include "WallpaperEngine/Scripting/JS.h"
#include "WallpaperEngine/Scripting/ScriptEngine.h"
#include "WallpaperEngine/Scripting/ScriptableObject.h"
#include "WallpaperEngine/VideoPlayback/MPV/GLPlayer.h"

using namespace WallpaperEngine::Data::Model;
using namespace WallpaperEngine::Scripting;
using namespace WallpaperEngine::Scripting::Adapters;
using WallpaperEngine::Render::Objects::CImage;
using WallpaperEngine::Render::Objects::CMesh;
using WallpaperEngine::Render::Objects::CParticle;
using WallpaperEngine::Render::Objects::CText;

namespace {
ScriptEngine& engineOf (const v8::FunctionCallbackInfo<v8::Value>& info) {
    return ScriptEngine::from (info.GetIsolate ());
}

// the layer a method's data points at, nullptr once it's gone
ScriptableObject* layerObject (v8::Local<v8::Value> data) {
    const auto* layer = JS::unwrap<Layer> (data);

    return layer == nullptr ? nullptr : layer->object;
}

// scenescript64's dispatcher (sub_1816214A0) only takes a number as an index (flag 8) when V8 sees an int32
std::optional<int32_t> int32Argument (v8::Local<v8::Value> value) {
    if (!value->IsNumber ()) {
	return std::nullopt;
    }

    const double number = value.As<v8::Number> ()->Value ();

    if (number != std::trunc (number) || number < INT32_MIN || number > INT32_MAX) {
	return std::nullopt;
    }

    return static_cast<int32_t> (number);
}

// WE's own Mat4 from baseclasses.js: m[i] is float i of the native matrix (scenescript64 sub_1816214A0)
v8::Local<v8::Object> makeMat4 (const ScriptEngine& engine, const glm::mat4& matrix) {
    auto* isolate = engine.getIsolate ();
    const auto context = engine.getContext ();
    const v8::Local<v8::Object> result = v8::Object::New (isolate);

    if (const auto prototype = JS::get (context, engine.getGlobalThis (), "_Mat4"); prototype->IsObject ()) {
	JS::setPrototype (context, result, prototype);
    }

    const v8::Local<v8::Array> values = v8::Array::New (isolate, 16);
    const float* floats = glm::value_ptr (matrix);
    for (uint32_t i = 0; i < 16; i++) {
	values->Set (context, i, v8::Number::New (isolate, floats[i])).Check ();
    }
    JS::set (context, result, "m", values);

    return result;
}

// m[i] is float i of the native 3x3
v8::Local<v8::Object> makeMat3 (const ScriptEngine& engine, const std::array<float, 9>& matrix) {
    auto* isolate = engine.getIsolate ();
    const auto context = engine.getContext ();
    const v8::Local<v8::Object> result = v8::Object::New (isolate);

    if (const auto prototype = JS::get (context, engine.getGlobalThis (), "_Mat3"); prototype->IsObject ()) {
	JS::setPrototype (context, result, prototype);
    }

    const v8::Local<v8::Array> values = v8::Array::New (isolate, 9);
    for (uint32_t i = 0; i < 9; i++) {
	values->Set (context, i, v8::Number::New (isolate, matrix[i])).Check ();
    }
    JS::set (context, result, "m", values);

    return result;
}

// elements that aren't numbers keep the identity's value, like sub_1816214A0
glm::mat4 readMat4 (v8::Local<v8::Context> context, v8::Local<v8::Value> value) {
    glm::mat4 result (1.0f);
    float* floats = glm::value_ptr (result);
    const auto values = JS::get (context, value, "m");

    if (!values->IsObject ()) {
	return result;
    }

    for (uint32_t i = 0; i < 16; i++) {
	if (const auto element = JS::get (context, values, i); element->IsNumber ()) {
	    floats[i] = static_cast<float> (element.As<v8::Number> ()->Value ());
	}
    }

    return result;
}

glm::vec3 readVec3 (v8::Local<v8::Context> context, v8::Local<v8::Value> value) {
    glm::vec3 result (0.0f);

    if (!value->IsObject ()) {
	return result;
    }

    const char* names[] = { "x", "y", "z" };
    for (int i = 0; i < 3; i++) {
	if (const auto component = JS::get (context, value, names[i]); component->IsNumber ()) {
	    result[i] = static_cast<float> (component.As<v8::Number> ()->Value ());
	}
    }

    return result;
}

v8::Local<v8::Object> makeVec3 (const ScriptEngine& engine, const glm::vec3& vector) {
    return engine.getAdapters ().vec3->create (vector);
}

// an index-or-name argument (flags 0x208): an int32 is an index, a string a name, and anything else (missing
// included) becomes an empty name, since the dispatcher's default for a mask with 0x200 is an empty string
struct IndexOrName {
    std::optional<int32_t> index;
    std::string name;
};

IndexOrName indexOrNameArgument (v8::Isolate* isolate, v8::Local<v8::Value> value) {
    if (const auto index = int32Argument (value)) {
	return { .index = index };
    }

    IndexOrName result;

    if (value->IsString ()) {
	result.name = JS::toString (isolate, value);
    }

    return result;
}

// thisLayer.play/pause/stop/isPlaying
void playback_call (const v8::FunctionCallbackInfo<v8::Value>& info, int call) {
    auto* object = layerObject (info.Data ());

    if (object == nullptr) {
	return;
    }

    if (call == 2) {
	info.GetReturnValue ().Set (object->isPlaying ());
	return;
    }

    using Playback = ScriptableObject::Playback;

    object->applyPlayback (call == 0 ? Playback::Playing : call == 1 ? Playback::Paused : Playback::Stopped);
}

// only scriptable layers can be handed to scripts, a plain group parent comes back as null
void hierarchy_call (const v8::FunctionCallbackInfo<v8::Value>& info, int children) {
    auto& engine = engineOf (info);
    auto* object = layerObject (info.Data ());
    const auto& scene = engine.getScene ();

    if (object == nullptr) {
	return;
    }

    if (children == 0) {
	info.GetReturnValue ().SetNull ();

	const auto& parentId = object->getObject ().parent;

	if (!parentId.has_value ()) {
	    return;
	}

	const auto* parent = scene.getObject (*parentId);

	if (parent != nullptr && parent->is<ScriptableObject> ()) {
	    info.GetReturnValue ().Set (engine.getAdapters ().object->instantiate (
		const_cast<ScriptableObject&> (*parent->as<ScriptableObject> ())
	    ));
	}

	return;
    }

    const auto context = engine.getContext ();
    const v8::Local<v8::Array> result = v8::Array::New (info.GetIsolate ());
    uint32_t index = 0;

    for (const auto* candidate : scene.childrenOf (object->getObject ().id)) {
	if (!candidate->is<ScriptableObject> ()) {
	    continue;
	}

	result
	    ->Set (
		context, index++,
		engine.getAdapters ().object->instantiate (
		    const_cast<ScriptableObject&> (*candidate->as<ScriptableObject> ())
		)
	    )
	    .Check ();
    }

    info.GetReturnValue ().Set (result);
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

// getEffectCount / getEffect: an index or an effect name (flags 0x208), null for anything else
void effect_call (const v8::FunctionCallbackInfo<v8::Value>& info, int get) {
    auto& engine = engineOf (info);
    auto* object = layerObject (info.Data ());
    const auto* effects = object == nullptr ? nullptr : effectsOf (object->getObject ());

    if (get == 0) {
	info.GetReturnValue ().Set (effects == nullptr ? 0 : static_cast<int32_t> (effects->size ()));
	return;
    }

    info.GetReturnValue ().SetNull ();

    if (effects == nullptr) {
	return;
    }

    const auto [index, name] = indexOrNameArgument (info.GetIsolate (), info[0]);

    if (index.has_value ()) {
	if (*index >= 0 && static_cast<size_t> (*index) < effects->size ()) {
	    info.GetReturnValue ().Set (engine.getAdapters ().object->effect (*(*effects)[*index]));
	}
	return;
    }

    for (const auto& effect : *effects) {
	if (!name.empty () && effect->name == name) {
	    info.GetReturnValue ().Set (engine.getAdapters ().object->effect (*effect));
	    return;
	}
    }
}

// video textures are only ever reached through the player's address, kept alive by the layer's texture for as long as
// the scene exists
void videotexture_call (const v8::FunctionCallbackInfo<v8::Value>& info, int call) {
    auto* player = JS::unwrap<WallpaperEngine::VideoPlayback::MPV::GLPlayer> (info.Data ());
    const auto context = info.GetIsolate ()->GetCurrentContext ();

    switch (call) {
	case 0:
	    if (player->hasEnded ()) {
		player->seek (0.0);
	    }

	    player->clearPaused ();
	    return;
	case 1:
	    player->setPaused ();
	    return;
	case 2:
	    player->setPaused ();
	    player->seek (0.0);
	    return;
	case 3:
	    info.GetReturnValue ().Set (!player->isPaused ());
	    return;
	case 4:
	    info.GetReturnValue ().Set (player->isPaused ());
	    return;
	case 5:
	    info.GetReturnValue ().Set (player->isPaused () && player->getPlaybackPosition () < 0.001);
	    return;
	case 6:
	    info.GetReturnValue ().Set (player->getPlaybackPosition ());
	    return;
	case 7:
	    if (double seconds = 0.0; info.Length () > 0 && info[0]->NumberValue (context).To (&seconds)) {
		player->seek (std::max (seconds, 0.0));
	    }
	    return;
	case 8:
	    if (info.Length () > 0) {
		engineOf (info).addVideoEndedCallback (player, info[0]);
	    }
	    return;
	case 9:
	    info.GetReturnValue ().Set (player->isLooping ());
	    return;
	case 10:
	    if (info.Length () > 0) {
		player->setLoop (info[0]->BooleanValue (info.GetIsolate ()));
	    }
	    return;
	case 11:
	    info.GetReturnValue ().Set (player->getDuration ());
	    return;
	case 12:
	    info.GetReturnValue ().Set (player->getSpeed ());
	    return;
	case 13:
	    if (double rate = 0.0; info.Length () > 0 && info[0]->NumberValue (context).To (&rate) && rate > 0.0) {
		player->setSpeed (rate);
	    }
	    return;
	default:
	    return;
    }
}

v8::Local<v8::Object>
videotexture_instantiate (ScriptEngine& engine, WallpaperEngine::VideoPlayback::MPV::GLPlayer& player) {
    auto* isolate = engine.getIsolate ();
    const auto context = engine.getContext ();
    const v8::Local<v8::Object> handle = v8::Object::New (isolate);
    const auto data = JS::external (isolate, &player);
    const auto method = [&] (const char* name, v8::FunctionCallback callback, int length) {
	JS::set (context, handle, name, JS::function (context, callback, data, length));
    };
    const auto property = [&] (const char* name, v8::FunctionCallback getter, v8::FunctionCallback setter) {
	handle->SetAccessorProperty (
	    JS::name (isolate, name), JS::function (context, getter, data), JS::function (context, setter, data, 1)
	);
    };

    method ("play", JS::bind<videotexture_call, 0>, 0);
    method ("pause", JS::bind<videotexture_call, 1>, 0);
    method ("stop", JS::bind<videotexture_call, 2>, 0);
    method ("isPlaying", JS::bind<videotexture_call, 3>, 0);
    method ("isPaused", JS::bind<videotexture_call, 4>, 0);
    method ("isStopped", JS::bind<videotexture_call, 5>, 0);
    method ("getCurrentTime", JS::bind<videotexture_call, 6>, 0);
    method ("setCurrentTime", JS::bind<videotexture_call, 7>, 1);
    method ("addEndedCallback", JS::bind<videotexture_call, 8>, 1);
    property ("loop", JS::bind<videotexture_call, 9>, JS::bind<videotexture_call, 10>);
    // "duration" gets an ignoring setter (14) since strict mode scripts throw on a property with none
    property ("duration", JS::bind<videotexture_call, 11>, JS::bind<videotexture_call, 14>);
    property ("rate", JS::bind<videotexture_call, 12>, JS::bind<videotexture_call, 13>);

    return handle;
}

enum TextureAnimationCall {
    TexturePlay,
    TexturePause,
    TextureStop,
    TextureIsPlaying,
    TextureSetFrame,
    TextureGetFrame,
    TextureJoin,
    TextureGetAnimation,
};

// pause and a rate other than 1 carry on from where the texture's own clock is
void detachTextureAnimation (CImage& image, CImage::TextureAnimation& animation) {
    const auto [frame, time] = image.sharedTextureFrame ();
    animation.frame = frame;
    animation.time = time;
    animation.detached = true;
}

CImage* imageOf (v8::Local<v8::Value> data) {
    auto* object = layerObject (data);

    return object != nullptr && object->is<CImage> () ? object->as<CImage> () : nullptr;
}

// ITextureAnimation methods (wallpaper64 2.8.42 sub_1402131A0, bodies sub_1401FA330..sub_1401FA490)
void textureanimation_call (const v8::FunctionCallbackInfo<v8::Value>& info, int call) {
    auto* image = imageOf (info.Data ());
    auto* animation = image == nullptr ? nullptr : image->getTextureAnimation ();

    if (animation == nullptr || call == TextureGetAnimation) {
	return;
    }

    switch (call) {
	case TexturePlay:
	    animation->playing = true;
	    break;
	case TexturePause:
	    if (!animation->detached) {
		detachTextureAnimation (*image, *animation);
	    }
	    animation->playing = false;
	    break;
	case TextureStop:
	    animation->frame = 0;
	    animation->time = 0.0f;
	    animation->detached = true;
	    animation->playing = false;
	    break;
	case TextureIsPlaying:
	    info.GetReturnValue ().Set (!animation->detached || animation->playing);
	    break;
	case TextureSetFrame:
	    // an int argument (flag 8), the dispatcher's 0 otherwise
	    animation->frame = int32Argument (info[0]).value_or (0);
	    animation->time = 0.0f;

	    if (!animation->detached) {
		animation->detached = true;
		animation->playing = true;
	    }
	    break;
	case TextureGetFrame:
	    info.GetReturnValue ().Set (animation->detached ? animation->frame : image->sharedTextureFrame ().first);
	    break;
	case TextureJoin:
	    animation->detached = false;
	    break;
	default:
	    break;
    }
}

enum EffectCall {
    EffectGetMaterialCount,
    EffectGetAnimation,
    EffectExecuteMaterialFunction,
    EffectGetMaterial,
    EffectSetMaterialProperty,
};

// IEffect (sub_1401EFCA0): one entry per pass. getMaterial takes an index or file name (last match, empty matches all),
// setMaterialProperty writes every material with that constant
void effect_method (const v8::FunctionCallbackInfo<v8::Value>& info, int call) {
    auto* effect = JS::unwrap<ImageEffect> (info.Data ());
    auto& engine = engineOf (info);
    auto* isolate = info.GetIsolate ();
    const size_t count = effect->effect != nullptr ? effect->effect->passes.size () : 0;

    if (call == EffectGetMaterialCount) {
	info.GetReturnValue ().Set (static_cast<int32_t> (count));
    } else if (call == EffectExecuteMaterialFunction && effect->effect != nullptr) {
	const std::string name = info[0]->IsString () ? JS::toString (isolate, info[0]) : std::string ();

	engine.getScene ().executeEffectFunction (*effect, name);
    } else if (call == EffectGetMaterial) {
	info.GetReturnValue ().SetNull ();

	// int or name, anything else is an empty name
	if (info[0]->IsInt32 () && !info[0]->IsString ()) {
	    const int index = info[0].As<v8::Int32> ()->Value ();

	    if (index >= 0 && static_cast<size_t> (index) < count) {
		info.GetReturnValue ().Set (engine.getAdapters ().object->material (*effect, index));
	    }
	    return;
	}

	const std::string name = info[0]->IsString () ? JS::toString (isolate, info[0]) : std::string ();
	std::optional<size_t> found;

	for (size_t index = 0; index < count; index++) {
	    const auto& material = effect->effect->passes[index]->material;

	    if (material.has_value () && (name.empty () || (*material)->filename == name)
		&& engine.getScene ().findEffectMaterial (*effect, index) != nullptr) {
		found = index;
	    }
	}

	if (found.has_value ()) {
	    info.GetReturnValue ().Set (engine.getAdapters ().object->material (*effect, *found));
	}
    } else if (call == EffectSetMaterialProperty) {
	// scenescript's dispatcher (sub_1816214A0): int32 is an int, other numbers floats, objects a vec2 of x/y, so a
	// Vec3 never changes a vec3 constant (live WE)
	const std::string name = info[0]->IsString () ? JS::toString (isolate, info[0]) : std::string ();
	const auto context = engine.getContext ();
	const v8::Local<v8::Value> value = info[1];
	std::optional<float> number;
	std::optional<glm::vec2> vector;

	if (value->IsNumber ()) {
	    number = value->IsInt32 () ? static_cast<float> (value.As<v8::Int32> ()->Value ())
				       : static_cast<float> (value.As<v8::Number> ()->Value ());
	} else if (value->IsObject ()) {
	    glm::vec2 read (0.0f);
	    const auto object = value.As<v8::Object> ();

	    for (int axis = 0; axis < 2; axis++) {
		const auto component = JS::get (context, object, axis == 0 ? "x" : "y");

		if (component->IsNumber ()) {
		    read[axis] = static_cast<float> (component.As<v8::Number> ()->Value ());
		}
	    }

	    vector = read;
	}

	for (size_t index = 0; index < count && (number.has_value () || vector.has_value ()); index++) {
	    auto* pass = engine.getScene ().findEffectMaterial (*effect, index);
	    const int size = pass != nullptr ? pass->getMaterialConstantSize (name) : 0;

	    auto* target = size == 1 || size == 2 ? pass->getScriptConstant (name) : nullptr;

	    // rad2deg constants take degrees
	    if (target != nullptr && size == 1 && number.has_value ()) {
		const float factor = pass->isMaterialConstantInDegrees (name) ? 0.017453292f : 1.0f;
		target->update (*number * factor, DynamicValue::UpdateSource::Script);
	    } else if (target != nullptr && size == 2 && vector.has_value ()) {
		target->update (*vector, DynamicValue::UpdateSource::Script);
	    }
	}
    }
}

enum BlendShapeCall { GetBlendShapeIndex, GetBlendShapeWeight, SetBlendShapeWeight };

// sub_140210400 / sub_1402104B0 / sub_1402105C0, non-puppets read -1 / 0 and ignore writes
void blend_shape_call (const v8::FunctionCallbackInfo<v8::Value>& info, int call) {
    auto* object = layerObject (info.Data ());
    auto* image = object != nullptr && object->is<CImage> () ? object->as<CImage> () : nullptr;

    if (call == GetBlendShapeIndex) {
	const std::string name = info[0]->IsString () ? JS::toString (info.GetIsolate (), info[0]) : std::string ();
	info.GetReturnValue ().Set (image != nullptr ? image->getBlendShapeIndex (name) : -1);
	return;
    }

    const auto [index, name] = indexOrNameArgument (info.GetIsolate (), info[0]);
    const int target = image == nullptr ? -1 : index.has_value () ? *index : image->getBlendShapeIndex (name);

    if (call == GetBlendShapeWeight) {
	info.GetReturnValue ().Set (
	    static_cast<double> (image != nullptr ? image->getBlendShapeWeight (target) : 0.0f)
	);
    } else if (image != nullptr) {
	const float weight = info[1]->IsNumber () ? static_cast<float> (info[1].As<v8::Number> ()->Value ()) : 0.0f;
	image->setBlendShapeWeight (target, weight);
    }
}

// data is the constants' group (registerEffectConstants)
void material_get_animation (const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto& engine = engineOf (info);
    const std::string group = JS::toString (info.GetIsolate (), info.Data ());
    std::string name = info[0]->IsString () ? JS::toString (info.GetIsolate (), info[0]) : std::string ();

    if (name.empty () && engine.getRunningModule () != nullptr) {
	name = engine.getRunningModule ()->propertyName;
    }

    v8::Local<v8::Value> animation;

    if (engine.findAnimation (name, group).ToLocal (&animation)) {
	info.GetReturnValue ().Set (animation);
    }
}

// sub_1401577E0, each a byte with its own name table
struct MaterialEnum {
    const char* name;
    size_t byte;
    std::vector<std::pair<const char*, uint8_t>> values;
};

const std::array<MaterialEnum, 5> MaterialEnums = { {
    { "depthwrite", 3, { { "disabled", 1 }, { "enabled", 0 } } },
    { "depthtest", 2, { { "disabled", 1 }, { "enabled", 0 } } },
    { "cullmode", 4, { { "normal", 0 }, { "nocull", 1 } } },
    { "alphawriting", 1, { { "default", 0 }, { "disabled", 2 }, { "enabled", 1 } } },
    { "blending", 0, { { "normal", 0 }, { "translucent", 1 }, { "additive", 2 }, { "alphatocoverage", 3 } } },
} };

// what the first pass sets on load (sub_140154480), missing keys stay 0
std::array<uint8_t, 5> materialState (const WallpaperEngine::Data::Model::MaterialPass& pass) {
    std::array<uint8_t, 5> state {};

    state[0] = pass.blending == BlendingMode_Translucent ? 1
	: pass.blending == BlendingMode_Additive         ? 2
	: pass.blending == BlendingMode_AlphaToCoverage  ? 3
							 : 0;
    state[1] = pass.alphawriting == "enabled" ? 1 : pass.alphawriting == "disabled" ? 2 : 0;
    state[2] = pass.depthtest == DepthtestMode_Disabled ? 1 : 0;
    state[3] = pass.depthwrite == DepthwriteMode_Disabled ? 1 : 0;
    state[4] = pass.cullmode == CullingMode_Disable ? 1 : 0;
    return state;
}

void applyMaterialState (WallpaperEngine::Render::Objects::Effects::CPass& pass, const std::array<uint8_t, 5>& state) {
    static constexpr BlendingMode blending[]
	= { BlendingMode_Normal, BlendingMode_Translucent, BlendingMode_Additive, BlendingMode_AlphaToCoverage };

    pass.setScriptMaterialState (
	blending[std::min<uint8_t> (state[0], 3)],
	std::make_pair (
	    state[2] == 1 ? DepthtestMode_Disabled : DepthtestMode_Enabled,
	    state[3] == 1 ? DepthwriteMode_Disabled : DepthwriteMode_Enabled
	),
	state[4] == 1 ? CullingMode_Disable : CullingMode_Normal
    );
}

// emitParticles (an int argument, flag 8, the dispatcher's 0 otherwise) and getAnimation on IParticleSystemInstance
void particle_call (const v8::FunctionCallbackInfo<v8::Value>& info, int emit) {
    auto* object = layerObject (info.Data ());

    if (emit != 0 && object != nullptr && object->is<CParticle> ()) {
	object->as<CParticle> ()->emitParticles (int32Argument (info[0]).value_or (0));
    }
}

enum AnimationCall {
    AnimationPlay,
    AnimationIsPlaying,
    AnimationStop,
    AnimationPause,
    AnimationSetFrame,
    AnimationGetFrame,
    AnimationGetAnimation,
};

// data is [animation system id, clock id]
AnimationClock* animationClock (v8::Local<v8::Context> context, v8::Local<v8::Value> data) {
    const int systemId = JS::get (context, data, 0u)->Int32Value (context).FromMaybe (0);
    const int clockId = JS::get (context, data, 1u)->Int32Value (context).FromMaybe (0);
    auto* system = AnimationSystem::find (systemId);

    return system == nullptr ? nullptr : system->clock (clockId);
}

// IAnimation methods (wallpaper64 2.8.42 sub_140177F70, bodies sub_1401707F0..sub_1401708A0). Frames are the
// timeline's time over its frame time, fractions included
void animation_call (const v8::FunctionCallbackInfo<v8::Value>& info, int call) {
    auto* clock = animationClock (info.GetIsolate ()->GetCurrentContext (), info.Data ());

    if (clock == nullptr || call == AnimationGetAnimation) {
	return;
    }

    switch (call) {
	case AnimationPlay:
	    clock->play ();
	    break;
	case AnimationPause:
	    clock->pause ();
	    break;
	case AnimationStop:
	    clock->stop ();
	    break;
	case AnimationIsPlaying:
	    info.GetReturnValue ().Set (clock->isPlaying ());
	    break;
	case AnimationGetFrame:
	    info.GetReturnValue ().Set (static_cast<double> (clock->getFrame ()));
	    break;
	case AnimationSetFrame:
	    clock->setFrame (info[0]->IsNumber () ? static_cast<float> (info[0].As<v8::Number> ()->Value ()) : 0.0f);
	    break;
	default:
	    break;
    }
}

enum AnimationLayerCall {
    LayerPlay,
    LayerPause,
    LayerStop,
    LayerIsPlaying,
    LayerGetFrame,
    LayerSetFrame,
    LayerAddEndedCallback,
    LayerGetFps,
    LayerGetFrameCount,
    LayerGetDuration,
    LayerGetName,
    LayerGetVisible,
    LayerSetVisible,
    LayerGetRate,
    LayerSetRate,
    LayerGetBlend,
    LayerSetBlend,
};

constexpr const char* AnimationLayerSerialKey = "__animationLayerSerial";
constexpr uint32_t LayerFrameSet = 0x2000000;
constexpr uint32_t LayerPaused = 0x20000000;
constexpr uint32_t LayerStopped = 0x40000000;

// the skeleton whose animation layers scripts reach: puppet images and models
WallpaperEngine::Render::Objects::PuppetRig* animationRig (ScriptableObject& object) {
    if (object.is<CImage> ()) {
	return &object.as<CImage> ()->getRig ();
    }

    if (object.is<CMesh> ()) {
	return &object.as<CMesh> ()->getRig ();
    }

    return nullptr;
}

// data is [the layer, the animation layer's serial]; the methods are sub_14026C420..sub_14026C4F0, the read only
// numbers sub_14026C3E0/C400/C410 (fps, frameCount, duration)
void animation_layer_call (const v8::FunctionCallbackInfo<v8::Value>& info, int call) {
    auto& engine = engineOf (info);
    const auto context = engine.getContext ();
    auto* object = layerObject (JS::get (context, info.Data (), 0u));
    const auto serial = static_cast<size_t> (JS::get (context, info.Data (), 1u)->IntegerValue (context).FromMaybe (0));
    auto* rig = object == nullptr ? nullptr : animationRig (*object);
    auto* clock = rig == nullptr ? nullptr : rig->findLayer (serial);

    // destroyed, or a scene layer without a clip (WE never creates one)
    if (clock == nullptr || clock->clip.fps <= 0.0f) {
	if (call == LayerIsPlaying) {
	    info.GetReturnValue ().Set (false);
	}
	return;
    }

    const auto& layer = *clock->layer;

    switch (call) {
	case LayerGetName:
	    info.GetReturnValue ().Set (JS::string (info.GetIsolate (), layer.name));
	    return;
	case LayerGetVisible:
	    info.GetReturnValue ().Set (engine.propertyToJs (*layer.visible->value, ""));
	    return;
	case LayerSetVisible:
	    engine.assignPropertyJsValue (info[0], *layer.visible->value, "");
	    return;
	case LayerGetRate:
	    info.GetReturnValue ().Set (engine.propertyToJs (*layer.rate->value, ""));
	    return;
	case LayerSetRate:
	    engine.assignPropertyJsValue (info[0], *layer.rate->value, "");
	    return;
	case LayerGetBlend:
	    info.GetReturnValue ().Set (engine.propertyToJs (*layer.blend->value, ""));
	    return;
	case LayerSetBlend:
	    engine.assignPropertyJsValue (info[0], *layer.blend->value, "");
	    return;
	default:
	    break;
    }

    const float frameTime = 1.0f / clock->clip.fps;

    switch (call) {
	case LayerPlay:
	    if ((clock->flags & LayerStopped) != 0) {
		clock->time = 0.0f;
	    }
	    clock->flags &= ~(LayerPaused | LayerStopped);
	    return;
	case LayerPause:
	    clock->flags |= LayerPaused;
	    return;
	case LayerStop:
	    // sub_14026C460 leaves it paused at 0, the stopped and backwards bits cleared
	    clock->time = 0.0f;
	    clock->flags = (clock->flags | LayerPaused) & 0x3FFFFFFF;
	    return;
	case LayerIsPlaying:
	    info.GetReturnValue ().Set ((clock->flags & (LayerPaused | LayerStopped)) == 0);
	    return;
	case LayerGetFrame:
	    info.GetReturnValue ().Set (static_cast<double> (clock->time / frameTime));
	    return;
	case LayerSetFrame:
	    if (info[0]->IsNumber ()) {
		clock->time = frameTime * static_cast<float> (info[0].As<v8::Number> ()->Value ());
		clock->flags |= LayerFrameSet;
	    }
	    return;
	case LayerAddEndedCallback:
	    if (info.Length () > 0) {
		engine.addAnimationLayerEndedCallback (*object, serial, info[0]);
	    }
	    return;
	case LayerGetFps:
	    info.GetReturnValue ().Set (static_cast<double> (1.0f / frameTime));
	    return;
	case LayerGetFrameCount:
	    info.GetReturnValue ().Set (static_cast<int32_t> (clock->clip.frameCount));
	    return;
	case LayerGetDuration:
	    info.GetReturnValue ().Set (static_cast<double> (static_cast<float> (clock->clip.frameCount) * frameTime));
	    return;
	default:
	    return;
    }
}
// getAnimation (sub_18162BBE0) on an animation layer's own properties, by animation or property name
void animation_layer_get_animation (const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto& engine = engineOf (info);
    const auto context = engine.getContext ();
    auto* object = layerObject (JS::get (context, info.Data (), 0u));
    const auto serial = static_cast<size_t> (JS::get (context, info.Data (), 1u)->IntegerValue (context).FromMaybe (0));
    std::string name;

    info.GetReturnValue ().SetNull ();

    if (object == nullptr) {
	return;
    }

    if (info.Length () > 0 && info[0]->IsString ()) {
	name = JS::toString (info.GetIsolate (), info[0]);
    } else if (engine.getRunningModule () != nullptr) {
	name = engine.getRunningModule ()->propertyName;
    }

    v8::Local<v8::Value> animation;

    if (engine
	    .findAnimation (name, WallpaperEngine::Scripting::Adapters::animationLayerGroup (object->getId (), serial))
	    .ToLocal (&animation)) {
	info.GetReturnValue ().Set (animation);
    }
}
} // namespace

std::string WallpaperEngine::Scripting::Adapters::animationLayerGroup (int objectId, size_t serial) {
    return "animationlayer" + std::to_string (objectId) + "[" + std::to_string (serial) + "]";
}

v8::Local<v8::Value> WallpaperEngine::Scripting::Adapters::makeAnimationLayerHandle (
    ScriptEngine& engine, ScriptableObject& object, size_t serial
) {
    auto* isolate = engine.getIsolate ();
    const auto context = engine.getContext ();
    const v8::Local<v8::Object> handle = v8::Object::New (isolate);
    const auto data = JS::data (
	isolate,
	{ JS::external (isolate, &engine.getAdapters ().object->layerOf (object)),
	  v8::Number::New (isolate, static_cast<double> (serial)) }
    );

    const auto method = [&] (const char* name, v8::FunctionCallback callback, int length) {
	JS::set (context, handle, name, JS::function (context, callback, data, length));
    };
    const auto accessor = [&] (const char* name, v8::FunctionCallback getter, v8::FunctionCallback setter) {
	handle->SetAccessorProperty (
	    JS::name (isolate, name), JS::function (context, getter, data),
	    setter == nullptr ? v8::Local<v8::Function> () : JS::function (context, setter, data, 1)
	);
    };

    method ("play", JS::bind<animation_layer_call, LayerPlay>, 0);
    method ("pause", JS::bind<animation_layer_call, LayerPause>, 0);
    method ("stop", JS::bind<animation_layer_call, LayerStop>, 0);
    method ("isPlaying", JS::bind<animation_layer_call, LayerIsPlaying>, 0);
    method ("getFrame", JS::bind<animation_layer_call, LayerGetFrame>, 0);
    method ("setFrame", JS::bind<animation_layer_call, LayerSetFrame>, 1);
    method ("addEndedCallback", JS::bind<animation_layer_call, LayerAddEndedCallback>, 1);
    accessor ("fps", JS::bind<animation_layer_call, LayerGetFps>, nullptr);
    accessor ("frameCount", JS::bind<animation_layer_call, LayerGetFrameCount>, nullptr);
    accessor ("duration", JS::bind<animation_layer_call, LayerGetDuration>, nullptr);
    accessor ("name", JS::bind<animation_layer_call, LayerGetName>, nullptr);
    accessor (
	"visible", JS::bind<animation_layer_call, LayerGetVisible>, JS::bind<animation_layer_call, LayerSetVisible>
    );
    accessor ("rate", JS::bind<animation_layer_call, LayerGetRate>, JS::bind<animation_layer_call, LayerSetRate>);
    accessor ("blend", JS::bind<animation_layer_call, LayerGetBlend>, JS::bind<animation_layer_call, LayerSetBlend>);
    method ("getAnimation", animation_layer_get_animation, 0);

    // lets destroyAnimationLayer() take the object back
    handle
	->DefineOwnProperty (
	    context, JS::name (isolate, AnimationLayerSerialKey),
	    v8::Number::New (isolate, static_cast<double> (serial)),
	    static_cast<v8::PropertyAttribute> (v8::ReadOnly | v8::DontEnum | v8::DontDelete)
	)
	.Check ();

    return handle;
}

namespace {
enum ImageAnimationLayerCall {
    GetAnimationLayerCount,
    GetAnimationLayer,
    CreateAnimationLayer,
    PlaySingleAnimation,
    DestroyAnimationLayer,
};

// scenescript64 hands objects over as JSON text; a string in the config slot is parsed as JSON text too
WallpaperEngine::Data::JSON::JSON
animationLayerArgument (v8::Isolate* isolate, v8::Local<v8::Value> value, bool parseStrings) {
    using WallpaperEngine::Data::JSON::JSON;

    if (value->IsString () && !parseStrings) {
	return JSON (JS::toString (isolate, value));
    }

    std::string text;

    if (value->IsString ()) {
	text = JS::toString (isolate, value);
    } else if (value->IsObject ()) {
	const v8::TryCatch tryCatch (isolate);
	v8::Local<v8::String> json;

	if (v8::JSON::Stringify (isolate->GetCurrentContext (), value).ToLocal (&json)) {
	    text = JS::toString (isolate, json);
	}
    }

    return JSON::parse (text, nullptr, false);
}

// IImageLayer animation layer calls, wallpaper64 2.8.42 sub_14020E910 (get), sub_14020E9F0 (count), sub_14020EA30
// (create), sub_14020EF40 (playSingle = create + removed once it ends), sub_14020EF80 (destroy)
void layer_animation_layer_call (const v8::FunctionCallbackInfo<v8::Value>& info, int call) {
    auto* isolate = info.GetIsolate ();
    auto& engine = engineOf (info);
    const auto context = engine.getContext ();
    auto* object = layerObject (info.Data ());
    auto* rig = object == nullptr ? nullptr : animationRig (*object);
    const auto handleFor = [&] (std::optional<size_t> serial) -> v8::Local<v8::Value> {
	return serial.has_value () ? makeAnimationLayerHandle (engine, *object, *serial)
				   : v8::Null (isolate).As<v8::Value> ();
    };

    switch (call) {
	case GetAnimationLayerCount:
	    info.GetReturnValue ().Set (rig == nullptr ? 0 : static_cast<int32_t> (rig->getLayerCount ()));
	    return;
	case GetAnimationLayer:
	    info.GetReturnValue ().SetNull ();

	    if (rig == nullptr || info.Length () < 1) {
		return;
	    }
	    if (info[0]->IsNumber ()) {
		info.GetReturnValue ().Set (
		    handleFor (rig->getLayerAt (info[0]->IntegerValue (context).FromMaybe (-1)))
		);
	    } else if (info[0]->IsString ()) {
		info.GetReturnValue ().Set (handleFor (rig->findLayerByName (JS::toString (isolate, info[0]))));
	    }
	    return;
	case CreateAnimationLayer:
	case PlaySingleAnimation:
	    {
		info.GetReturnValue ().SetNull ();

		if (rig == nullptr || info.Length () < 1) {
		    return;
		}

		const auto animation = animationLayerArgument (isolate, info[0], false);
		const auto config = info.Length () > 1 ? animationLayerArgument (isolate, info[1], true)
						       : WallpaperEngine::Data::JSON::JSON ();

		const auto serial = rig->createLayer (
		    animation, config, call == PlaySingleAnimation, object->getScene ().getScene ().project
		);

		// through the scene loader's property handler (sub_1401730D0), scripts included
		if (const auto* layer = serial.has_value () ? rig->findLayer (*serial) : nullptr; layer != nullptr) {
		    object->registerAnimationLayerProperties (*serial, *layer->layer);
		}

		info.GetReturnValue ().Set (handleFor (serial));
		return;
	    }
	case DestroyAnimationLayer:
	    {
		info.GetReturnValue ().Set (false);

		if (rig == nullptr || info.Length () < 1) {
		    return;
		}
		if (info[0]->IsNumber ()) {
		    const auto serial = rig->getLayerAt (info[0]->IntegerValue (context).FromMaybe (-1));
		    info.GetReturnValue ().Set (serial.has_value () && rig->destroyLayer (*serial));
		} else if (info[0]->IsString ()) {
		    info.GetReturnValue ().Set (rig->destroyLayersByName (JS::toString (isolate, info[0])));
		} else if (info[0]->IsObject ()) {
		    const auto serial = JS::get (context, info[0], AnimationLayerSerialKey);
		    info.GetReturnValue ().Set (
			serial->IsNumber ()
			&& rig->destroyLayer (static_cast<size_t> (serial->IntegerValue (context).FromMaybe (0)))
		    );
		}
		return;
	    }
	default:
	    return;
    }
}

// scripts call this on image layers whose material is a video (mp4 texture), everything else gets null
void video_texture_call (const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto* image = imageOf (info.Data ());
    const auto texture = image == nullptr ? nullptr : image->getTexture ();

    info.GetReturnValue ().SetNull ();

    if (texture == nullptr || texture->getPlayer () == nullptr) {
	return;
    }

    info.GetReturnValue ().Set (videotexture_instantiate (engineOf (info), *texture->getPlayer ()));
}

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

// thisLayer bone calls (wallpaper64 2.8.42 image methods, sub_140211070). A bone is an index or a name, see
// indexOrNameArgument. What a call returns when the method leaves its result alone is the dispatcher's
// initial value for the return type: identity Mat4, zero Vec3, 0
void bone_call (const v8::FunctionCallbackInfo<v8::Value>& info, int call) {
    auto& engine = engineOf (info);
    const auto context = engine.getContext ();
    auto* object = layerObject (info.Data ());

    const auto unset = [&] () {
	switch (call) {
	    case GetBoneCount:
		info.GetReturnValue ().Set (0);
		return;
	    // both index lookups write -1 before looking at the layer
	    case GetBoneIndex:
	    case GetBoneParentIndex:
		info.GetReturnValue ().Set (-1);
		return;
	    case GetBoneTransform:
	    case GetLocalBoneTransform:
		info.GetReturnValue ().Set (makeMat4 (engine, glm::mat4 (1.0f)));
		return;
	    case GetLocalBoneAngles:
	    case GetLocalBoneOrigin:
		info.GetReturnValue ().Set (makeVec3 (engine, glm::vec3 (0.0f)));
		return;
	    default:
		return;
	}
    };

    if (object == nullptr || !object->is<CImage> ()) {
	unset ();
	return;
    }

    auto* image = object->as<CImage> ();
    const auto& bones = image->getPuppetBones ();
    const int count = static_cast<int> (bones.size ());

    if (count == 0) {
	unset ();
	return;
    }

    if (call == GetBoneCount) {
	info.GetReturnValue ().Set (count);
	return;
    }

    const auto [index, name] = indexOrNameArgument (info.GetIsolate (), info[0]);

    if (call == GetBoneIndex) {
	// names only (flag 0x200): an index becomes a string the lookup won't find, an empty name finds nothing
	if (index.has_value () || name.empty ()) {
	    unset ();
	    return;
	}

	info.GetReturnValue ().Set (image->findPuppetBone (name));
	return;
    }

    if (call == GetBoneParentIndex) {
	// by name the first bone of that name that has a parent (sub_140210860)
	for (int bone = 0; bone < count; bone++) {
	    const bool matches = index.has_value () ? bone == *index : !name.empty () && bones[bone].name == name;

	    if (matches && bones[bone].parent != -1) {
		info.GetReturnValue ().Set (bones[bone].parent);
		return;
	    }
	}

	unset ();
	return;
    }

    // an empty name is the first bone here (sub_140210990, sub_140210E10), the other calls need a match
    int bone = -1;
    if (index.has_value ()) {
	bone = *index;
    } else if (call == ApplyBonePhysicsImpulse || call == ResetBonePhysicsSimulation) {
	bone = name.empty () ? 0 : image->findPuppetBone (name);
    } else if (!name.empty ()) {
	bone = image->findPuppetBone (name);
    }

    if (bone < 0 || bone >= count) {
	unset ();
	return;
    }

    if (call == ApplyBonePhysicsImpulse) {
	image->applyPuppetBonePhysicsImpulse (bone, readVec3 (context, info[1]), readVec3 (context, info[2]));
	return;
    }

    if (call == ResetBonePhysicsSimulation) {
	image->resetPuppetBonePhysics (bone);
	return;
    }

    // the matrices exist from the layer's first update on
    if (!image->hasPuppetPose ()) {
	unset ();
	return;
    }

    const auto value = info[1];

    switch (call) {
	case GetBoneTransform:
	    info.GetReturnValue ().Set (makeMat4 (engine, image->getPuppetBoneTransform (bone)));
	    break;
	case SetBoneTransform:
	    image->setPuppetBoneTransform (bone, readMat4 (context, value));
	    break;
	case GetLocalBoneTransform:
	    info.GetReturnValue ().Set (makeMat4 (engine, image->getPuppetLocalBoneTransform (bone)));
	    break;
	case SetLocalBoneTransform:
	    image->setPuppetLocalBoneTransform (bone, readMat4 (context, value));
	    break;
	case GetLocalBoneAngles:
	    info.GetReturnValue ().Set (makeVec3 (engine, localBoneAngles (image->getPuppetLocalBoneTransform (bone))));
	    break;
	case SetLocalBoneAngles:
	    {
		glm::mat4 local = image->getPuppetLocalBoneTransform (bone);
		setLocalBoneAngles (local, readVec3 (context, value));
		image->setPuppetLocalBoneTransform (bone, local);
		break;
	    }
	case GetLocalBoneOrigin:
	    info.GetReturnValue ().Set (makeVec3 (engine, glm::vec3 (image->getPuppetLocalBoneTransform (bone)[3])));
	    break;
	case SetLocalBoneOrigin:
	    {
		// sub_140210250: floats 12..14, the rest stays
		glm::mat4 local = image->getPuppetLocalBoneTransform (bone);
		local[3] = glm::vec4 (readVec3 (context, value), local[3].w);
		image->setPuppetLocalBoneTransform (bone, local);
		break;
	    }
	default:
	    break;
    }
}

// IObject.getAnimation (scenescript64 sub_18162BBE0): by name, or without a string the property whose script runs,
// among this layer's animated properties (engine slot 15). undefined when there is none
void get_animation_call (const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto& engine = engineOf (info);
    auto* object = layerObject (info.Data ());
    std::string name;

    if (object == nullptr) {
	return;
    }

    if (info[0]->IsString ()) {
	name = JS::toString (info.GetIsolate (), info[0]);
    }

    if (name.empty () && engine.getRunningModule () != nullptr) {
	name = engine.getRunningModule ()->propertyName;
    }

    v8::Local<v8::Value> animation;

    if (engine.findAnimation (name, object->getId ()).ToLocal (&animation)) {
	info.GetReturnValue ().Set (animation);
    }
}

// image layers with an animated texture get their ITextureAnimation, everything else null
void texture_animation_call (const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto* layer = JS::unwrap<Layer> (info.Data ());

    info.GetReturnValue ().SetNull ();

    if (layer != nullptr && layer->object != nullptr && layer->object->is<CImage> ()) {
	info.GetReturnValue ().Set (engineOf (info).getAdapters ().object->textureAnimation (*layer));
    }
}

enum TransformCall {
    GetTransformMatrix,
    RotateObjectSpace,
    LookAt,
    LookAtYaw,
    GetAttachmentIndex,
    GetAttachmentMatrix,
    GetAttachmentOrigin,
    GetAttachmentAngles,
};

// vtable slot 16 of WE's objects: images move by their alignment, text by its anchor, the rest is the base world
glm::mat4 layerWorld (const ScriptableObject& object) {
    using namespace WallpaperEngine::Render::Objects;

    if (object.is<CImage> ()) {
	return object.as<CImage> ()->worldMatrix ();
    }

    if (object.is<CText> ()) {
	return object.as<CText> ()->worldMatrix ();
    }

    return object.getScene ().objectWorldMatrix (object.getObject ());
}

// renderable +752. Uses the data model since scripted properties can update from the ScriptableObject constructor
std::optional<glm::vec2> renderableSize (const ScriptableObject& object) {
    const auto& model = object.getObject ();

    if (model.is<Image> ()) {
	return model.as<Image> ()->size;
    }

    if (const auto* text = dynamic_cast<const CText*> (&object); text != nullptr) {
	return text->measuredSize ();
    }

    if (model.is<Text> ()) {
	return glm::vec2 (2.0f);
    }

    return std::nullopt;
}

// puppet images and models have attachment points (slots 14/15), every other object has none
const WallpaperEngine::Render::Objects::PuppetRig* layerRig (const ScriptableObject& object) {
    if (object.is<CImage> ()) {
	return &object.as<CImage> ()->getRig ();
    }

    if (object.is<CMesh> ()) {
	return &object.as<CMesh> ()->getRig ();
    }

    return nullptr;
}

// the object's rotation matrix (+332) as sub_1401DD630 builds it from the angles, WE's rows in m[row * 3 + column]
std::array<float, 9> rotationRows (const glm::vec3& angles) {
    glm::mat4 rotation (1.0f);
    setLocalBoneAngles (rotation, angles);
    const float* m = glm::value_ptr (rotation);

    return { m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10] };
}

// back to angles the way sub_1401DF620 / sub_1401DFC00 do it, the same atan2s as getLocalBoneAngles
glm::vec3 rowsToAngles (const std::array<float, 9>& m) {
    glm::mat4 rotation (1.0f);
    float* floats = glm::value_ptr (rotation);

    for (int row = 0; row < 3; row++) {
	for (int column = 0; column < 3; column++) {
	    floats[row * 4 + column] = m[row * 3 + column];
	}
    }

    return localBoneAngles (rotation);
}

// lookAt/lookAtYaw (sub_1401DFC00/sub_1401DFE30): sub_14019D920 is a right handed view matrix, the angles are read
// off its transpose. Anything but a camera turns its back to the target, the eye is the object's origin
std::optional<glm::vec3> lookAtAngles (
    const ScriptableObject& object, const glm::vec3& origin, const glm::vec3& direction, const glm::vec3& up
) {
    if (glm::dot (direction, direction) <= 1.1920929e-7f) {
	return std::nullopt;
    }

    const bool camera = object.is<WallpaperEngine::Render::Objects::CCamera> ();
    const glm::vec3 target = camera ? origin + direction : origin - direction;

    return localBoneAngles (glm::transpose (glm::lookAtRH (origin, target, up)));
}

// sub_140196AC0: a layer object, list index or name; an unknown name is read as an id
const CObject* layerArgument (ScriptEngine& engine, v8::Local<v8::Value> value) {
    auto& scene = engine.getScene ();
    const CObject* layer = engine.getAdapters ().object->getObject (value);

    if (value->IsInt32 ()) {
	const auto layers = scene.getLayers ();
	const int index = value.As<v8::Int32> ()->Value ();

	if (index >= 0 && index < static_cast<int> (layers.size ())) {
	    layer = layers[index];
	}
    } else if (value->IsString ()) {
	const std::string name = JS::toString (engine.getIsolate (), value);

	for (const auto* candidate : scene.getObjectsByRenderOrder ()) {
	    if (candidate->getObject ().name == name && candidate->is<ScriptableObject> ()) {
		layer = candidate;
		break;
	    }
	}

	if (layer == nullptr) {
	    const auto* byId = scene.getObject (static_cast<int> (std::strtol (name.c_str (), nullptr, 10)));
	    layer = byId != nullptr && byId->is<ScriptableObject> () ? byId : nullptr;
	}
    }

    return layer;
}

void set_parent_call (const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto& engine = engineOf (info);
    auto* object = layerObject (info.Data ());

    if (object == nullptr) {
	return;
    }

    auto* isolate = info.GetIsolate ();
    auto& scene = engine.getScene ();
    const CObject* parent = layerArgument (engine, info[0]);

    const bool adjust = (info[1]->IsBoolean () && info[1]->IsTrue ()) || (info[2]->IsBoolean () && info[2]->IsTrue ());
    int attachment = -1;

    if (parent != nullptr && info[1]->IsInt32 ()) {
	attachment = info[1].As<v8::Int32> ()->Value ();
    } else if (parent != nullptr && info[1]->IsString ()) {
	const auto* scriptable = dynamic_cast<const ScriptableObject*> (parent);
	const auto* rig = scriptable != nullptr ? layerRig (*scriptable) : nullptr;

	attachment = rig != nullptr ? rig->findAttachment (JS::toString (isolate, info[1])) : -1;
    }

    // WE throws a plain Error
    if (!scene.setObjectParent (*object, parent, attachment, adjust)) {
	isolate->ThrowException (v8::Exception::Error (JS::string (isolate, "Invalid parent configuration.")));
    }
}

// thisLayer transform and attachment calls (wallpaper64 2.8.42 object methods, sub_1401E0530). Matrices follow
// the bone calls: glm column-vector, the same 16 floats as WE's row-vector ones
void transform_call (const v8::FunctionCallbackInfo<v8::Value>& info, int call) {
    auto& engine = engineOf (info);
    const auto context = engine.getContext ();
    auto* object = layerObject (info.Data ());

    if (object == nullptr) {
	return;
    }

    auto* angles = object->tryGetProperty ("angles");
    const auto setAngles = [angles] (const glm::vec3& value) {
	if (angles != nullptr) {
	    angles->update (value, DynamicValue::UpdateSource::Script);
	}
    };

    switch (call) {
	case GetTransformMatrix:
	    info.GetReturnValue ().Set (makeMat4 (engine, layerWorld (*object)));
	    return;
	case RotateObjectSpace:
	    {
		// sub_1401DF620: z, y, then x, each an axis-angle rotation in front of the rotation rows
		const glm::vec3 rotation = readVec3 (context, info[0]);
		auto m = rotationRows (angles != nullptr ? angles->getVec3 () : glm::vec3 (0.0f));

		for (const int axis : { 2, 1, 0 }) {
		    glm::vec3 a (0.0f);
		    a[axis] = 1.0f;
		    const float c = std::cos (rotation[axis]);
		    const float sn = std::sin (rotation[axis]);
		    const float t = 1.0f - c;
		    const float r[9] = {
			t * a.x * a.x + c,        t * a.x * a.y + a.z * sn, t * a.x * a.z - a.y * sn,
			t * a.x * a.y - a.z * sn, t * a.y * a.y + c,        t * a.y * a.z + a.x * sn,
			t * a.x * a.z + a.y * sn, t * a.y * a.z - a.x * sn, t * a.z * a.z + c,
		    };
		    std::array<float, 9> rotated {};

		    for (int row = 0; row < 3; row++) {
			for (int column = 0; column < 3; column++) {
			    rotated[row * 3 + column] = r[row * 3] * m[column] + r[row * 3 + 1] * m[3 + column]
				+ r[row * 3 + 2] * m[6 + column];
			}
		    }

		    m = rotated;
		}

		setAngles (rowsToAngles (m));
		return;
	    }
	case LookAt:
	case LookAtYaw:
	    {
		// a missing up (flag below 0) is +y, the dispatcher's zero vector for a missing center
		const glm::vec3 origin = object->getObject ().origin->value->getVec3 ();
		const glm::vec3 up = info[1]->IsObject () ? readVec3 (context, info[1]) : glm::vec3 (0.0f, 1.0f, 0.0f);
		glm::vec3 direction = readVec3 (context, info[0]) - origin;

		// the yaw version drops the part along up, which it doesn't normalize
		if (call == LookAtYaw) {
		    direction -= up * glm::dot (up, direction);
		}

		if (const auto result = lookAtAngles (*object, origin, direction, up)) {
		    setAngles (*result);
		}

		return;
	    }
	case GetAttachmentIndex:
	    {
		// names only (flag 0x200), the base slot 14 is -1
		const auto* rig = layerRig (*object);
		const std::string name
		    = info[0]->IsString () ? JS::toString (info.GetIsolate (), info[0]) : std::string ();

		info.GetReturnValue ().Set (rig != nullptr ? rig->findAttachment (name) : -1);
		return;
	    }
	default:
	    break;
    }

    // the attachment point (identity when there is none) in front of the world (sub_1401E01F0, E02C0, E0390)
    const auto [index, name] = indexOrNameArgument (info.GetIsolate (), info[0]);
    glm::mat4 attachment (1.0f);

    if (const auto* rig = layerRig (*object)) {
	if (const auto point = rig->attachmentMatrix (index.has_value () ? *index : rig->findAttachment (name))) {
	    attachment = *point;
	}
    }

    const glm::mat4 matrix = layerWorld (*object) * attachment;

    switch (call) {
	case GetAttachmentMatrix:
	    info.GetReturnValue ().Set (makeMat4 (engine, matrix));
	    return;
	case GetAttachmentOrigin:
	    info.GetReturnValue ().Set (makeVec3 (engine, glm::vec3 (matrix[3])));
	    return;
	case GetAttachmentAngles:
	    info.GetReturnValue ().Set (makeVec3 (engine, localBoneAngles (matrix) * 57.29578f));
	    return;
	default:
	    return;
    }
}

// in WE's y up world, with the perspective layer camera or camera parallax (sub_14018AAC0)
glm::mat4 layerViewProjection (const ScriptableObject& object) {
    const auto& scene = object.getScene ();
    const auto& perspective = object.getObject ().perspective;
    glm::mat4 viewProjection = scene.getWorldViewProjection (perspective != nullptr && perspective->value->getBool ());

    if (!scene.getCamera ().isPerspective () && scene.getScene ().camera.parallax.enabled->value->getBool ()) {
	const glm::vec2 offset = scene.getParallaxOffset (object.getObject ());
	viewProjection = glm::translate (viewProjection, glm::vec3 (offset.x, -offset.y, 0.0f));
    }

    return viewProjection;
}

// sub_1401ED0D0: the other layer's attachment point and a point 100 units along its x go to the screen and back onto
// this layer's plane. Returns a Mat3 of texture direction and position
void transform_attachment_to_texture_call (const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto& engine = engineOf (info);
    auto* object = layerObject (info.Data ());
    const auto* other = dynamic_cast<const ScriptableObject*> (layerArgument (engine, info[0]));
    std::array<float, 9> result = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f };

    // WE crashes on a missing layer, return the identity
    if (object == nullptr || other == nullptr) {
	info.GetReturnValue ().Set (makeMat3 (engine, result));
	return;
    }

    const auto [index, name] = indexOrNameArgument (info.GetIsolate (), info[1]);
    glm::mat4 attachment (1.0f);

    if (const auto* rig = layerRig (*other)) {
	if (const auto point = rig->attachmentMatrix (index.has_value () ? *index : rig->findAttachment (name))) {
	    attachment = *point;
	}
    }

    const glm::mat4 toScreen = layerViewProjection (*other) * layerWorld (*other) * attachment;
    const glm::mat4 fromScreen = glm::inverse (layerViewProjection (*object));
    const glm::mat4 toLocal = glm::inverse (layerWorld (*object));
    const glm::vec2 size = renderableSize (*object).value_or (glm::vec2 (0.0f));

    // sub_1401E5DD0: barycentrics of the line against (0,0,0) (1,0,0) (0,1,0) are local x/y, then into texture space
    const auto texturePoint = [&] (const glm::vec4& local) {
	const glm::vec4 clip = toScreen * local;
	const glm::vec2 ndc = glm::vec2 (clip) / clip.w;
	const glm::vec4 nearPoint = fromScreen * glm::vec4 (ndc, -1.0f, 1.0f);
	const glm::vec4 farPoint = fromScreen * glm::vec4 (ndc, 1.0f, 1.0f);
	const glm::vec3 origin = toLocal * (nearPoint / nearPoint.w);
	const glm::vec3 direction = glm::vec3 (toLocal * (farPoint / farPoint.w)) - origin;
	glm::vec2 uv (0.0f);

	if (std::abs (direction.z) > 1.1920929e-7f) {
	    const float t = -origin.z / direction.z;
	    uv = glm::vec2 (origin) + glm::vec2 (direction) * t;
	}

	return glm::vec2 ((uv.x - 0.5f) / size.x + 0.5f, 0.5f - (uv.y - 0.5f) / size.y);
    };

    const glm::vec2 position = texturePoint (glm::vec4 (0.0f, 0.0f, 0.0f, 1.0f));
    const glm::vec2 along = texturePoint (glm::vec4 (100.0f, 0.0f, 0.0f, 1.0f));
    const float length = glm::length (along - position);
    const glm::vec2 direction = length <= 1.1920929e-7f ? glm::vec2 (0.0f) : (along - position) / length;

    result = { direction.x, direction.y, 0.0f, direction.y, -direction.x, 0.0f, position.x, position.y, 1.0f };
    info.GetReturnValue ().Set (makeMat3 (engine, result));
}

// The members WE's layer objects have: scenescript64 builds each layer object from the object's property and method
// lists, parents included, plus getAnimation (sub_181652380). The lists are wallpaper64 2.8.42's static tables,
// object (sub_1401E0530) under renderable (sub_1401EE520) under image (sub_140211070) and text (sub_140258CA0);
// particles (sub_14024CB00), models (sub_140227470), lights (sub_14025DA80) and cameras (sub_1401F3460) sit
// directly on the object one. Each table is a hash map, Object.keys () walks them in the order live WE 2.8.42
// lists them (probe kit we_live/tests/keyorder): the type's properties, the renderable's, the object's, then the
// object's methods, the renderable's, the type's and getAnimation. Models and cameras weren't probed, their lists
// keep the table order
constexpr std::string_view ObjectProperties[] = {
    "solid", "name", "parallaxDepth", "sortorder", "scale", "angles", "origin", "disablepropagation",
};
constexpr std::string_view ObjectMethods[] = {
    "getChildren",         "getTransformMatrix", "rotateObjectSpace", "setParent",           "lookAt",
    "getAttachmentIndex",  "lookAtYaw",          "getParent",         "getAttachmentMatrix", "getAttachmentOrigin",
    "getAttachmentAngles",
};
constexpr std::string_view RenderableProperties[] = {
    "colorBlendMode", "ledsource", "nointerpolation", "castshadow",     "perspective", "visible",
    "brightness",     "alpha",     "color",           "copybackground", "size",        "clampuvs",
};
constexpr std::string_view RenderableMethods[] = { "transformAttachmentToTexture", "getEffect", "getEffectCount" };
constexpr std::string_view GroupProperties[] = { "visible" };
constexpr std::string_view ImageProperties[] = { "alignment" };
constexpr std::string_view ImageMethods[] = {
    "getBoneCount",
    "getTextureAnimation",
    "getVideoTexture",
    "getAnimationLayer",
    "getAnimationLayerCount",
    "setLocalBoneOrigin",
    "destroyAnimationLayer",
    "createAnimationLayer",
    "playSingleAnimation",
    "getBoneTransform",
    "setBoneTransform",
    "getLocalBoneTransform",
    "setLocalBoneTransform",
    "getLocalBoneAngles",
    "setLocalBoneAngles",
    "getLocalBoneOrigin",
    "getBlendShapeIndex",
    "getBlendShapeWeight",
    "getBoneParentIndex",
    "setBlendShapeWeight",
    "getBoneIndex",
    "applyBonePhysicsImpulse",
    "resetBonePhysicsSimulation",
};
constexpr std::string_view TextProperties[] = {
    "anchor",
    "verticalalign",
    "horizontalalign",
    "depthtest",
    "dropshadowoffset",
    "dropshadowcolor",
    "blursize",
    "outlinethickness",
    "dropshadowopacity",
    "dropshadow",
    "blur",
    "outline",
    "msdf",
    "maxrows",
    "maxwidth",
    "spacing",
    "text",
    "blockalign",
    "limituseellipsis",
    "limitrows",
    "dropshadowsize",
    "limitwidth",
    "font",
    "backgroundcolor",
    "outlinecolor",
    "pointsize",
    "opaquebackground",
    "padding",
    "backgroundbrightness",
};
constexpr std::string_view ParticleProperties[] = { "instance", "visible" };
constexpr std::string_view ParticleMethods[] = { "play", "emitParticles", "isPlaying", "stop", "pause" };
constexpr std::string_view ModelProperties[] = { "visible", "perspective", "castshadow", "rootmotion" };
constexpr std::string_view ModelMethods[] = {
    "getAnimationLayer",   "getAnimationLayerCount", "createAnimationLayer",
    "playSingleAnimation", "destroyAnimationLayer",
};
constexpr std::string_view LightProperties[] = {
    "castvolumetrics",  "usecookie",           "castshadow", "light",     "controlpoint", "cascadedistance2",
    "cascadedistance1", "volumetricsexponent", "visible",    "outercone", "exponent",     "cascadedistance0",
    "radius",           "lightsourcesize",     "density",    "intensity", "color",        "innercone",
};
constexpr std::string_view CameraProperties[] = { "visible", "fov", "zoom", "queuemode" };

struct LayerMember {
    std::string_view name;
    bool method;
};

// by the data model's type: a layer's JS object can be made while the base ScriptableObject constructor still runs
// (a script on one of its base properties), before the render object is its final type
std::vector<LayerMember> layerMembers (const ScriptableObject& object) {
    const auto& model = object.getObject ();
    std::vector<LayerMember> members;
    const auto add = [&members] (const auto& list, bool method) {
	for (const auto& name : list) {
	    members.push_back ({ name, method });
	}
    };
    const auto build = [&] (const auto& properties, bool renderable, const auto& methods) {
	add (properties, false);
	if (renderable) {
	    add (RenderableProperties, false);
	}
	add (ObjectProperties, false);
	add (ObjectMethods, true);
	if (renderable) {
	    add (RenderableMethods, true);
	}
	add (methods, true);
	members.push_back ({ "getAnimation", true });
    };
    constexpr std::array<std::string_view, 0> none {};

    if (model.is<Image> ()) {
	build (ImageProperties, true, ImageMethods);
    } else if (model.is<Text> ()) {
	build (TextProperties, true, none);
    } else if (model.is<Particle> ()) {
	build (ParticleProperties, false, ParticleMethods);
    } else if (model.is<Mesh> ()) {
	build (ModelProperties, false, ModelMethods);
    } else if (model.is<Light> ()) {
	build (LightProperties, false, none);
    } else if (model.is<SceneCamera> ()) {
	build (CameraProperties, false, none);
    } else {
	build (GroupProperties, false, none);
    }

    return members;
}

// a member without a native is there but undefined
struct LayerMethod {
    std::string_view name;
    v8::FunctionCallback callback;
};

constexpr LayerMethod LayerMethods[] = {
    { "play", JS::bind<playback_call, 0> },
    { "pause", JS::bind<playback_call, 1> },
    { "stop", JS::bind<playback_call, 3> },
    { "isPlaying", JS::bind<playback_call, 2> },
    { "getParent", JS::bind<hierarchy_call, 0> },
    { "setParent", set_parent_call },
    { "getBlendShapeIndex", JS::bind<blend_shape_call, GetBlendShapeIndex> },
    { "getBlendShapeWeight", JS::bind<blend_shape_call, GetBlendShapeWeight> },
    { "setBlendShapeWeight", JS::bind<blend_shape_call, SetBlendShapeWeight> },
    { "getChildren", JS::bind<hierarchy_call, 1> },
    { "getEffectCount", JS::bind<effect_call, 0> },
    { "getEffect", JS::bind<effect_call, 1> },
    { "transformAttachmentToTexture", transform_attachment_to_texture_call },
    { "getAnimationLayerCount", JS::bind<layer_animation_layer_call, GetAnimationLayerCount> },
    { "getAnimationLayer", JS::bind<layer_animation_layer_call, GetAnimationLayer> },
    { "createAnimationLayer", JS::bind<layer_animation_layer_call, CreateAnimationLayer> },
    { "playSingleAnimation", JS::bind<layer_animation_layer_call, PlaySingleAnimation> },
    { "destroyAnimationLayer", JS::bind<layer_animation_layer_call, DestroyAnimationLayer> },
    { "getTransformMatrix", JS::bind<transform_call, GetTransformMatrix> },
    { "rotateObjectSpace", JS::bind<transform_call, RotateObjectSpace> },
    { "lookAt", JS::bind<transform_call, LookAt> },
    { "lookAtYaw", JS::bind<transform_call, LookAtYaw> },
    { "getAttachmentIndex", JS::bind<transform_call, GetAttachmentIndex> },
    { "getAttachmentMatrix", JS::bind<transform_call, GetAttachmentMatrix> },
    { "getAttachmentOrigin", JS::bind<transform_call, GetAttachmentOrigin> },
    { "getAttachmentAngles", JS::bind<transform_call, GetAttachmentAngles> },
    { "getBoneCount", JS::bind<bone_call, GetBoneCount> },
    { "getBoneTransform", JS::bind<bone_call, GetBoneTransform> },
    { "setBoneTransform", JS::bind<bone_call, SetBoneTransform> },
    { "getLocalBoneTransform", JS::bind<bone_call, GetLocalBoneTransform> },
    { "setLocalBoneTransform", JS::bind<bone_call, SetLocalBoneTransform> },
    { "getLocalBoneAngles", JS::bind<bone_call, GetLocalBoneAngles> },
    { "setLocalBoneAngles", JS::bind<bone_call, SetLocalBoneAngles> },
    { "getLocalBoneOrigin", JS::bind<bone_call, GetLocalBoneOrigin> },
    { "setLocalBoneOrigin", JS::bind<bone_call, SetLocalBoneOrigin> },
    { "getBoneIndex", JS::bind<bone_call, GetBoneIndex> },
    { "getBoneParentIndex", JS::bind<bone_call, GetBoneParentIndex> },
    { "applyBonePhysicsImpulse", JS::bind<bone_call, ApplyBonePhysicsImpulse> },
    { "resetBonePhysicsSimulation", JS::bind<bone_call, ResetBonePhysicsSimulation> },
    { "emitParticles", JS::bind<particle_call, 1> },
    { "getAnimation", get_animation_call },
    { "getTextureAnimation", texture_animation_call },
    { "getVideoTexture", video_texture_call },
};

// layer properties: the object's DynamicValue when it has one, the few that aren't one, undefined otherwise
void layer_get (v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info) {
    auto* isolate = info.GetIsolate ();
    auto* layer = JS::unwrap<Layer> (info.Data ());

    if (layer == nullptr || layer->object == nullptr) {
	return;
    }

    auto& engine = ScriptEngine::from (isolate);
    auto& object = *layer->object;
    const std::string name = JS::toString (isolate, property);

    if (auto* value = object.tryGetProperty (name); value != nullptr) {
	// vec2 descriptors filled by a JSON number (sub_1401E0530)
	const bool scalar = value->getType () == DynamicValue::Float || value->getType () == DynamicValue::Int;

	if (scalar
	    && (name == "padding" || name == "spacing" || name == "dropshadowoffset" || name == "parallaxDepth")) {
	    info.GetReturnValue ().Set (engine.getAdapters ().vec2->create (glm::vec2 (value->getFloat ())));
	    return;
	}

	info.GetReturnValue ().Set (engine.propertyToJs (*value, name));
	return;
    }

    if (name == "name") {
	info.GetReturnValue ().Set (JS::string (isolate, object.getObject ().name));
    } else if (name == "id") {
	info.GetReturnValue ().Set (object.getObject ().id);
    } else if (name == "instance" && object.is<CParticle> ()) {
	info.GetReturnValue ().Set (engine.getAdapters ().object->particleInstance (*layer));
    } else if (name == "size") {
	if (const auto size = renderableSize (object)) {
	    info.GetReturnValue ().Set (engine.getAdapters ().vec2->create (*size));
	}
    }
}

// writes go through to the real property so `thisLayer.visible = ...` takes effect. Members without a DynamicValue
// (name, size, "horizontalalign" on images, ...) take the write and drop it: scripts run as strict mode modules,
// throwing here would abort the whole script over an unsupported property
void layer_set (v8::Local<v8::Name> property, v8::Local<v8::Value> value, const v8::PropertyCallbackInfo<void>& info) {
    auto* isolate = info.GetIsolate ();
    auto* layer = JS::unwrap<Layer> (info.Data ());

    if (layer == nullptr || layer->object == nullptr) {
	return;
    }

    const std::string name = JS::toString (isolate, property);

    if (auto* target = layer->object->tryGetProperty (name); target != nullptr) {
	ScriptEngine::from (isolate).assignPropertyJsValue (value, *target, name);
    }
}

void native_get (v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
    if (const auto* property = JS::unwrap<NativeProperty> (info.Data ()); property != nullptr && property->get) {
	info.GetReturnValue ().Set (property->get ());
    }
}

void native_set (v8::Local<v8::Name>, v8::Local<v8::Value> value, const v8::PropertyCallbackInfo<void>& info) {
    if (const auto* property = JS::unwrap<NativeProperty> (info.Data ()); property != nullptr && property->set) {
	property->set (value);
    }
}
} // namespace

ScriptableObjectAdapter::ScriptableObjectAdapter (ScriptEngine& engine) : m_engine (engine) {
    auto* isolate = engine.getIsolate ();
    const auto context = engine.getContext ();

    this->m_layerKey.Reset (isolate, v8::Private::New (isolate, JS::string (isolate, "ILayer")));
    this->m_layerConstructor.Reset (
	isolate, v8::FunctionTemplate::New (isolate)->GetFunction (context).ToLocalChecked ()
    );
    this->m_nativeConstructor.Reset (
	isolate, v8::FunctionTemplate::New (isolate)->GetFunction (context).ToLocalChecked ()
    );
}

Layer& ScriptableObjectAdapter::layerOf (ScriptableObject& object) {
    auto& layer = this->m_layers[&object];

    if (layer == nullptr) {
	layer = std::make_unique<Layer> (Layer { .object = &object });
    }

    return *layer;
}

v8::Local<v8::Object> ScriptableObjectAdapter::instantiate (ScriptableObject& object) {
    auto* isolate = this->m_engine.getIsolate ();
    const auto context = this->m_engine.getContext ();
    auto& layer = this->layerOf (object);

    if (!layer.instance.IsEmpty ()) {
	return layer.instance.Get (isolate);
    }

    const v8::Local<v8::Object> result
	= this->m_layerConstructor.Get (isolate)->NewInstance (context).ToLocalChecked ();
    const auto data = JS::external (isolate, &layer);
    const auto members = layerMembers (object);
    const bool particle = object.getObject ().is<Particle> ();

    result->SetPrivate (context, this->m_layerKey.Get (isolate), data).Check ();

    const auto defineMethod = [&] (std::string_view name, v8::PropertyAttribute attributes) {
	const auto method = std::ranges::find (LayerMethods, name, &LayerMethod::name);
	const v8::Local<v8::Value> value = method == std::end (LayerMethods)
	    ? v8::Undefined (isolate).As<v8::Value> ()
	    : JS::function (context, method->callback, data).As<v8::Value> ();

	result->DefineOwnProperty (context, JS::name (isolate, name), value, attributes).Check ();
    };

    // own enumerable members like WE's (template accessors and functions without DontEnum), so `in`,
    // hasOwnProperty, getOwnPropertyDescriptor and Object.keys see them next to whatever scripts store on the layer
    for (const auto& [name, method] : members) {
	if (method) {
	    defineMethod (name, v8::None);
	    continue;
	}

	// read only in sub_14024CB00 (+96 = 2)
	const bool readOnly = particle && name == "instance";

	result
	    ->SetNativeDataProperty (
		context, JS::name (isolate, name), layer_get, readOnly ? nullptr : layer_set, data,
		readOnly ? v8::ReadOnly : v8::None
	    )
	    .Check ();
    }

    // ours only, not members: the id, and the calls every layer has here (thisLayer.play () from a base property's
    // init (), the bone calls answering with their defaults)
    result->SetNativeDataProperty (context, JS::name (isolate, "id"), layer_get, layer_set, data, v8::DontEnum)
	.Check ();

    for (const auto& method : LayerMethods) {
	if ((method.name == "emitParticles" && !particle)
	    || std::ranges::find (members, method.name, &LayerMember::name) != members.end ()) {
	    continue;
	}

	defineMethod (method.name, v8::DontEnum);
    }

    layer.instance.Reset (isolate, result);
    return result;
}

void ScriptableObjectAdapter::forget (const ScriptableObject& object) {
    const auto it = this->m_layers.find (&object);

    if (it == this->m_layers.end ()) {
	return;
    }

    it->second->object = nullptr;
    it->second->instance.Reset ();
    it->second->textureAnimation.Reset ();
    it->second->particleInstance.Reset ();
    this->m_forgotten.push_back (std::move (it->second));
    this->m_layers.erase (it);
}

ScriptableObject* ScriptableObjectAdapter::getObject (v8::Local<v8::Value> value) const {
    if (value.IsEmpty () || !value->IsObject ()) {
	return nullptr;
    }

    auto* isolate = this->m_engine.getIsolate ();
    v8::Local<v8::Value> data;

    if (!value.As<v8::Object> ()
	     ->GetPrivate (this->m_engine.getContext (), this->m_layerKey.Get (isolate))
	     .ToLocal (&data)) {
	return nullptr;
    }

    return layerObject (data);
}

v8::Local<v8::Object> ScriptableObjectAdapter::makeNativeObject (
    std::vector<NativeProperty> properties, const std::vector<std::pair<std::string, v8::Local<v8::Function>>>& methods
) {
    auto* isolate = this->m_engine.getIsolate ();
    const auto context = this->m_engine.getContext ();
    const v8::Local<v8::Object> result
	= this->m_nativeConstructor.Get (isolate)->NewInstance (context).ToLocalChecked ();
    auto& members
	= *this->m_nativeMembers.emplace_back (std::make_unique<std::vector<NativeProperty>> (std::move (properties)));

    for (auto& property : members) {
	result
	    ->SetNativeDataProperty (
		context, JS::name (isolate, property.name), native_get, property.set ? native_set : nullptr,
		JS::external (isolate, &property), property.set ? v8::None : v8::ReadOnly
	    )
	    .Check ();
    }

    for (const auto& [name, function] : methods) {
	JS::define (context, result, name, function);
    }

    return result;
}

v8::Local<v8::Value> ScriptableObjectAdapter::textureAnimation (Layer& layer) {
    auto* isolate = this->m_engine.getIsolate ();
    const auto context = this->m_engine.getContext ();
    auto* image = layer.object == nullptr || !layer.object->is<CImage> () ? nullptr : layer.object->as<CImage> ();

    if (image == nullptr || image->getTextureAnimation () == nullptr) {
	return v8::Null (isolate);
    }

    if (!layer.textureAnimation.IsEmpty ()) {
	return layer.textureAnimation.Get (isolate);
    }

    // the members in the order WE's object lists them: properties, then methods, then IObject's getAnimation.
    // frameCount and duration are read only (+96 = 2)
    Layer* owner = &layer;
    const auto alive = [owner] { return owner->object != nullptr ? owner->object->as<CImage> () : nullptr; };
    std::vector<NativeProperty> properties = {
	{ .name = "frameCount",
	  .get = [isolate, alive] () -> v8::Local<v8::Value> {
	      if (auto* current = alive ()) {
		  return v8::Integer::New (isolate, current->getTextureFrameCount ());
	      }
	      return v8::Undefined (isolate);
	  } },
	{ .name = "duration",
	  .get = [isolate, alive] () -> v8::Local<v8::Value> {
	      if (auto* current = alive ()) {
		  return v8::Number::New (isolate, current->getTextureDuration ());
	      }
	      return v8::Undefined (isolate);
	  } },
	{ .name = "rate",
	  .get = [isolate, alive] () -> v8::Local<v8::Value> {
	      if (auto* current = alive (); current != nullptr && current->getTextureAnimation () != nullptr) {
		  return v8::Number::New (isolate, current->getTextureAnimation ()->rate);
	      }
	      return v8::Undefined (isolate);
	  },
	  .set =
	      [alive] (v8::Local<v8::Value> value) {
		  auto* current = alive ();
		  auto* animation = current == nullptr ? nullptr : current->getTextureAnimation ();

		  if (animation == nullptr) {
		      return;
		  }

		  if (value->IsNumber ()) {
		      animation->rate = static_cast<float> (value.As<v8::Number> ()->Value ());
		  }

		  // sub_1401FA4A0, the property's change callback
		  if (animation->rate != 1.0f && !animation->detached) {
		      detachTextureAnimation (*current, *animation);
		  }
	      } },
    };

    const auto data = JS::external (isolate, &layer);
    // every method reports length 0, like WE's
    const std::vector<std::pair<std::string, v8::Local<v8::Function>>> methods = {
	{ "play", JS::function (context, JS::bind<textureanimation_call, TexturePlay>, data) },
	{ "isPlaying", JS::function (context, JS::bind<textureanimation_call, TextureIsPlaying>, data) },
	{ "stop", JS::function (context, JS::bind<textureanimation_call, TextureStop>, data) },
	{ "pause", JS::function (context, JS::bind<textureanimation_call, TexturePause>, data) },
	{ "setFrame", JS::function (context, JS::bind<textureanimation_call, TextureSetFrame>, data) },
	{ "getFrame", JS::function (context, JS::bind<textureanimation_call, TextureGetFrame>, data) },
	{ "join", JS::function (context, JS::bind<textureanimation_call, TextureJoin>, data) },
	{ "getAnimation", JS::function (context, JS::bind<textureanimation_call, TextureGetAnimation>, data) },
    };

    const auto result = this->makeNativeObject (std::move (properties), methods);
    layer.textureAnimation.Reset (isolate, result);
    return result;
}

v8::Local<v8::Value> ScriptableObjectAdapter::animation (int systemId, int clockId) {
    auto* isolate = this->m_engine.getIsolate ();
    const auto context = this->m_engine.getContext ();

    if (const auto it = this->m_animations.find (clockId); it != this->m_animations.end ()) {
	return it->second.Get (isolate);
    }

    auto* system = AnimationSystem::find (systemId);
    auto* clock = system == nullptr ? nullptr : system->clock (clockId);

    if (clock == nullptr) {
	return v8::Null (isolate);
    }

    // looked up by id on every access, the timeline may be gone by then
    const auto clockOf = [systemId, clockId] () -> AnimationClock* {
	auto* current = AnimationSystem::find (systemId);
	return current == nullptr ? nullptr : current->clock (clockId);
    };
    const auto number = [isolate, clockOf] (const std::function<double (AnimationClock&)>& read) {
	return [isolate, clockOf, read] () -> v8::Local<v8::Value> {
	    auto* current = clockOf ();
	    return current == nullptr ? v8::Undefined (isolate).As<v8::Value> ()
				      : v8::Number::New (isolate, read (*current)).As<v8::Value> ();
	};
    };

    // sub_1401A8C10: frame time 1 / fps and duration length / fps, as floats
    std::vector<NativeProperty> properties = {
	{ .name = "name",
	  .get = [isolate, clockOf] () -> v8::Local<v8::Value> {
	      auto* current = clockOf ();
	      return current == nullptr ? v8::Undefined (isolate).As<v8::Value> ()
					: JS::string (isolate, current->getDefinition ().name).As<v8::Value> ();
	  } },
	{ .name = "frameCount", .get = number ([] (AnimationClock& c) {
				    return static_cast<double> (static_cast<int32_t> (c.getDefinition ().length));
				}) },
	{ .name = "duration",
	  .get = number ([] (AnimationClock& c) {
	      const auto& d = c.getDefinition ();
	      return static_cast<double> (static_cast<float> (static_cast<int32_t> (d.length)) / d.fps);
	  }) },
	{ .name = "fps", .get = number ([] (AnimationClock& c) {
			     return static_cast<double> (1.0f / (1.0f / c.getDefinition ().fps));
			 }) },
	{ .name = "rate",
	  .get = number ([] (AnimationClock& c) { return static_cast<double> (c.getRate ()); }),
	  .set =
	      [clockOf] (v8::Local<v8::Value> value) {
		  if (auto* current = clockOf (); current != nullptr && value->IsNumber ()) {
		      current->setRate (static_cast<float> (value.As<v8::Number> ()->Value ()));
		  }
	      } },
    };

    const auto data = JS::data (isolate, { v8::Integer::New (isolate, systemId), v8::Integer::New (isolate, clockId) });
    const std::vector<std::pair<std::string, v8::Local<v8::Function>>> methods = {
	{ "play", JS::function (context, JS::bind<animation_call, AnimationPlay>, data) },
	{ "isPlaying", JS::function (context, JS::bind<animation_call, AnimationIsPlaying>, data) },
	{ "stop", JS::function (context, JS::bind<animation_call, AnimationStop>, data) },
	{ "pause", JS::function (context, JS::bind<animation_call, AnimationPause>, data) },
	{ "setFrame", JS::function (context, JS::bind<animation_call, AnimationSetFrame>, data) },
	{ "getFrame", JS::function (context, JS::bind<animation_call, AnimationGetFrame>, data) },
	{ "getAnimation", JS::function (context, JS::bind<animation_call, AnimationGetAnimation>, data) },
    };

    const auto result = this->makeNativeObject (std::move (properties), methods);
    this->m_animations[clockId].Reset (isolate, result);
    return result;
}

v8::Local<v8::Value> ScriptableObjectAdapter::effect (ImageEffect& effect) {
    auto* isolate = this->m_engine.getIsolate ();
    const auto context = this->m_engine.getContext ();

    if (const auto it = this->m_effects.find (&effect); it != this->m_effects.end ()) {
	return it->second.Get (isolate);
    }

    // IEffect (sub_1401EFCA0) in live WE's order
    auto& engine = this->m_engine;
    std::vector<NativeProperty> properties = {
	{ .name = "name",
	  .get = [isolate, &effect] () -> v8::Local<v8::Value> { return JS::string (isolate, effect.name); },
	  .set =
	      [isolate, &effect] (v8::Local<v8::Value> value) {
		  const v8::TryCatch tryCatch (isolate);
		  v8::Local<v8::String> text;

		  if (value->ToString (isolate->GetCurrentContext ()).ToLocal (&text)) {
		      effect.name = JS::toString (isolate, text);
		  }
	      } },
	{ .name = "visible",
	  .get = [isolate, &effect] () -> v8::Local<v8::Value> {
	      return v8::Boolean::New (isolate, effect.visible->value->getBool ());
	  },
	  .set
	  = [&effect, &engine] (v8::Local<v8::Value> value) { engine.assignJsValue (value, *effect.visible->value); } },
    };

    const auto data = JS::external (isolate, &effect);
    const std::vector<std::pair<std::string, v8::Local<v8::Function>>> methods = {
	{ "getMaterial", JS::function (context, JS::bind<effect_method, EffectGetMaterial>, data) },
	{ "executeMaterialFunction",
	  JS::function (context, JS::bind<effect_method, EffectExecuteMaterialFunction>, data) },
	{ "getMaterialCount", JS::function (context, JS::bind<effect_method, EffectGetMaterialCount>, data) },
	{ "setMaterialProperty", JS::function (context, JS::bind<effect_method, EffectSetMaterialProperty>, data) },
	{ "getAnimation", JS::function (context, JS::bind<effect_method, EffectGetAnimation>, data) },
    };

    const auto result = this->makeNativeObject (std::move (properties), methods);
    this->m_effects[&effect].Reset (isolate, result);
    return result;
}

v8::Local<v8::Value> ScriptableObjectAdapter::material (ImageEffect& effect, size_t passIndex) {
    auto* isolate = this->m_engine.getIsolate ();
    const auto context = this->m_engine.getContext ();
    const auto key = std::make_pair (static_cast<const ImageEffect*> (&effect), passIndex);
    auto& engine = this->m_engine;
    const auto passOf
	= [&engine, &effect, passIndex] { return engine.getScene ().findEffectMaterial (effect, passIndex); };
    auto* pass = passOf ();

    if (pass == nullptr) {
	return v8::Null (isolate);
    }

    if (const auto it = this->m_materials.find (key); it != this->m_materials.end ()) {
	return it->second.Get (isolate);
    }

    auto& state = this->m_materialStates.try_emplace (key, materialState (pass->getPass ())).first->second;
    std::vector<NativeProperty> properties;

    // own constants first, then the static IMaterial members
    for (const auto& name : pass->getMaterialConstantNames ()) {
	properties.push_back (
	    { .name = name,
	      .get = [isolate, &engine, passOf, name] () -> v8::Local<v8::Value> {
		  auto* current = passOf ();
		  const auto* value = current != nullptr ? current->getMaterialConstant (name) : nullptr;
		  const int size = current != nullptr ? current->getMaterialConstantSize (name) : 0;

		  if (value == nullptr || size == 0) {
		      return v8::Undefined (isolate);
		  }

		  const bool scalar
		      = value->getType () == DynamicValue::Float || value->getType () == DynamicValue::Int;
		  const glm::vec4 vector
		      = scalar ? glm::vec4 (value->getFloat (), 0.0f, 0.0f, 0.0f) : value->getVec4 ();
		  // flag 4 floats and vec3s in degrees (sub_1816208E0)
		  const float factor = current->isMaterialConstantInDegrees (name) ? 57.29578f : 1.0f;

		  switch (size) {
		      case 2:
			  return engine.getAdapters ().vec2->create (glm::vec2 (vector));
		      case 3:
			  return engine.getAdapters ().vec3->create (glm::vec3 (vector) * factor);
		      case 4:
			  return engine.getAdapters ().vec4->create (vector);
		      default:
			  return v8::Number::New (isolate, value->getFloat () * factor);
		  }
	      },
	      .set =
		  [&engine, passOf, name] (v8::Local<v8::Value> value) {
		      auto* current = passOf ();

		      auto* target = current != nullptr ? current->getScriptConstant (name) : nullptr;

		      if (target == nullptr) {
			  return;
		      }

		      engine.assignPropertyJsValue (value, *target, "");

		      // sub_181620E10
		      if (current->isMaterialConstantInDegrees (name)) {
			  if (target->getType () == DynamicValue::Float) {
			      target->update (target->getFloat () * 0.017453292f, DynamicValue::UpdateSource::Script);
			  } else if (target->getType () == DynamicValue::Vec3) {
			      target->update (target->getVec3 () * 0.017453292f, DynamicValue::UpdateSource::Script);
			  }
		      }
		  } }
	);
    }

    for (const auto& member : MaterialEnums) {
	properties.push_back (
	    { .name = member.name,
	      // sub_140158490
	      .get = [isolate, &state, &member] () -> v8::Local<v8::Value> {
		  v8::Local<v8::Value> result = v8::Undefined (isolate);

		  for (const auto& [text, value] : member.values) {
		      if (value == state[member.byte]) {
			  result = JS::string (isolate, text);
		      }
		  }

		  return result;
	      },
	      // sub_1401583B0: unknown names are the first entry
	      .set =
		  [isolate, &state, &member, passOf] (v8::Local<v8::Value> value) {
		      const std::string text = JS::toString (isolate, value);
		      const auto entry = std::ranges::find_if (member.values, [&text] (const auto& cur) {
			  return text == cur.first;
		      });

		      state[member.byte]
			  = entry != member.values.end () ? entry->second : member.values.front ().second;

		      if (auto* current = passOf ()) {
			  applyMaterialState (*current, state);
		      }
		  } }
	);
    }

    // the group registerEffectConstants used
    std::string group;

    for (const auto* object : engine.getScene ().getObjectsByRenderOrder ()) {
	const auto* effects = effectsOf (object->getObject ());

	if (effects == nullptr) {
	    continue;
	}

	for (size_t index = 0; index < effects->size (); index++) {
	    if ((*effects)[index].get () != &effect) {
		continue;
	    }

	    const auto& passes = effect.effect->passes;
	    const auto materialIndex
		= std::count_if (passes.begin (), passes.begin () + passIndex, [] (const auto& cur) {
		      return cur->material.has_value ();
		  });
	    group = "obj" + std::to_string (object->getId ()) + "/fx" + std::to_string (index) + ".p"
		+ std::to_string (materialIndex) + ".";
	}
    }

    const std::vector<std::pair<std::string, v8::Local<v8::Function>>> methods = {
	{ "getAnimation", JS::function (context, material_get_animation, JS::string (isolate, group)) },
    };

    const auto result = this->makeNativeObject (std::move (properties), methods);
    this->m_materials[key].Reset (isolate, result);
    return result;
}

v8::Local<v8::Value> ScriptableObjectAdapter::particleInstance (Layer& layer) {
    auto* isolate = this->m_engine.getIsolate ();
    const auto context = this->m_engine.getContext ();

    if (layer.object == nullptr || !layer.object->is<CParticle> ()) {
	return v8::Undefined (isolate);
    }

    if (!layer.particleInstance.IsEmpty ()) {
	return layer.particleInstance.Get (isolate);
    }

    auto& engine = this->m_engine;
    Layer* owner = &layer;
    const auto alive = [owner] { return owner->object != nullptr ? owner->object->as<CParticle> () : nullptr; };
    const auto& instanceOverride = alive ()->getParticle ().instanceOverride;
    std::vector<NativeProperty> properties;

    const auto addFloat = [&] (const char* name, DynamicValue& value) {
	properties.push_back (
	    { .name = name,
	      .get = [&engine, &value, isolate, alive] () -> v8::Local<v8::Value> {
		  return alive () == nullptr ? v8::Undefined (isolate).As<v8::Value> ()
					     : engine.propertyToJs (value, "");
	      },
	      .set =
		  [&engine, &value, alive] (v8::Local<v8::Value> js) {
		      if (alive () != nullptr) {
			  engine.assignPropertyJsValue (js, value, "");
		      }
		  } }
	);
    };

    // every write only marks the struct dirty (sub_14022AB30), the particle reads it live. The order is live WE's
    for (size_t i = 8; i-- > 0;) {
	for (const bool angle : { true, false }) {
	    properties.push_back (
		{ .name = (angle ? "controlpointangle" : "controlpoint") + std::to_string (i),
		  .get = [&engine, isolate, alive, i, angle] () -> v8::Local<v8::Value> {
		      auto* particle = alive ();
		      return particle == nullptr
			  ? v8::Undefined (isolate).As<v8::Value> ()
			  : makeVec3 (engine, particle->getInstanceControlPoint (i, angle)).As<v8::Value> ();
		  },
		  .set =
		      [alive, isolate, i, angle] (v8::Local<v8::Value> js) {
			  if (auto* particle = alive ()) {
			      particle->setInstanceControlPoint (
				  i, angle, readVec3 (isolate->GetCurrentContext (), js)
			      );
			  }
		      } }
	    );
	}
    }

    addFloat ("rate", *instanceOverride.rate->value);
    addFloat ("brightness", *instanceOverride.brightness->value);
    properties.push_back (
	{ .name = "colorn",
	  .get = [&engine, isolate, alive] () -> v8::Local<v8::Value> {
	      auto* particle = alive ();
	      return particle == nullptr ? v8::Undefined (isolate).As<v8::Value> ()
					 : makeVec3 (engine, particle->getInstanceColor ()).As<v8::Value> ();
	  },
	  .set =
	      [alive, isolate] (v8::Local<v8::Value> js) {
		  if (auto* particle = alive ()) {
		      particle->setInstanceColor (readVec3 (isolate->GetCurrentContext (), js));
		  }
	      } }
    );
    addFloat ("lifetime", *instanceOverride.lifetime->value);
    addFloat ("speed", *instanceOverride.speed->value);
    addFloat ("size", *instanceOverride.size->value);
    addFloat ("count", *instanceOverride.count->value);
    addFloat ("alpha", *instanceOverride.alpha->value);

    const std::vector<std::pair<std::string, v8::Local<v8::Function>>> methods = {
	{ "getAnimation", JS::function (context, JS::bind<particle_call, 0>, JS::external (isolate, &layer)) },
    };

    const auto result = this->makeNativeObject (std::move (properties), methods);
    layer.particleInstance.Reset (isolate, result);
    return result;
}
