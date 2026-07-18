//
// Created by yonglan.whl on 2021/7/14.
//
#include "quickjs_wrapper.h"
#include "../quickjs/cutils.h"
#include "quickjs_extend_libraries.h"
#include <cstring>
#include <cmath>

#define MAX_SAFE_INTEGER (((int64_t)1 << 53) - 1)

// util
static string getJavaName(JNIEnv *env, jobject javaClass)
{
    auto classType = env->GetObjectClass(javaClass);
    const auto method = env->GetMethodID(classType, "getName", "()Ljava/lang/String;");
    auto javaString = (jstring)(env->CallObjectMethod(javaClass, method));
    const auto s = env->GetStringUTFChars(javaString, nullptr);

    std::string str(s);
    env->ReleaseStringUTFChars(javaString, s);
    env->DeleteLocalRef(javaString);
    env->DeleteLocalRef(classType);
    return str;
}

// quickjs 没有提供 JS_IsArrayBuffer 方法，这里通过取巧的方式来实现，后续可以替换掉
static bool JS_IsArrayBuffer(JSValue value)
{
    // quickjs 里的 ArrayBuffer 对应的类型枚举�?
    int8_t JS_CLASS_ARRAY_BUFFER = 19;
    return JS_GetClassID(value) == JS_CLASS_ARRAY_BUFFER;
}

static void tryToTriggerOnError(JSContext *ctx, JSValueConst *error)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue onerror = JS_GetPropertyStr(ctx, global, "onError");
    if (JS_IsNull(onerror))
    {
        // may be lowercase
        onerror = JS_GetPropertyStr(ctx, global, "onerror");
    }

    if (JS_IsNull(onerror))
    {
        // do nothing
        return;
    }

    JS_Call(ctx, onerror, global, 1, error);
    JS_FreeValue(ctx, onerror);
    JS_FreeValue(ctx, global);
}

static string jsonEscape(const char *s) {
    if (!s) return "\"\"";
    string r;
    for (const char *p = s; *p; p++) {
        switch (*p) {
            case '\\': r += "\\\\"; break;
            case '"':  r += "\\\""; break;
            case '\n': r += "\\n"; break;
            case '\r': r += "\\r"; break;
            case '\t': r += "\\t"; break;
            default:   r += *p;
        }
    }
    return "\"" + r + "\"";
}

string QuickJSWrapper::getJSErrorStr(JSContext *ctx, JSValueConst error)
{
    JSValue val;
    bool is_error;
    is_error = JS_IsError(ctx, error);
    string result = "{";

    if (is_error)
    {
        tryToTriggerOnError(ctx, &error);

        // message
        val = JS_GetPropertyStr(ctx, error, "message");
        if (!JS_IsUndefined(val)) {
            const char *s = JS_ToCString(ctx, val);
            result += "\"message\":" + jsonEscape(s) + ",";
            JS_FreeCString(ctx, s);
        }
        JS_FreeValue(ctx, val);

        // fileName
        val = JS_GetPropertyStr(ctx, error, "fileName");
        if (!JS_IsUndefined(val)) {
            const char *s = JS_ToCString(ctx, val);
            result += "\"fileName\":" + jsonEscape(s) + ",";
            JS_FreeCString(ctx, s);
        }
        JS_FreeValue(ctx, val);

        // lineNumber
        val = JS_GetPropertyStr(ctx, error, "lineNumber");
        if (!JS_IsUndefined(val)) {
            double n; JS_ToFloat64(ctx, &n, val);
            result += "\"lineNumber\":" + std::to_string((int)n) + ",";
        }
        JS_FreeValue(ctx, val);

        // columnNumber
        val = JS_GetPropertyStr(ctx, error, "columnNumber");
        if (!JS_IsUndefined(val)) {
            double n; JS_ToFloat64(ctx, &n, val);
            result += "\"columnNumber\":" + std::to_string((int)n) + ",";
        }
        JS_FreeValue(ctx, val);

        // stack
        val = JS_GetPropertyStr(ctx, error, "stack");
        if (!JS_IsUndefined(val)) {
            const char *s = JS_ToCString(ctx, val);
            result += "\"stack\":" + jsonEscape(s) + ",";
            JS_FreeCString(ctx, s);
        }
        JS_FreeValue(ctx, val);

        result += "\"jsError\":true";
    }
    else
    {
        const char *s = JS_ToCString(ctx, error);
        result += "\"message\":" + jsonEscape(s) + ",\"jsError\":false";
        JS_FreeCString(ctx, s);
    }

    result += "}";
    return result;
}

string QuickJSWrapper::getJSErrorStr(JSContext *ctx)
{
    JSValue error = JS_GetException(ctx);
    string error_str = QuickJSWrapper::getJSErrorStr(ctx, error);
    JS_FreeValue(ctx, error);
    return error_str;
}

static void throwJavaException(JNIEnv *env, const char *exceptionClass, const char *fmt, ...)
{
    char msg[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    jclass e = env->FindClass(exceptionClass);
    env->ThrowNew(e, msg);
    env->DeleteLocalRef(e);
}

void QuickJSWrapper::throwJSException(JNIEnv *env, const char *msg)
{
    if (env->ExceptionCheck())
    {
        return;
    }

    jclass e = env->FindClass("com/whl/quickjs/wrapper/QuickJSException");
    jmethodID init = env->GetMethodID(e, "<init>", "(Ljava/lang/String;Z)V");
    jstring ret = env->NewStringUTF(msg);
    auto t = (jthrowable)env->NewObject(e, init, ret, JNI_TRUE);
    env->Throw(t);
    env->DeleteLocalRef(e);
}

void QuickJSWrapper::throwJSException(JNIEnv *env, JSContext *ctx)
{
    string error = QuickJSWrapper::getJSErrorStr(ctx);
    QuickJSWrapper::throwJSException(env, error.c_str());
}

// js function callback
static JSClassID js_func_callback_class_id;

static void jsFuncCallbackFinalizer(JSRuntime *rt, JSValue val)
{
    auto wrapper = reinterpret_cast<const QuickJSWrapper *>(JS_GetRuntimeOpaque(rt));
    if (wrapper)
    {
        int *callbackId = (int *)(JS_GetOpaque2(wrapper->context, val, js_func_callback_class_id));
        wrapper->removeCallFunction(*callbackId);
        delete callbackId;
    }
}

static JSClassDef js_func_callback_class = {
    "JSFuncCallback",
    .finalizer = jsFuncCallbackFinalizer,
};

static JSValue jsFnCallback(JSContext *ctx,
                            JSValueConst this_obj,
                            int argc, JSValueConst *argv,
                            int magic, JSValue *func_data)
{

    int callbackId = *((int *)JS_GetOpaque2(ctx, func_data[0], js_func_callback_class_id));
    auto wrapper = reinterpret_cast<QuickJSWrapper *>(JS_GetRuntimeOpaque(JS_GetRuntime(ctx)));
    JSValue value = wrapper->jsFuncCall(callbackId, this_obj, argc, argv);
    return value;
}

static void initJSFuncCallback(JSContext *ctx)
{
    // JSFuncCallback class
    JS_NewClassID(&js_func_callback_class_id);
    JS_NewClass(JS_GetRuntime(ctx), js_func_callback_class_id, &js_func_callback_class);
}

// js module
static char *jsModuleNormalizeFunc(JSContext *ctx, const char *module_base_name,
                                   const char *module_name, void *opaque)
{
    auto wrapper = reinterpret_cast<const QuickJSWrapper *>(JS_GetRuntimeOpaque(JS_GetRuntime(ctx)));
    auto env = wrapper->jniEnv;

    // module loader handle.
    jobject moduleLoader = env->CallObjectMethod(wrapper->jniThiz, env->GetMethodID(wrapper->quickjsContextClass, "getModuleLoader", "()Lcom/whl/quickjs/wrapper/ModuleLoader;"));
    if (moduleLoader == nullptr)
    {
        JS_ThrowInternalError(ctx, "Failed to load module, the ModuleLoader can not be null!");
        return nullptr;
    }
    jmethodID moduleNormalizeName = env->GetMethodID(wrapper->moduleLoaderClass, "moduleNormalizeName", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");

    jstring j_module_base_name = env->NewStringUTF(module_base_name);
    jstring j_module_name = env->NewStringUTF(module_name);
    auto result = env->CallObjectMethod(moduleLoader, moduleNormalizeName, j_module_base_name, j_module_name);
    if (result == nullptr)
    {
        QuickJSWrapper::throwJSException(env, "Failed to load module, cause moduleName was null!");
        return nullptr;
    }

    env->DeleteLocalRef(j_module_base_name);
    env->DeleteLocalRef(j_module_name);
    env->DeleteLocalRef(moduleLoader);

    // todo 这里作为返回值，没有调用 ReleaseStringUTFChars，quickjs.c 里面会对 char* 进行释放，需�?check 下是否有释放�?
    auto ret = (char *)env->GetStringUTFChars((jstring)result, nullptr);
    env->DeleteLocalRef(result);
    return ret;
}

static JSModuleDef *
jsModuleLoaderFunc(JSContext *ctx, const char *module_name, void *opaque)
{
    auto wrapper = reinterpret_cast<const QuickJSWrapper *>(JS_GetRuntimeOpaque(JS_GetRuntime(ctx)));
    auto env = wrapper->jniEnv;
    auto arg = env->NewStringUTF(module_name);

    // module loader handle.
    jobject moduleLoader = env->CallObjectMethod(wrapper->jniThiz, env->GetMethodID(wrapper->quickjsContextClass, "getModuleLoader", "()Lcom/whl/quickjs/wrapper/ModuleLoader;"));
    if (moduleLoader == nullptr)
    {
        JS_ThrowInternalError(ctx, "Failed to load module, the ModuleLoader can not be null!");
        return (JSModuleDef *)JS_VALUE_GET_PTR(JS_EXCEPTION);
    }

    bool isBytecodeModule = env->CallBooleanMethod(moduleLoader, env->GetMethodID(wrapper->moduleLoaderClass, "isBytecodeMode", "()Z"));

    void *m;
    if (isBytecodeModule)
    {
        jmethodID getModuleBytecode = env->GetMethodID(wrapper->moduleLoaderClass, "getModuleBytecode", "(Ljava/lang/String;)[B");

        auto bytecode = (jbyteArray)(env->CallObjectMethod(moduleLoader, getModuleBytecode, arg));
        if (bytecode == nullptr)
        {
            QuickJSWrapper::throwJSException(env, "Failed to load module, cause bytecode was null!");
            return nullptr;
        }

        const auto buffer = env->GetByteArrayElements(bytecode, nullptr);
        const auto bufferLength = env->GetArrayLength(bytecode);
        const auto flags = JS_READ_OBJ_BYTECODE | JS_READ_OBJ_REFERENCE;
        auto obj = JS_ReadObject(ctx, reinterpret_cast<const uint8_t *>(buffer), bufferLength, flags);
        env->ReleaseByteArrayElements(bytecode, buffer, JNI_ABORT);

        if (JS_IsException(obj))
        {
            QuickJSWrapper::throwJSException(env, ctx);
            return (JSModuleDef *)JS_VALUE_GET_PTR(JS_EXCEPTION);
        }

        if (JS_ResolveModule(ctx, obj))
        {
            QuickJSWrapper::throwJSException(env, "Failed to resolve JS module");
            return nullptr;
        }

        m = JS_VALUE_GET_PTR(obj);
        JS_FreeValue(ctx, obj);
        env->DeleteLocalRef(bytecode);
    }
    else
    {
        jmethodID getModuleStringCode = env->GetMethodID(wrapper->moduleLoaderClass, "getModuleStringCode", "(Ljava/lang/String;)Ljava/lang/String;");

        auto result = env->CallObjectMethod(moduleLoader, getModuleStringCode, arg);
        if (result == nullptr)
        {
            QuickJSWrapper::throwJSException(env, "Failed to load module, cause string code was null!");
            return nullptr;
        }

        const auto script = env->GetStringUTFChars((jstring)(result), JNI_FALSE);
        int scriptLen = env->GetStringUTFLength((jstring)result);
        JSValue func_val = JS_Eval(ctx, script, scriptLen, module_name,
                                   JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        env->ReleaseStringUTFChars((jstring)(result), script);
        if (JS_IsException(func_val))
        {
            JS_FreeValue(ctx, func_val);
            QuickJSWrapper::throwJSException(env, ctx);
            return (JSModuleDef *)JS_VALUE_GET_PTR(JS_EXCEPTION);
        }

        m = JS_VALUE_GET_PTR(func_val);
        JS_FreeValue(ctx, func_val);
    }

    env->DeleteLocalRef(arg);
    env->DeleteLocalRef(moduleLoader);
    return (JSModuleDef *)m;
}

static bool throwIfUnhandledRejections(QuickJSWrapper *wrapper, JSContext *ctx)
{
    string error;
    while (!wrapper->unhandledRejections.empty())
    {
        JSValueConst reason = wrapper->unhandledRejections.front();
        error += QuickJSWrapper::getJSErrorStr(ctx, reason);
        error += "\n";
        JS_FreeValue(ctx, reason);
        wrapper->unhandledRejections.pop();
    }

    bool is_error = !error.empty();
    if (is_error)
    {
        error = "UnhandledPromiseRejectionException: " + error;
        QuickJSWrapper::throwJSException(wrapper->jniEnv, error.c_str());
    }
    return is_error;
}

static bool executePendingJobLoop(JNIEnv *env, JSRuntime *rt, JSContext *ctx)
{
    if (env->ExceptionCheck())
    {
        return false;
    }

    JSContext *ctx1;
    bool success = true;
    int err;
    /* execute the pending jobs */
    for (;;)
    {
        err = JS_ExecutePendingJob(rt, &ctx1);
        if (err <= 0)
        {
            if (err < 0)
            {
                success = false;
                string error = QuickJSWrapper::getJSErrorStr(ctx);
                QuickJSWrapper::throwJSException(env, error.c_str());
            }
            break;
        }
    }

    if (success && throwIfUnhandledRejections(reinterpret_cast<QuickJSWrapper *>(JS_GetRuntimeOpaque(rt)), ctx))
    {
        success = false;
    }

    return success;
}

static void promiseRejectionTracker(JSContext *ctx, JSValueConst promise,
                                    JSValueConst reason, BOOL is_handled, void *opaque)
{
    auto unhandledRejections = static_cast<queue<JSValue> *>(opaque);
    if (!is_handled)
    {
        unhandledRejections->push(JS_DupValue(ctx, reason));
    }
    else
    {
        if (!unhandledRejections->empty())
        {
            JSValueConst rej = unhandledRejections->front();
            JS_FreeValue(ctx, rej);
            unhandledRejections->pop();
        }
    }
}

QuickJSWrapper::QuickJSWrapper(JNIEnv *env, jobject thiz, JSRuntime *rt)
{
    jniEnv = env;
    runtime = rt;
    jniThiz = jniEnv->NewGlobalRef(thiz);

    // init ES6Module
    JS_SetModuleLoaderFunc(runtime, jsModuleNormalizeFunc, jsModuleLoaderFunc, nullptr);

    JS_SetHostPromiseRejectionTracker(runtime, promiseRejectionTracker, &unhandledRejections);

    context = JS_NewContext(runtime);

    JS_SetRuntimeOpaque(runtime, this);
    initJSFuncCallback(context);
    loadExtendLibraries(context);

    const char *getOwnPropertyNames = "Object.getOwnPropertyNames";
    ownPropertyNames = JS_Eval(context, getOwnPropertyNames, strlen(getOwnPropertyNames), getOwnPropertyNames, JS_EVAL_TYPE_GLOBAL);

    objectClass = (jclass)(jniEnv->NewGlobalRef(jniEnv->FindClass("java/lang/Object")));
    booleanClass = (jclass)(jniEnv->NewGlobalRef(jniEnv->FindClass("java/lang/Boolean")));
    integerClass = (jclass)(jniEnv->NewGlobalRef(jniEnv->FindClass("java/lang/Integer")));
    longClass = (jclass)(jniEnv->NewGlobalRef(jniEnv->FindClass("java/lang/Long")));
    doubleClass = (jclass)(jniEnv->NewGlobalRef(jniEnv->FindClass("java/lang/Double")));
    stringClass = (jclass)(jniEnv->NewGlobalRef(jniEnv->FindClass("java/lang/String")));
    jsObjectClass = (jclass)(jniEnv->NewGlobalRef(jniEnv->FindClass("com/whl/quickjs/wrapper/JSObject")));
    jsArrayClass = (jclass)(jniEnv->NewGlobalRef(jniEnv->FindClass("com/whl/quickjs/wrapper/JSArray")));
    jsFunctionClass = (jclass)(jniEnv->NewGlobalRef(jniEnv->FindClass("com/whl/quickjs/wrapper/JSFunction")));
    jsCallFunctionClass = (jclass)(jniEnv->NewGlobalRef(jniEnv->FindClass("com/whl/quickjs/wrapper/JSCallFunction")));
    quickjsContextClass = (jclass)(jniEnv->NewGlobalRef(jniEnv->FindClass("com/whl/quickjs/wrapper/QuickJSContext")));
    moduleLoaderClass = (jclass)(jniEnv->NewGlobalRef(jniEnv->FindClass("com/whl/quickjs/wrapper/ModuleLoader")));
    creatorClass = (jclass)(jniEnv->NewGlobalRef(jniEnv->FindClass("com/whl/quickjs/wrapper/JSObjectCreator")));
    byteArrayClass = (jclass)jniEnv->NewGlobalRef(env->FindClass("[B"));

    booleanValueOf = jniEnv->GetStaticMethodID(booleanClass, "valueOf", "(Z)Ljava/lang/Boolean;");
    integerValueOf = jniEnv->GetStaticMethodID(integerClass, "valueOf", "(I)Ljava/lang/Integer;");
    longValueOf = jniEnv->GetStaticMethodID(longClass, "valueOf", "(J)Ljava/lang/Long;");
    doubleValueOf = jniEnv->GetStaticMethodID(doubleClass, "valueOf", "(D)Ljava/lang/Double;");

    booleanGetValue = jniEnv->GetMethodID(booleanClass, "booleanValue", "()Z");
    integerGetValue = jniEnv->GetMethodID(integerClass, "intValue", "()I");
    longGetValue = jniEnv->GetMethodID(longClass, "longValue", "()J");
    doubleGetValue = jniEnv->GetMethodID(doubleClass, "doubleValue", "()D");
    jsObjectGetValue = jniEnv->GetMethodID(jsObjectClass, "getPointer", "()J");

    callFunctionBackM = jniEnv->GetMethodID(quickjsContextClass, "callFunctionBack", "(I[Ljava/lang/Object;)Ljava/lang/Object;");
    removeCallFunctionM = jniEnv->GetMethodID(quickjsContextClass, "removeCallFunction", "(I)V");
    callFunctionHashCodeM = jniEnv->GetMethodID(objectClass, "hashCode", "()I");
    creatorM = jniEnv->GetMethodID(quickjsContextClass, "getCreator", "()Lcom/whl/quickjs/wrapper/JSObjectCreator;");
    newObjectM = jniEnv->GetMethodID(creatorClass, "newObject",
                                     "(Lcom/whl/quickjs/wrapper/QuickJSContext;J)Lcom/whl/quickjs/wrapper/JSObject;");
    newArrayM = jniEnv->GetMethodID(creatorClass, "newArray",
                                    "(Lcom/whl/quickjs/wrapper/QuickJSContext;J)Lcom/whl/quickjs/wrapper/JSArray;");
    newFunctionM = jniEnv->GetMethodID(creatorClass, "newFunction",
                                       "(Lcom/whl/quickjs/wrapper/QuickJSContext;JJI)Lcom/whl/quickjs/wrapper/JSFunction;");
}

QuickJSWrapper::~QuickJSWrapper()
{
    JS_FreeValue(context, ownPropertyNames);
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);

    jniEnv->DeleteGlobalRef(jniThiz);
    jniEnv->DeleteGlobalRef(objectClass);
    jniEnv->DeleteGlobalRef(doubleClass);
    jniEnv->DeleteGlobalRef(integerClass);
    jniEnv->DeleteGlobalRef(longClass);
    jniEnv->DeleteGlobalRef(booleanClass);
    jniEnv->DeleteGlobalRef(stringClass);
    jniEnv->DeleteGlobalRef(jsObjectClass);
    jniEnv->DeleteGlobalRef(jsArrayClass);
    jniEnv->DeleteGlobalRef(jsFunctionClass);
    jniEnv->DeleteGlobalRef(jsCallFunctionClass);
    jniEnv->DeleteGlobalRef(moduleLoaderClass);
    jniEnv->DeleteGlobalRef(quickjsContextClass);
    jniEnv->DeleteGlobalRef(creatorClass);
    jniEnv->DeleteGlobalRef(byteArrayClass);
}

jobject QuickJSWrapper::toJavaObject(JNIEnv *env, jobject thiz, JSValueConst this_obj, JSValueConst value) const
{
    jobject result;
    switch (JS_VALUE_GET_NORM_TAG(value))
    {
    case JS_TAG_EXCEPTION:
    {
        result = nullptr;
        break;
    }

    case JS_TAG_STRING:
    {
        result = toJavaString(env, value);
        break;
    }

    case JS_TAG_BOOL:
    {
        jvalue v;
        v.z = static_cast<jboolean>(JS_VALUE_GET_BOOL(value));
        result = env->CallStaticObjectMethodA(booleanClass, booleanValueOf, &v);
        break;
    }

    case JS_TAG_INT:
    {
        jvalue v;
        v.j = static_cast<jint>(JS_VALUE_GET_INT(value));
        result = env->CallStaticObjectMethodA(integerClass, integerValueOf, &v);
        break;
    }

    case JS_TAG_BIG_INT:
    {
        int64_t e;
        JS_ToBigInt64(context, &e, value);
        jvalue v;
        v.j = e;
        result = env->CallStaticObjectMethodA(longClass, longValueOf, &v);
        break;
    }

    case JS_TAG_FLOAT64:
    {
        jvalue v;
        double d = JS_VALUE_GET_FLOAT64(value);
        bool isInteger = floor(d) == d;
        if (isInteger)
        {
            v.j = static_cast<jlong>(d);
            result = env->CallStaticObjectMethodA(longClass, longValueOf, &v);
        }
        else
        {
            v.d = static_cast<jdouble>(d);
            result = env->CallStaticObjectMethodA(doubleClass, doubleValueOf, &v);
        }
        break;
    }

    case JS_TAG_OBJECT:
    {
        auto value_ptr = reinterpret_cast<jlong>(JS_VALUE_GET_PTR(value));
        jobject creatorObj = env->CallObjectMethod(thiz, creatorM);
        if (JS_IsFunction(context, value))
        {
            auto obj_ptr = reinterpret_cast<jlong>(JS_VALUE_GET_PTR(this_obj));
            result = env->CallObjectMethod(creatorObj, newFunctionM, thiz, value_ptr, obj_ptr, JS_VALUE_GET_TAG(this_obj));
        }
        else if (JS_IsArray(context, value))
        {
            result = env->CallObjectMethod(creatorObj, newArrayM, thiz, value_ptr);
        }
        else if (JS_IsArrayBuffer(value))
        {
            size_t byteLength = 0;
            uint8_t *buffer = JS_GetArrayBuffer(context, &byteLength, value);
            jbyteArray byteArray = env->NewByteArray(byteLength);
            void *elementsPtr = env->GetPrimitiveArrayCritical(byteArray, nullptr);
            jbyte *elements = reinterpret_cast<jbyte *>(elementsPtr);
            memcpy(elements, buffer, byteLength);
            result = byteArray;
            JS_FreeValue(context, value);
            env->ReleasePrimitiveArrayCritical(byteArray, elements, 0);
        }
        else
        {
            result = env->CallObjectMethod(creatorObj, newObjectM, thiz, value_ptr);
        }
        env->DeleteLocalRef(creatorObj);
        break;
    }

    default:
        result = nullptr;
        break;
    }

    return result;
}

jobject QuickJSWrapper::evaluate(JNIEnv *env, jobject thiz, jstring script, jstring file_name)
{
    const char *c_script = env->GetStringUTFChars(script, JNI_FALSE);
    const char *c_file_name = env->GetStringUTFChars(file_name, JNI_FALSE);

    JSValue result = JS_Eval(context, c_script, strlen(c_script), c_file_name, JS_EVAL_TYPE_GLOBAL);
    env->ReleaseStringUTFChars(script, c_script);
    env->ReleaseStringUTFChars(file_name, c_file_name);
    if (JS_IsException(result))
    {
        QuickJSWrapper::throwJSException(env, context);
        return nullptr;
    }

    if (!executePendingJobLoop(env, runtime, context))
    {
        JS_FreeValue(context, result);
        return nullptr;
    }

    return toJavaObject(env, thiz, JS_UNDEFINED, result);
}

jobject QuickJSWrapper::getGlobalObject(JNIEnv *env, jobject thiz) const
{
    JSValue value = JS_GetGlobalObject(context);

    auto value_ptr = reinterpret_cast<jlong>(JS_VALUE_GET_PTR(value));
    jobject result = env->CallObjectMethod(env->CallObjectMethod(thiz, creatorM), newObjectM, thiz, value_ptr);

    JS_FreeValue(context, value);
    return result;
}

jobject QuickJSWrapper::getProperty(JNIEnv *env, jobject thiz, jlong value, jstring name)
{
    JSValue jsObject = JS_MKPTR(JS_TAG_OBJECT, reinterpret_cast<void *>(value));

    const char *propsName = env->GetStringUTFChars(name, JNI_FALSE);
    JSValue propsValue = JS_GetPropertyStr(context, jsObject, propsName);
    env->ReleaseStringUTFChars(name, propsName);
    if (JS_IsException(propsValue))
    {
        QuickJSWrapper::throwJSException(env, context);
        return nullptr;
    }

    return toJavaObject(env, thiz, jsObject, propsValue);
}

jobject QuickJSWrapper::call(JNIEnv *env, jobject thiz, jlong func, jlong this_obj,
                             jint this_obj_tag, jobjectArray args)
{
    int argc = env->GetArrayLength(args);
    vector<JSValue> arguments;
    vector<JSValue> freeArguments;
    for (int numArgs = 0; numArgs < argc && !env->ExceptionCheck(); numArgs++)
    {
        jobject arg = env->GetObjectArrayElement(args, numArgs);
        auto jsArg = toJSValue(env, thiz, arg);
        if (JS_IsException(jsArg))
        {
            return nullptr;
        }

        // 基础类型(例如 string )�?Java callback 类型需要使用完 free.
        if (env->IsInstanceOf(arg, stringClass) || env->IsInstanceOf(arg, doubleClass) ||
            env->IsInstanceOf(arg, integerClass) || env->IsInstanceOf(arg, longClass) ||
            env->IsInstanceOf(arg, booleanClass) || env->IsInstanceOf(arg, jsCallFunctionClass) || env->IsInstanceOf(arg, byteArrayClass))
        {
            freeArguments.push_back(jsArg);
        }

        env->DeleteLocalRef(arg);

        arguments.push_back(jsArg);
    }

    JSValue jsObj = JS_MKPTR(this_obj_tag, reinterpret_cast<void *>(this_obj));
    JSValue jsFunc = JS_MKPTR(JS_TAG_OBJECT, reinterpret_cast<void *>(func));

    JSValue ret = JS_Call(context, jsFunc, jsObj, arguments.size(), arguments.data());
    if (JS_IsException(ret))
    {
        JS_FreeValue(context, ret);
        QuickJSWrapper::throwJSException(env, context);
        return nullptr;
    }

    for (JSValue argument : freeArguments)
    {
        JS_FreeValue(context, argument);
    }

    // release vector by swap.
    vector<JSValue>().swap(arguments);
    vector<JSValue>().swap(freeArguments);

    if (!executePendingJobLoop(env, runtime, context))
    {
        JS_FreeValue(context, ret);
        return nullptr;
    }

    return toJavaObject(env, thiz, jsObj, ret);
}

jstring QuickJSWrapper::jsonStringify(JNIEnv *env, jlong value) const
{
    JSValue obj = JS_JSONStringify(context, JS_MKPTR(JS_TAG_OBJECT, reinterpret_cast<void *>(value)), JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(obj))
    {
        QuickJSWrapper::throwJSException(env, context);
        return nullptr;
    }

    return toJavaString(env, obj);
}

jint QuickJSWrapper::length(JNIEnv *env, jlong value) const
{
    JSValue jsObj = JS_MKPTR(JS_TAG_OBJECT, reinterpret_cast<void *>(value));

    JSValue length = JS_GetPropertyStr(context, jsObj, "length");
    if (JS_IsException(length))
    {
        QuickJSWrapper::throwJSException(env, context);
        return -1;
    }

    JS_FreeValue(context, length);

    return JS_VALUE_GET_INT(length);
}

jobject QuickJSWrapper::get(JNIEnv *env, jobject thiz, jlong value, jint index)
{
    JSValue jsObj = JS_MKPTR(JS_TAG_OBJECT, reinterpret_cast<void *>(value));
    JSValue child = JS_GetPropertyUint32(context, jsObj, index);

    return toJavaObject(env, thiz, jsObj, child);
}

void QuickJSWrapper::set(JNIEnv *env, jobject thiz, jlong this_obj, jobject value, jint index)
{
    JSValue jsObj = JS_MKPTR(JS_TAG_OBJECT, reinterpret_cast<void *>(this_obj));
    JSValue child = toJSValue(env, thiz, value);
    if (JS_IsString(child))
    {
        // JSString 类型不需�?JS_DupValue
        JS_SetPropertyUint32(context, jsObj, index, child);
    }
    else
    {
        JS_SetPropertyUint32(context, jsObj, index, JS_DupValue(context, child));
    }
}

void QuickJSWrapper::setProperty(JNIEnv *env, jobject thiz, jlong this_obj, jstring name, jobject value) const
{
    const char *propName = env->GetStringUTFChars(name, JNI_FALSE);
    JSValue propValue = toJSValue(env, thiz, value);
    if (env->IsInstanceOf(value, jsObjectClass))
    {
        // 这里需要手动增加引用计数，不然 QuickJS 垃圾回收会报 assertion "p->ref_count > 0" 的错误�?
        JS_DupValue(context, propValue);
    }
    else if (env->IsInstanceOf(value, jsCallFunctionClass))
    {
        // 通过 JS_NewCFunctionData 创建�?fn 对象�?name 属性值被定义�?Empty 了，
        // 这里需要额外定义下，不�?js 层拿到的 fn.name 的值为�?
        JSAtom name_atom = JS_NewAtom(context, propName);
        JSAtom name_atom_key = JS_NewAtom(context, "name");
        JS_DefinePropertyValue(context, propValue, name_atom_key,
                               JS_AtomToString(context, name_atom), JS_PROP_CONFIGURABLE);
        JS_FreeAtom(context, name_atom);
        JS_FreeAtom(context, name_atom_key);
    }

    JSValue jsObj = JS_MKPTR(JS_TAG_OBJECT, reinterpret_cast<void *>(this_obj));
    JS_SetPropertyStr(context, jsObj, propName, propValue);

    env->ReleaseStringUTFChars(name, propName);
}

JSValue QuickJSWrapper::jsFuncCall(int callback_id, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (jniEnv->ExceptionCheck())
    {
        return JS_EXCEPTION;
    }

    jobjectArray javaArgs = jniEnv->NewObjectArray((jsize)argc, objectClass, nullptr);

    for (int i = 0; i < argc; i++)
    {
        JSValue v = JS_DupValue(context, argv[i]);
        auto java_arg = toJavaObject(jniEnv, jniThiz, this_val, v);
        jniEnv->SetObjectArrayElement(javaArgs, (jsize)i, java_arg);
        jniEnv->DeleteLocalRef(java_arg);
    }

    auto result = jniEnv->CallObjectMethod(jniThiz, callFunctionBackM, callback_id, javaArgs);

    jniEnv->DeleteLocalRef(javaArgs);

    JSValue jsValue = toJSValue(jniEnv, jniThiz, result);

    jniEnv->DeleteLocalRef(result);
    return jsValue;
}

void QuickJSWrapper::removeCallFunction(int callback_id) const
{
    if (jniEnv->ExceptionCheck())
    {
        return;
    }

    jniEnv->CallVoidMethod(jniThiz, removeCallFunctionM, callback_id);
}

JSValue QuickJSWrapper::toJSValue(JNIEnv *env, jobject thiz, jobject value) const
{
    if (value == nullptr)
    {
        return JS_NULL;
    }

    JSValue result;
    if (env->IsInstanceOf(value, stringClass))
    {
        const auto s = env->GetStringUTFChars((jstring)(value), JNI_FALSE);
        result = JS_NewString(context, s);
        env->ReleaseStringUTFChars((jstring)(value), s);
    }
    else if (env->IsInstanceOf(value, doubleClass))
    {
        result = JS_NewFloat64(context, env->CallDoubleMethod(value, doubleGetValue));
    }
    else if (env->IsInstanceOf(value, integerClass))
    {
        result = JS_NewInt32(context, env->CallIntMethod(value, integerGetValue));
    }
    else if (env->IsInstanceOf(value, longClass))
    {
        jlong l_val = env->CallLongMethod(value, longGetValue);
        if (l_val > MAX_SAFE_INTEGER || l_val < -MAX_SAFE_INTEGER)
        {
            result = JS_NewBigInt64(context, l_val);
        }
        else
        {
            result = JS_NewInt64(context, l_val);
        }
    }
    else if (env->IsInstanceOf(value, booleanClass))
    {
        result = JS_NewBool(context, env->CallBooleanMethod(value, booleanGetValue));
    }
    else if (env->IsInstanceOf(value, byteArrayClass))
    {
        jbyteArray bytes = static_cast<jbyteArray>(value);
        jbyte *byteData = env->GetByteArrayElements(bytes, nullptr);
        jsize length = env->GetArrayLength(bytes);
        result = JS_NewArrayBufferCopy(context, reinterpret_cast<uint8_t *>(byteData), length);
        env->ReleaseByteArrayElements(bytes, byteData, JNI_ABORT);
    }
    else if (env->IsInstanceOf(value, jsObjectClass))
    {
        result = JS_MKPTR(JS_TAG_OBJECT, reinterpret_cast<void *>(env->CallLongMethod(value, jsObjectGetValue)));
    }
    else if (env->IsInstanceOf(value, jsCallFunctionClass))
    {
        // 这里�?obj 是用来获�?JSFuncCallback 对象�?
        JSValue obj = JS_NewObjectClass(context, js_func_callback_class_id);
        result = JS_NewCFunctionData(context, jsFnCallback, 1, 0, 1, &obj);
        // JS_NewCFunctionData �?dupValue obj，这里需要对 obj 计数减一，保持计数平�?
        JS_FreeValue(context, obj);

        int *callbackId = new int(jniEnv->CallIntMethod(value, callFunctionHashCodeM));
        JS_SetOpaque(obj, callbackId);
    }
    else
    {
        auto classType = env->GetObjectClass(value);
        const auto typeName = getJavaName(env, classType);
        env->DeleteLocalRef(classType);
        // Throw an exception for unsupported argument type.
        throwJavaException(env, "java/lang/IllegalArgumentException", "Unsupported Java type %s",
                           typeName.c_str());
        result = JS_EXCEPTION;
    }

    return result;
}

void QuickJSWrapper::freeValue(jlong value) const
{
    JSValue jsObj = JS_MKPTR(JS_TAG_OBJECT, reinterpret_cast<void *>(value));
    JS_FreeValue(context, jsObj);
}

void QuickJSWrapper::dupValue(jlong value) const
{
    JSValue jsObj = JS_MKPTR(JS_TAG_OBJECT, reinterpret_cast<void *>(value));
    JS_DupValue(context, jsObj);
}

/**
 * @deprecated
 * See {@link freeValue(String)}
 * @param value
 */
void QuickJSWrapper::freeDupValue(jlong value) const
{
    JSValue jsObj = JS_MKPTR(JS_TAG_OBJECT, reinterpret_cast<void *>(value));
    JS_FreeValue(context, jsObj);
}

jobject QuickJSWrapper::parseJSON(JNIEnv *env, jobject thiz, jstring json)
{
    const char *c_json = env->GetStringUTFChars(json, JNI_FALSE);
    auto jsonObj = JS_ParseJSON(context, c_json, strlen(c_json), "parseJSON.js");
    if (JS_IsException(jsonObj))
    {
        QuickJSWrapper::throwJSException(env, context);
        return nullptr;
    }

    JSValue jsObj = JS_UNDEFINED;
    jobject result = toJavaObject(env, thiz, jsObj, jsonObj);
    env->ReleaseStringUTFChars(json, c_json);
    return result;
}

jbyteArray QuickJSWrapper::compile(JNIEnv *env, jstring source, jstring file_name, jboolean isModule) const
{
    const auto sourceCode = env->GetStringUTFChars(source, JNI_FALSE);
    const auto fileName = env->GetStringUTFChars(file_name, JNI_FALSE);
    auto eval_flags = JS_EVAL_FLAG_COMPILE_ONLY;
    if (isModule)
    {
        eval_flags = JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY;
    }
    auto compiled = JS_Eval(context, sourceCode, strlen(sourceCode), fileName, eval_flags);
    env->ReleaseStringUTFChars(source, sourceCode);
    env->ReleaseStringUTFChars(file_name, fileName);

    if (JS_IsException(compiled))
    {
        QuickJSWrapper::throwJSException(env, context);
        return nullptr;
    }

    size_t bufferLength = 0;
    auto buffer = JS_WriteObject(context, &bufferLength, compiled, JS_WRITE_OBJ_BYTECODE | JS_WRITE_OBJ_REFERENCE);

    auto result = buffer && bufferLength > 0 ? env->NewByteArray(bufferLength) : nullptr;
    if (result)
    {
        env->SetByteArrayRegion(result, 0, bufferLength, reinterpret_cast<const jbyte *>(buffer));
    }
    else
    {
        QuickJSWrapper::throwJSException(env, context);
    }

    JS_FreeValue(context, compiled);
    js_free(context, buffer);

    return result;
}

jobject QuickJSWrapper::execute(JNIEnv *env, jobject thiz, jbyteArray bytecode)
{
    if (bytecode == nullptr)
    {
        QuickJSWrapper::throwJSException(env, "bytecode can not be null");
        return nullptr;
    }

    const auto buffer = env->GetByteArrayElements(bytecode, nullptr);
    const auto bufferLength = env->GetArrayLength(bytecode);
    const auto flags = JS_READ_OBJ_BYTECODE | JS_READ_OBJ_REFERENCE;
    auto obj = JS_ReadObject(context, reinterpret_cast<const uint8_t *>(buffer), bufferLength, flags);
    env->ReleaseByteArrayElements(bytecode, buffer, JNI_ABORT);

    if (JS_IsException(obj))
    {
        QuickJSWrapper::throwJSException(env, context);
        return nullptr;
    }

    if (JS_ResolveModule(context, obj))
    {
        // TODO throwJsExceptionFmt(env, this, "Failed to resolve JS module");
        return nullptr;
    }

    auto val = JS_EvalFunction(context, obj);

    if (!executePendingJobLoop(env, runtime, context))
    {
        JS_FreeValue(context, val);
        return nullptr;
    }

    jobject result;
    if (!JS_IsException(val))
    {
        result = toJavaObject(env, thiz, JS_UNDEFINED, val);
    }
    else
    {
        result = nullptr;
        QuickJSWrapper::throwJSException(env, context);
    }

    return result;
}

jobject
QuickJSWrapper::evaluateModule(JNIEnv *env, jobject thiz, jstring script, jstring file_name)
{
    const char *c_script = env->GetStringUTFChars(script, JNI_FALSE);
    const char *c_file_name = env->GetStringUTFChars(file_name, JNI_FALSE);

    JSValue result = JS_Eval(context, c_script, strlen(c_script), c_file_name, JS_EVAL_TYPE_MODULE);
    env->ReleaseStringUTFChars(script, c_script);
    env->ReleaseStringUTFChars(file_name, c_file_name);
    if (JS_IsException(result))
    {
        QuickJSWrapper::throwJSException(env, context);
        return nullptr;
    }

    if (!executePendingJobLoop(env, runtime, context))
    {
        JS_FreeValue(context, result);
        return nullptr;
    }

    JSValue global = JS_GetGlobalObject(context);
    jobject jsObj = toJavaObject(env, thiz, global, result);
    JS_FreeValue(context, global);
    return jsObj;
}

jobject QuickJSWrapper::getOwnPropertyNames(JNIEnv *env, jobject thiz, jlong obj)
{
    if (JS_IsException(ownPropertyNames))
    {
        QuickJSWrapper::throwJSException(env, context);
        return nullptr;
    }

    JSValue jsObject = JS_MKPTR(JS_TAG_OBJECT, reinterpret_cast<void *>(obj));
    JSValue ret = JS_Call(context, ownPropertyNames, JS_NULL, 1, &jsObject);
    if (JS_IsException(ret))
    {
        QuickJSWrapper::throwJSException(env, context);
        JS_FreeValue(context, ret);
        return nullptr;
    }

    return toJavaObject(env, thiz, JS_UNDEFINED, ret);
}

jstring QuickJSWrapper::toJavaString(JNIEnv *env, JSValue value) const
{
    jstring result;
#ifdef IS_ANDROID
    const char *string = JS_ToCString(context, value);
    result = env->NewStringUTF(string);
    JS_FreeCString(context, string);
    // JSString 类型�?JSValue 需要手动释放掉，不然会泄漏
    JS_FreeValue(context, value);
#else
    // 这里需要注意，JVM 平台�?NewStringUTF 方法对部�?unicode 的转换有问题，会出现乱码，换了另一种方式解决�?
    const char *str;
    size_t len;
    str = JS_ToCStringLen(context, &len, value);

    jbyteArray jba = env->NewByteArray(len);
    env->SetByteArrayRegion(jba, 0, len, reinterpret_cast<const jbyte *>(str));

    result = static_cast<jstring>(env->NewObject(stringClass,
                                                 env->GetMethodID(stringClass, "<init>", "([B)V"),
                                                 jba));

    JS_FreeCString(context, str);
    env->DeleteLocalRef(jba);
    // JSString 类型�?JSValue 需要手动释放掉，不然会泄漏
    JS_FreeValue(context, value);
#endif

    return result;
}

// ── Yeow buffer transport ─────────────────────────────────────────
void QuickJSWrapper::initBuffer(void *addr, uint32_t size) {
    bufPtr = addr;
    bufSize = size;

    JSValue global = JS_GetGlobalObject(context);
    JSValue wb = JS_NewCFunction(context, yeowWriteBuffer, "$_writeBuffer", 1);
    JS_SetPropertyStr(context, global, "$_writeBuffer", wb);

    JSValue rb = JS_NewCFunction(context, yeowReadBuffer, "$_readBuffer", 0);
    JS_SetPropertyStr(context, global, "$_readBuffer", rb);

    JS_FreeValue(context, global);
}

static uint8_t readU8(void *buf, uint32_t off) { return *(uint8_t *)((uint8_t *)buf + off); }
static void writeU8(void *buf, uint32_t off, uint8_t v) { *(uint8_t *)((uint8_t *)buf + off) = v; }
static double readF64(void *buf, uint32_t off) { double v; memcpy(&v, (uint8_t *)buf + off, 8); return v; }
static void writeF64(void *buf, uint32_t off, double v) { memcpy((uint8_t *)buf + off, &v, 8); }

uint64_t QuickJSWrapper::fnv1a(const char *s, size_t len) {
    uint64_t h = 0xCBF29CE484222325ULL;
    for (size_t i = 0; i < len; i++)
        h = (h ^ (unsigned char)s[i]) * 0x100000001B3ULL;
    return h;
}

#define HT_SIZE  17
#define HT_OFF   0
#define NUM_OFF  153
#define STR_OFF  217
#define BOL_OFF  2025
#define CNT_OFF  2030
#define MAX_NUM  8
#define MAX_STR  4
#define MAX_BOL  5
#define STR_MAX  450

// pos encoding: high 2 bits = type (0=num,1=str,2=bol), low 6 bits = slot index
static uint8_t makePos(int type, int idx) { return (uint8_t)((type << 6) | idx); }
static int posType(uint8_t p) { return (p >> 6) & 3; }
static int posIdx(uint8_t p) { return p & 0x3F; }

// $_writeBuffer(obj) → true/false
JSValue QuickJSWrapper::yeowWriteBuffer(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
    auto *self = (QuickJSWrapper *)JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    void *buf = self->bufPtr;
    if (!buf || argc < 1) return JS_FALSE;

    // Zero out hash table
    memset(buf, 0, HT_SIZE * 9);

    // Get keys array
    JSValue keys = JS_GetPropertyStr(ctx, argv[0], "length");
    uint32_t keyCount = JS_IsNumber(keys) ? (uint32_t)JS_VALUE_GET_INT(keys) : 0;
    JS_FreeValue(ctx, keys);

    keys = JS_GetPropertyStr(ctx, argv[0], "keys");
    keys = JS_Call(ctx, keys, argv[0], 0, nullptr);
    keys = JS_Call(ctx, keys, JS_UNDEFINED, 0, nullptr); // Array.from(keys()) via ...spread

    // Actually get keys via Object.keys
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue objKeysFn = JS_GetPropertyStr(ctx, g, "Object");
    objKeysFn = JS_GetPropertyStr(ctx, objKeysFn, "keys");
    keys = JS_Call(ctx, objKeysFn, JS_UNDEFINED, 1, &argv[0]);
    JS_FreeValue(ctx, objKeysFn);
    JS_FreeValue(ctx, g);

    keyCount = JS_VALUE_GET_INT(JS_GetPropertyStr(ctx, keys, "length"));
    if (keyCount > HT_SIZE) { JS_FreeValue(ctx, keys); return JS_FALSE; }

    int nIdx = 0, sIdx = 0, bIdx = 0;

    for (uint32_t i = 0; i < keyCount; i++) {
        JSValue k = JS_GetPropertyUint32(ctx, keys, i);
        const char *keyStr = JS_ToCString(ctx, k);
        size_t keyLen = strlen(keyStr);
        uint64_t hash = fnv1a(keyStr, keyLen);

        JSValue v = JS_GetPropertyStr(ctx, argv[0], keyStr);
        JS_FreeCString(ctx, keyStr);

        int type = -1, idx = -1;

        if (JS_IsNumber(v)) { type = 0;
            if (nIdx >= MAX_NUM) { JS_FreeValue(ctx, v); JS_FreeValue(ctx, k); JS_FreeValue(ctx, keys); return JS_FALSE; }
            double d; JS_ToFloat64(ctx, &d, v);
            writeF64(buf, (uint32_t)(NUM_OFF + nIdx * 8), d);
            idx = nIdx++;
        } else if (JS_IsString(v)) {
            size_t slen; const char *s = JS_ToCStringLen(ctx, &slen, v);
            if (sIdx >= MAX_STR || slen > STR_MAX) { JS_FreeCString(ctx, s); JS_FreeValue(ctx, v); JS_FreeValue(ctx, k); JS_FreeValue(ctx, keys); return JS_FALSE; }
            writeU8(buf, (uint32_t)(STR_OFF + sIdx * (2 + STR_MAX)), (uint8_t)((slen >> 8) & 0xFF));
            writeU8(buf, (uint32_t)(STR_OFF + sIdx * (2 + STR_MAX) + 1), (uint8_t)(slen & 0xFF));
            memcpy((uint8_t *)buf + STR_OFF + sIdx * (2 + STR_MAX) + 2, s, slen);
            JS_FreeCString(ctx, s);
            idx = sIdx++;
        } else if (JS_IsBool(v)) {
            if (bIdx >= MAX_BOL) { JS_FreeValue(ctx, v); JS_FreeValue(ctx, k); JS_FreeValue(ctx, keys); return JS_FALSE; }
            writeU8(buf, (uint32_t)(BOL_OFF + bIdx), JS_ToBool(ctx, v) ? 1 : 0);
            idx = bIdx++;
        } else { JS_FreeValue(ctx, v); JS_FreeValue(ctx, k); JS_FreeValue(ctx, keys); return JS_FALSE; }

        // Find empty slot in hash table
        int slot = -1;
        for (int j = 0; j < HT_SIZE; j++) {
            uint64_t h = readF64(buf, (uint32_t)(HT_OFF + j * 9)); // read as double to get 8 bytes
            // Re-read as uint64 via memcpy
            uint64_t existing;
            memcpy(&existing, (uint8_t *)buf + HT_OFF + j * 9, 8);
            if (existing == 0) { slot = j; break; }
        }
        if (slot < 0) { JS_FreeValue(ctx, v); JS_FreeValue(ctx, k); JS_FreeValue(ctx, keys); return JS_FALSE; }

        memcpy((uint8_t *)buf + HT_OFF + slot * 9, &hash, 8);
        writeU8(buf, (uint32_t)(HT_OFF + slot * 9 + 8), makePos(type, idx));
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, k);
    }

    writeU8(buf, CNT_OFF, (uint8_t)nIdx);
    writeU8(buf, CNT_OFF + 1, (uint8_t)sIdx);
    writeU8(buf, CNT_OFF + 2, (uint8_t)bIdx);
    JS_FreeValue(ctx, keys);
    return JS_TRUE;
}

// $_readBuffer() → {get:fn, has:fn, _hashTable:[], _nums:[], _strs:[], _bools:[]}
JSValue QuickJSWrapper::yeowReadBuffer(JSContext *ctx, JSValueConst this_val,
                                        int argc, JSValueConst *argv) {
    auto *self = (QuickJSWrapper *)JS_GetRuntimeOpaque(JS_GetRuntime(ctx));
    void *buf = self->bufPtr;
    if (!buf) return JS_NULL;

    // Build the script to create a result object with get/has methods
    // We embed the raw data as literal arrays in the script
    uint32_t nc = readU8(buf, CNT_OFF);
    uint32_t sc = readU8(buf, CNT_OFF + 1);
    uint32_t bc = readU8(buf, CNT_OFF + 2);

    // Read hash table
    std::string numsArr = "[";
    for (uint32_t i = 0; i < nc; i++) {
        if (i > 0) numsArr += ",";
        double d = readF64(buf, (uint32_t)(NUM_OFF + i * 8));
        char buf64[64];
        snprintf(buf64, sizeof(buf64), "%.17g", d);
        numsArr += buf64;
    }
    numsArr += "]";

    std::string strsArr = "[";
    for (uint32_t i = 0; i < sc; i++) {
        if (i > 0) strsArr += ",";
        uint32_t off = (uint32_t)(STR_OFF + i * (2 + STR_MAX));
        uint32_t len = ((uint32_t)readU8(buf, off) << 8) | readU8(buf, off + 1);
        strsArr += "\"";
        // Escape string for JS literal
        auto *data = (uint8_t *)buf + off + 2;
        for (uint32_t j = 0; j < len; j++) {
            char c = (char)data[j];
            if (c == '"' || c == '\\') { strsArr += '\\'; strsArr += c; }
            else if (c == '\n') strsArr += "\\n";
            else if (c == '\r') strsArr += "\\r";
            else if (c == '\t') strsArr += "\\t";
            else if ((unsigned char)c < 32) { char bufE[8]; snprintf(bufE, 8, "\\x%02x", (unsigned char)c); strsArr += bufE; }
            else strsArr += c;
        }
        strsArr += "\"";
    }
    strsArr += "]";

    std::string boolsArr = "[";
    for (uint32_t i = 0; i < bc; i++) {
        if (i > 0) boolsArr += ",";
        boolsArr += readU8(buf, (uint32_t)(BOL_OFF + i)) ? "true" : "false";
    }
    boolsArr += "]";

    // Build hash table array: [hash0,hash1,...]
    std::string htArr = "[";
    for (int i = 0; i < HT_SIZE; i++) {
        if (i > 0) htArr += ",";
        uint64_t h; memcpy(&h, (uint8_t *)buf + HT_OFF + i * 9, 8);
        uint8_t p = readU8(buf, (uint32_t)(HT_OFF + i * 9 + 8));
        // Encode as [hash, pos]
        char bufH[32];
        if (h == 0) { htArr += "0"; }
        else {
            // Use a string trick: encode as base-10 string for BigInt
            // Actually for JS number-safe range check
            if (h <= 0x1FFFFFFFFFFFFFULL) {
                snprintf(bufH, 32, "%llu", (unsigned long long)h);
                htArr += bufH;
            } else {
                // Use two-number encoding: high and low 32 bits
                snprintf(bufH, 32, "[%u,%u]", (unsigned)(h >> 32), (unsigned)(h & 0xFFFFFFFF));
                htArr += bufH;
            }
        }
    }
    htArr += "]";

    // Build pos array
    std::string posArr = "[";
    for (int i = 0; i < HT_SIZE; i++) {
        if (i > 0) posArr += ",";
        uint8_t p = readU8(buf, (uint32_t)(HT_OFF + i * 9 + 8));
        char bufP[8]; snprintf(bufP, 8, "%u", p);
        posArr += bufP;
    }
    posArr += "]";

    // Create result via eval
    std::string script = "(()=>{";
    script += "var _ht=" + htArr + ";";
    script += "var _pos=" + posArr + ";";
    script += "var _nums=" + numsArr + ";";
    script += "var _strs=" + strsArr + ";";
    script += "var _bools=" + boolsArr + ";";
    script += "var C=" + std::to_string(HT_SIZE) + ";";
    script += "var r={";
    script += "_raw:[_nums,_strs,_bools],";
    script += "get:function(k){";
    script += "var h=0xCBF29CE484222325n;";
    script += "for(var i=0;i<k.length;i++)h=BigInt.asUintN(64,(h^BigInt(k.charCodeAt(i)))*0x100000001B3n);";
    script += "var hn=Number(h&0x1FFFFFFFFFFFFFn);";
    script += "for(var i=0;i<C;i++){if(_ht[i]===0)continue;var m=_ht[i];if(typeof m==='number'?m===hn:m[0]===Number(h>>64n)&&m[1]===Number(h&0xFFFFFFFFn)){";
    script += "var t=_pos[i]>>6;var idx=_pos[i]&0x3F;";
    script += "if(t===0)return _nums[idx];if(t===1)return _strs[idx];return _bools[idx];}}return undefined;";
    script += "},";
    script += "has:function(k){return this.get(k)!==undefined;},";
    script += "keys:function(){var ks=[];for(var i=0;i<C;i++)if(_ht[i]!==0)ks.push('?')return ks;}";
    script += "};return r;})()";

    JSValue result = JS_Eval(ctx, script.c_str(), script.length(), "<buf>", JS_EVAL_TYPE_GLOBAL);
    return result;
}
