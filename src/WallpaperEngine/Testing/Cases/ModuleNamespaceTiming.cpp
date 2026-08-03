#include <catch2/catch_test_macros.hpp>

#include "quickjs.h"

#include <cstring>
#include <memory>
#include <string>

// Isolated repro for the queueScript() timing question: if we set some external C++ state
// *before* calling JS_EvalFunction() on a compile-only-obtained module, does the module's
// top-level code (executed synchronously inside JS_EvalFunction, for a non-async/no-top-level-
// await module) actually observe that state when it calls a bound native function?
// This exists to verify the mechanism ScriptEngine::queueScript() relies on (setting
// m_runningModule before evaluating a script's module body so createScriptProperties()/finish()
// can resolve the right object) independent of the full engine/scene/GL stack.

namespace {
int g_probeValue = -1;

JSValue probe (JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/) {
    return JS_NewInt32 (ctx, g_probeValue);
}
} // namespace

TEST_CASE ("QuickJS module top-level code sees external state set before JS_EvalFunction") {
    JSRuntime* rt = JS_NewRuntime ();
    JSContext* ctx = JS_NewContext (rt);

    JSValue globalObj = JS_GetGlobalObject (ctx);
    JS_SetPropertyStr (ctx, globalObj, "probe", JS_NewCFunction (ctx, probe, "probe", 0));
    JS_FreeValue (ctx, globalObj);

    const char* source = "export var x = probe();";

    JSValue compiled = JS_Eval (ctx, source, strlen (source), "<test-module>", JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);

    REQUIRE_FALSE (JS_IsException (compiled));

    auto* moduleDef = static_cast<JSModuleDef*> (JS_VALUE_GET_PTR (compiled));

    // set the external state *after* compiling but *before* evaluating - mirrors
    // ScriptEngine::queueScript() setting m_runningModule right before JS_EvalFunction
    g_probeValue = 777;

    JSValue evalResult = JS_EvalFunction (ctx, compiled);

    REQUIRE_FALSE (JS_IsException (evalResult));
    JS_FreeValue (ctx, evalResult);

    JSValue ns = JS_GetModuleNamespace (ctx, moduleDef);

    REQUIRE_FALSE (JS_IsException (ns));

    JSValue x = JS_GetPropertyStr (ctx, ns, "x");
    int32_t xVal = -1;
    JS_ToInt32 (ctx, &xVal, x);

    JS_FreeValue (ctx, x);
    JS_FreeValue (ctx, ns);

    REQUIRE (xVal == 777);

    JS_FreeContext (ctx);
    JS_FreeRuntime (rt);
}

// `this_val` is a *borrowed* reference (JSValueConst) - a C function returning it directly
// under-counts the object's refcount by one per chained call, since the interpreter treats
// whatever a JSValue-returning function hands back as owned and frees it once done. A version of
// this test exercising the non-duplicating path deliberately isn't kept here since it corrupts
// the heap and segfaults the whole test binary by design - only the fixed path is verified below.
namespace {
int g_chainableFinalizedCount = 0;
JSClassID g_chainableClassId = 0;

void chainableFinalizer (JSRuntime* /*rt*/, JSValueConst /*val*/) { g_chainableFinalizedCount++; }

JSValue chainFixed (JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/) {
    return JS_DupValue (ctx, this_val);
}

JSValue makeChainable (JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/) {
    JSValue obj = JS_NewObjectClass (ctx, g_chainableClassId);
    JS_SetOpaque (obj, reinterpret_cast<void*> (1));
    return obj;
}
} // namespace

TEST_CASE ("Duplicating this_val before returning it from a chained method keeps refcounting correct") {
    g_chainableFinalizedCount = 0;

    JSRuntime* rt = JS_NewRuntime ();
    JSContext* ctx = JS_NewContext (rt);

    JS_NewClassID (rt, &g_chainableClassId);
    JSClassDef classDef = { .class_name = "Chainable", .finalizer = chainableFinalizer };
    JS_NewClass (rt, g_chainableClassId, &classDef);

    JSValue proto = JS_NewObject (ctx);
    JS_DefinePropertyValueStr (
	ctx, proto, "add", JS_NewCFunction (ctx, chainFixed, "add", 0), JS_PROP_ENUMERABLE
    );
    JS_SetClassProto (ctx, g_chainableClassId, proto);

    JSValue globalObj = JS_GetGlobalObject (ctx);
    JS_SetPropertyStr (ctx, globalObj, "make", JS_NewCFunction (ctx, makeChainable, "make", 0));
    JS_FreeValue (ctx, globalObj);

    const char* source = "export var x = make().add().add().add().add();";
    JSValue compiled = JS_Eval (
	ctx, source, strlen (source), "<test-chain-fixed>", JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY
    );

    REQUIRE_FALSE (JS_IsException (compiled));

    auto* moduleDef = static_cast<JSModuleDef*> (JS_VALUE_GET_PTR (compiled));
    JSValue evalResult = JS_EvalFunction (ctx, compiled);

    REQUIRE (JS_PromiseState (ctx, evalResult) == JS_PROMISE_FULFILLED);
    JS_FreeValue (ctx, evalResult);

    // must not have been finalized yet - the module's `x` binding still holds a live reference
    JSValue ns = JS_GetModuleNamespace (ctx, moduleDef);
    JSValue x = JS_GetPropertyStr (ctx, ns, "x");
    REQUIRE (JS_IsObject (x));
    REQUIRE (g_chainableFinalizedCount == 0);

    JS_FreeValue (ctx, x);
    JS_FreeValue (ctx, ns);
    JS_FreeContext (ctx);
    JS_FreeRuntime (rt);

    // freeing the context/runtime drops the last reference - finalizes exactly once, not zero
    // (leak) and not more than once (double free)
    REQUIRE (g_chainableFinalizedCount == 1);
}

TEST_CASE ("A module-level throw does not surface via JS_IsException on JS_EvalFunction's result") {
    JSRuntime* rt = JS_NewRuntime ();
    JSContext* ctx = JS_NewContext (rt);

    // calling a method on undefined mirrors the real bug: createScriptProperties() can return
    // undefined (e.g. no matching instance registered), and the script's chained .addSlider(...)
    // call on that throws - but as a *module-level* throw, not a C-level JS_EXCEPTION.
    const char* source = "export var x = undefined.someMethod();";

    JSValue compiled = JS_Eval (ctx, source, strlen (source), "<test-throwing-module>", JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);

    REQUIRE_FALSE (JS_IsException (compiled));

    JSValue evalResult = JS_EvalFunction (ctx, compiled);

    // this is the trap: the promise wrapper itself is never an "exception" JSValue, even though
    // the module body threw - JS_IsException(evalResult) alone can never detect this.
    REQUIRE_FALSE (JS_IsException (evalResult));
    REQUIRE (JS_PromiseState (ctx, evalResult) == JS_PROMISE_REJECTED);

    JSValue reason = JS_PromiseResult (ctx, evalResult);
    const char* str = JS_ToCString (ctx, reason);
    REQUIRE (str != nullptr);
    CHECK (std::string (str).find ("undefined") != std::string::npos);

    JS_FreeCString (ctx, str);
    JS_FreeValue (ctx, reason);
    JS_FreeValue (ctx, evalResult);

    JS_FreeContext (ctx);
    JS_FreeRuntime (rt);
}

namespace {
JSClassID g_exoticProtoClassId = 0;

// mirrors vector_property_get's shape: an exotic getter that only special-cases one hardcoded
// name, matching how VectorAdapter only special-cases x/y/z/w.
JSValue exoticGetOnlyX (JSContext* ctx, JSValueConst /*obj_val*/, JSAtom atom, JSValueConst /*receiver*/) {
    const char* name = JS_AtomToCString (ctx, atom);
    if (name != nullptr && strcmp (name, "x") == 0) {
	JS_FreeCString (ctx, name);
	return JS_NewFloat64 (ctx, 42.0);
    }
    if (name != nullptr) {
	JS_FreeCString (ctx, name);
    }

    // falls back to a real prototype lookup instead of throwing for anything not special-cased -
    // this is what lets obj.someMethod(...) reach a method defined on the class prototype.
    JSValue proto = JS_GetClassProto (ctx, g_exoticProtoClassId);
    JSValue result = JS_GetProperty (ctx, proto, atom);
    JS_FreeValue (ctx, proto);
    return result;
}
} // namespace

TEST_CASE ("An exotic get_property handler that only special-cases some names must still reach "
	   "prototype methods for everything else") {
    JSRuntime* rt = JS_NewRuntime ();
    JSContext* ctx = JS_NewContext (rt);

    JS_NewClassID (rt, &g_exoticProtoClassId);
    JSClassExoticMethods exotic = { .get_property = exoticGetOnlyX };
    JSClassDef classDef = { .class_name = "ExoticWithProtoMethod", .exotic = &exotic };
    JS_NewClass (rt, g_exoticProtoClassId, &classDef);

    JSValue proto = JS_NewObject (ctx);
    JS_SetPropertyStr (
	ctx, proto, "double",
	JS_NewCFunction (
	    ctx,
	    [] (JSContext* ctx, JSValueConst, int, JSValueConst*) -> JSValue { return JS_NewInt32 (ctx, 84); }, "double",
	    0
	)
    );
    JS_SetClassProto (ctx, g_exoticProtoClassId, proto);

    JSValue globalObj = JS_GetGlobalObject (ctx);
    JS_SetPropertyStr (
	ctx, globalObj, "make",
	JS_NewCFunction (
	    ctx,
	    [] (JSContext* ctx, JSValueConst, int, JSValueConst*) -> JSValue {
		JSValue obj = JS_NewObjectClass (ctx, g_exoticProtoClassId);
		JS_SetOpaque (obj, reinterpret_cast<void*> (1));
		return obj;
	    },
	    "make", 0
	)
    );
    JS_FreeValue (ctx, globalObj);

    // direct data property (x) still resolves via the exotic handler itself
    const char* xSource = "make().x";
    JSValue xResult = JS_Eval (ctx, xSource, strlen (xSource), "<x>", JS_EVAL_TYPE_GLOBAL);
    REQUIRE_FALSE (JS_IsException (xResult));
    double xVal = -1;
    JS_ToFloat64 (ctx, &xVal, xResult);
    CHECK (xVal == 42.0);
    JS_FreeValue (ctx, xResult);

    // a prototype method call must actually reach the method, not throw
    const char* methodSource = "make().double()";
    JSValue methodResult = JS_Eval (ctx, methodSource, strlen (methodSource), "<method>", JS_EVAL_TYPE_GLOBAL);
    REQUIRE_FALSE (JS_IsException (methodResult));
    int32_t methodVal = -1;
    JS_ToInt32 (ctx, &methodVal, methodResult);
    CHECK (methodVal == 84);
    JS_FreeValue (ctx, methodResult);

    JS_FreeContext (ctx);
    JS_FreeRuntime (rt);
}

// Mirrors the VectorAdapter/ScriptEngine teardown bug: a class finalizer calls back into an
// external C++ object (VectorAdapter::free()) to release state it owns. If that object is
// destroyed before JS_FreeRuntime() runs its GC pass, the finalizer for any still-live instance
// (e.g. a script's initialScale) calls into freed memory. The owning object must outlive
// JS_FreeRuntime(); only that order is exercised here.
namespace {
JSClassID g_ownedClassId = 0;

struct Adapter {
    int freedCount = 0;
    void free () { freedCount++; }
};

struct OwnedOpaque {
    Adapter& adapter;
};

void ownedFinalizer (JSRuntime* /*rt*/, JSValueConst val) {
    auto* opaque = static_cast<OwnedOpaque*> (JS_GetOpaque (val, g_ownedClassId));
    if (opaque != nullptr) {
	opaque->adapter.free ();
	delete opaque;
    }
}
} // namespace

TEST_CASE ("An owning adapter freed after JS_FreeRuntime sees its still-live instances finalized "
	   "safely") {
    JSRuntime* rt = JS_NewRuntime ();
    JSContext* ctx = JS_NewContext (rt);

    JS_NewClassID (rt, &g_ownedClassId);
    JSClassDef classDef = { .class_name = "Owned", .finalizer = ownedFinalizer };
    JS_NewClass (rt, g_ownedClassId, &classDef);

    auto adapter = std::make_unique<Adapter> ();

    JSValue instance = JS_NewObjectClass (ctx, g_ownedClassId);
    JS_SetOpaque (instance, new OwnedOpaque { .adapter = *adapter });

    // keep the instance reachable (mirrors a script holding onto a live Vec3) so it's still
    // around, not yet GC'd, when the runtime gets torn down below
    JSValue globalObj = JS_GetGlobalObject (ctx);
    JS_SetPropertyStr (ctx, globalObj, "kept", instance);
    JS_FreeValue (ctx, globalObj);

    // correct order: adapter is still alive here, so the finalizer's call back into it is safe
    JS_FreeContext (ctx);
    JS_FreeRuntime (rt);

    REQUIRE (adapter->freedCount == 1);
}
