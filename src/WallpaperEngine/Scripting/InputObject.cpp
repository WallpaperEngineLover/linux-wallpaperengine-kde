#include "InputObject.h"

#include "EngineObject.h"
#include "ScriptEngine.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"

using namespace WallpaperEngine::Scripting;

JSValue get_cursor_world_position (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JSClassID classId;
    auto* input = static_cast<InputObject*> (JS_GetAnyOpaque (this_val, &classId));

    auto& scene = input->getScene ();
    const auto position = scene.getMousePositionNormalized ();

    // same scene space as layer origins (y up from the bottom), and the point the cursor event hit test uses
    JSValue result = scene.getScriptEngine ().getAdapters ().vec3->instantiate ();

    JS_SetPropertyStr (ctx, result, "x", JS_NewFloat64 (ctx, position->x * static_cast<float> (scene.getWidth ())));
    JS_SetPropertyStr (ctx, result, "y", JS_NewFloat64 (ctx, position->y * static_cast<float> (scene.getHeight ())));
    JS_SetPropertyStr (ctx, result, "z", JS_NewFloat64 (ctx, 0.0));

    return result;
}

JSValue get_cursor_screen_position (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JSClassID classId;
    auto* input = static_cast<InputObject*> (JS_GetAnyOpaque (this_val, &classId));
    auto position = input->getScene ().getMousePositionNormalized ();

    JSValue result = input->getScene ().getScriptEngine ().getAdapters ().vec2->instantiate ();

    JS_SetPropertyStr (ctx, result, "x", JS_NewFloat64 (ctx, position->x));
    JS_SetPropertyStr (ctx, result, "y", JS_NewFloat64 (ctx, position->y));

    return result;
}

JSValue get_cursor_left_down (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    JSClassID classId;
    auto* input = static_cast<InputObject*> (JS_GetAnyOpaque (this_val, &classId));

    return JS_NewBool (ctx, input->getScene ().isCursorLeftDown ());
}

JSValue input_set_value (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) { return JS_EXCEPTION; }

InputObject::InputObject (ScriptEngine& engine, Render::Wallpapers::CScene& scene) :
    m_scene (scene), m_engine (engine), m_classId (0) {
    this->m_definition = { .class_name = "IInput" };
    JS_NewClassID (this->m_engine.getRuntime (), &this->m_classId);
    JS_NewClass (this->m_engine.getRuntime (), this->m_classId, &this->m_definition);
    this->m_instance = JS_NewObjectClass (this->m_engine.getContext (), this->m_classId);

    JS_DupValue (this->m_engine.getContext (), this->m_instance);

    JS_SetOpaque (this->m_instance, this);
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance,
	JS_NewAtom (this->m_engine.getContext (), "cursorWorldPosition"),
	JS_NewCFunction (this->m_engine.getContext (), get_cursor_world_position, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), input_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance,
	JS_NewAtom (this->m_engine.getContext (), "cursorScreenPosition"),
	JS_NewCFunction (this->m_engine.getContext (), get_cursor_screen_position, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), input_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "cursorLeftDown"),
	JS_NewCFunction (this->m_engine.getContext (), get_cursor_left_down, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), input_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
}
InputObject::~InputObject () { JS_FreeValue (this->m_engine.getContext (), this->m_instance); }