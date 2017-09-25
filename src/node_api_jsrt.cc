/******************************************************************************
* Experimental prototype for demonstrating VM agnostic and ABI stable API
* for native modules to use instead of using Nan and V8 APIs directly.
*
* The current status is "Experimental" and should not be used for
* production applications.  The API is still subject to change
* and as an experimental feature is NOT subject to semver.
*
******************************************************************************/

#include <stddef.h>
#include <node_api.h>
#include <node_buffer.h>
#include <node_object_wrap.h>
#include <env.h>
#include <array>
#include <vector>
#include <algorithm>
#include "ChakraCore.h"

#include "src/jsrtutils.h"

// Forward declare a dependency on an internal node helper
// This is so that we don't take an additional dependency on
// the chakrashim (v8::trycatch) and so that node doesn't
// have to expose another public API
namespace node {
  extern void FatalException(v8::Isolate* isolate,
    v8::Local<v8::Value> error,
    v8::Local<v8::Message> message);
}

#ifndef CALLBACK
#define CALLBACK
#endif

#define RETURN_STATUS_IF_FALSE(condition, status)                       \
  do {                                                                  \
    if (!(condition)) {                                                 \
      return napi_set_last_error((status));                             \
    }                                                                   \
  } while (0)

#define CHECK_ENV(env)        \
  if ((env) == nullptr) {     \
    return napi_invalid_arg;  \
  }

#define CHECK_ARG(arg)                                                  \
  RETURN_STATUS_IF_FALSE((arg), napi_invalid_arg)

#define CHECK_JSRT(expr)                                                \
  do {                                                                  \
    JsErrorCode err = (expr);                                           \
    if (err != JsNoError) return napi_set_last_error(err);              \
  } while (0)

#define CHECK_JSRT_EXPECTED(expr, expected)                             \
  do {                                                                  \
    JsErrorCode err = (expr);                                           \
    if (err == JsErrorInvalidArgument)                                  \
      return napi_set_last_error(expected);                             \
    if (err != JsNoError) return napi_set_last_error(err);              \
  } while (0)

// This does not call napi_set_last_error because the expression
// is assumed to be a NAPI function call that already did.
#define CHECK_NAPI(expr)                                                \
  do {                                                                  \
    napi_status status = (expr);                                        \
    if (status != napi_ok) return status;                               \
  } while (0)


// utf8 multibyte codepoint start check
#define UTF8_MULTIBYTE_START(c) (((c) & 0xC0) == 0xC0)

static napi_status napi_set_last_error(napi_status error_code,
                                       uint32_t engine_error_code = 0,
                                       void* engine_reserved = nullptr);
static napi_status napi_set_last_error(JsErrorCode jsError,
                                       void* engine_reserved = nullptr);
static void napi_clear_last_error();

// Callback Info struct as per JSRT native function.
struct CallbackInfo {
  napi_value newTarget;
  napi_value thisArg;
  uint16_t argc;
  napi_value* argv;
  void* data;
};

struct napi_env__ {
  explicit napi_env__(v8::Isolate* _isolate): isolate(_isolate) {}
  ~napi_env__() {}
  v8::Isolate* isolate;
};

namespace {
namespace v8impl {

//=== Conversion between V8 Isolate and napi_env ==========================

napi_env JsEnvFromV8Isolate(v8::Isolate* isolate) {
  return reinterpret_cast<napi_env>(isolate);
}

v8::Isolate* V8IsolateFromJsEnv(napi_env e) {
  return reinterpret_cast<v8::Isolate*>(e);
}

class HandleScopeWrapper {
 public:
  explicit HandleScopeWrapper(
    v8::Isolate* isolate) : scope(isolate) { }
 private:
  v8::HandleScope scope;
};

class EscapableHandleScopeWrapper {
 public:
  explicit EscapableHandleScopeWrapper(
    v8::Isolate* isolate) : scope(isolate) { }
  template <typename T>
  v8::Local<T> Escape(v8::Local<T> handle) {
    return scope.Escape(handle);
  }
 private:
  v8::EscapableHandleScope scope;
};

napi_handle_scope JsHandleScopeFromV8HandleScope(HandleScopeWrapper* s) {
  return reinterpret_cast<napi_handle_scope>(s);
}

HandleScopeWrapper* V8HandleScopeFromJsHandleScope(napi_handle_scope s) {
  return reinterpret_cast<HandleScopeWrapper*>(s);
}

napi_escapable_handle_scope
  JsEscapableHandleScopeFromV8EscapableHandleScope(
    EscapableHandleScopeWrapper* s) {
  return reinterpret_cast<napi_escapable_handle_scope>(s);
}

EscapableHandleScopeWrapper*
  V8EscapableHandleScopeFromJsEscapableHandleScope(
    napi_escapable_handle_scope s) {
  return reinterpret_cast<EscapableHandleScopeWrapper*>(s);
}

//=== Conversion between V8 Handles and napi_value ========================

// This is assuming v8::Local<> will always be implemented with a single
// pointer field so that we can pass it around as a void* (maybe we should
// use intptr_t instead of void*)

napi_value JsValueFromV8LocalValue(v8::Local<v8::Value> local) {
  return reinterpret_cast<napi_value>(*local);
}

v8::Local<v8::Value> V8LocalValueFromJsValue(napi_value v) {
  v8::Local<v8::Value> local;
  memcpy(&local, &v, sizeof(v));
  return local;
}
}  // end of namespace v8impl

namespace jsrtimpl {

// Adapter for JSRT external data + finalize callback.
class ExternalData {
 public:
  ExternalData(napi_env env,
                void* data,
                napi_finalize finalize_cb,
                void* hint)
    : _env(env),
      _data(data),
      _cb(finalize_cb),
      _hint(hint) {
  }

  void* Data() {
    return _data;
  }

  // JsFinalizeCallback
  static void CALLBACK Finalize(void* callbackState) {
    ExternalData* externalData =
      reinterpret_cast<ExternalData*>(callbackState);
    if (externalData != nullptr) {
      if (externalData->_cb != nullptr) {
        externalData->_cb(
          externalData->_env, externalData->_data, externalData->_hint);
      }

      delete externalData;
    }
  }

  // node::Buffer::FreeCallback
  static void FinalizeBuffer(char* data, void* callbackState) {
    Finalize(callbackState);
  }

 private:
  napi_env _env;
  void* _data;
  napi_finalize _cb;
  void* _hint;
};

// Adapter for JSRT external callback + callback data.
class ExternalCallback {
 public:
  ExternalCallback(napi_env env, napi_callback cb, void* data)
    : _env(env), _cb(cb), _data(data) {
  }

  // JsNativeFunction
  static JsValueRef CALLBACK Callback(JsValueRef callee,
                                      bool isConstructCall,
                                      JsValueRef *arguments,
                                      uint16_t argumentCount,
                                      void *callbackState) {
    jsrtimpl::ExternalCallback* externalCallback =
      reinterpret_cast<jsrtimpl::ExternalCallback*>(callbackState);

    // Make sure any errors encountered last time we were in N-API are gone.
    napi_clear_last_error();

    CallbackInfo cbInfo;
    cbInfo.thisArg = reinterpret_cast<napi_value>(arguments[0]);

    // TODO(digitalinfinity): This is incorrect as per spec, but
    // implementing this behavior for now to at least handle the case
    // where folks check newTarget != null to determine they're in a
    // constructor.
    // JSRT will need to implement an API to get new.target for this
    // to work correctly
    cbInfo.newTarget = (isConstructCall ? cbInfo.thisArg : nullptr);
    cbInfo.argc = argumentCount - 1;
    cbInfo.argv = reinterpret_cast<napi_value*>(arguments + 1);
    cbInfo.data = externalCallback->_data;

    napi_value result = externalCallback->_cb(
      externalCallback->_env, reinterpret_cast<napi_callback_info>(&cbInfo));
    return reinterpret_cast<JsValueRef>(result);
  }

  // JsObjectBeforeCollectCallback
  static void CALLBACK Finalize(JsRef ref, void* callbackState) {
    jsrtimpl::ExternalCallback* externalCallback =
      reinterpret_cast<jsrtimpl::ExternalCallback*>(callbackState);
    delete externalCallback;
  }

 private:
  napi_env _env;
  napi_callback _cb;
  void* _data;
};

class StringUtf8 {
 public:
  StringUtf8() : _str(nullptr), _length(0) {}
  ~StringUtf8() {
    if (_str != nullptr) {
      free(_str);
      _str = nullptr;
      _length = 0;
    }
  }
  char *operator*() { return _str; }
  operator const char *() const { return _str; }
  int length() const { return static_cast<int>(_length); }

  napi_status From(JsValueRef strRef) {
    CHAKRA_ASSERT(!_str);

    size_t length = 0;
    CHECK_JSRT_EXPECTED(JsCopyString(strRef, nullptr, 0, &length),
                        napi_string_expected);

    _str = reinterpret_cast<char*>(malloc(length + 1));
    CHAKRA_VERIFY(_str != nullptr);

    CHECK_JSRT_EXPECTED(
      JsCopyString(strRef, _str, length, nullptr),
                   napi_string_expected);

    _str[length] = '\0';
    _length = length;
    return napi_ok;
  }

 private:
  // Disallow copying and assigning
  StringUtf8(const StringUtf8&);
  void operator=(const StringUtf8&);

  char* _str;
  int _length;
};

inline napi_status JsPropertyIdFromKey(JsValueRef key,
                                       JsPropertyIdRef* propertyId) {
  JsValueType keyType;
  CHECK_JSRT(JsGetValueType(key, &keyType));

  if (keyType == JsString) {
    StringUtf8 str;
    CHECK_NAPI(str.From(key));

    CHECK_JSRT(JsCreatePropertyId(*str, str.length(), propertyId));
  } else if (keyType == JsSymbol) {
    CHECK_JSRT(JsGetPropertyIdFromSymbol(key, propertyId));
  } else {
    return napi_set_last_error(napi_name_expected);
  }
  return napi_ok;
}

inline napi_status
JsPropertyIdFromPropertyDescriptor(const napi_property_descriptor* p,
                                   JsPropertyIdRef* propertyId) {
  if (p->utf8name != nullptr) {
    CHECK_JSRT(JsCreatePropertyId(
      p->utf8name, strlen(p->utf8name), propertyId));
    return napi_ok;
  } else {
    return JsPropertyIdFromKey(p->name, propertyId);
  }
}

inline napi_status
JsNameValueFromPropertyDescriptor(const napi_property_descriptor* p,
                                  napi_value* name) {
  if (p->utf8name != nullptr) {
    CHECK_JSRT(JsCreateString(
      p->utf8name,
      strlen(p->utf8name),
      reinterpret_cast<JsValueRef*>(name)));
  } else {
    *name = p->name;
  }
  return napi_ok;
}

inline napi_status FindWrapper(JsValueRef obj, JsValueRef* wrapper,
                               JsValueRef* parent = nullptr) {
  // Search the object's prototype chain for the wrapper with external data.
  // Usually the wrapper would be the first in the chain, but it is OK for
  // other objects to be inserted in the prototype chain.
  JsValueRef candidate = obj;
  JsValueRef current = JS_INVALID_REFERENCE;
  bool hasExternalData = false;

  JsValueRef nullValue = JS_INVALID_REFERENCE;
  CHECK_JSRT(JsGetNullValue(&nullValue));

  do {
    current = candidate;

    CHECK_JSRT(JsGetPrototype(current, &candidate));
    if (candidate == JS_INVALID_REFERENCE || candidate == nullValue) {
      if (parent != nullptr) {
        *parent = JS_INVALID_REFERENCE;
      }

      *wrapper = JS_INVALID_REFERENCE;
      return napi_ok;
    }

    CHECK_JSRT(JsHasExternalData(candidate, &hasExternalData));
  } while (!hasExternalData);

  if (parent != nullptr) {
    *parent = current;
  }

  *wrapper = candidate;

  return napi_ok;
}

inline napi_status Unwrap(JsValueRef obj, jsrtimpl::ExternalData** externalData,
                          JsValueRef* wrapper = nullptr,
                          JsValueRef* parent = nullptr) {
  JsValueRef candidate = JS_INVALID_REFERENCE;
  JsValueRef candidateParent = JS_INVALID_REFERENCE;
  CHECK_NAPI(jsrtimpl::FindWrapper(obj, &candidate, &candidateParent));
  RETURN_STATUS_IF_FALSE(candidate != JS_INVALID_REFERENCE, napi_invalid_arg);

  CHECK_JSRT(JsGetExternalData(candidate,
                               reinterpret_cast<void**>(externalData)));

  if (wrapper != nullptr) {
    *wrapper = candidate;
  }

  if (parent != nullptr) {
    *parent = candidateParent;
  }

  return napi_ok;
}

static napi_status SetErrorCode(JsValueRef error,
                                napi_value code,
                                const char* codeString) {
  if ((code != nullptr) || (codeString != nullptr)) {
    JsValueRef codeValue = reinterpret_cast<JsValueRef>(code);
    if (codeValue != JS_INVALID_REFERENCE) {
      JsValueType valueType = JsUndefined;
      CHECK_JSRT(JsGetValueType(codeValue, &valueType));
      RETURN_STATUS_IF_FALSE(valueType == JsString, napi_string_expected);
    } else {
      CHECK_JSRT(JsCreateString(codeString, strlen(codeString), &codeValue));
    }

    const char* codePropIdName = "code";
    JsPropertyIdRef codePropId = JS_INVALID_REFERENCE;
    CHECK_JSRT(JsCreatePropertyId(codePropIdName,
                                  strlen(codePropIdName),
                                  &codePropId));

    CHECK_JSRT(JsSetProperty(error, codePropId, codeValue, true));

    JsValueRef nameArray = JS_INVALID_REFERENCE;
    CHECK_JSRT(JsCreateArray(0, &nameArray));

    const char* pushPropIdName = "push";
    JsPropertyIdRef pushPropId = JS_INVALID_REFERENCE;
    CHECK_JSRT(JsCreatePropertyId(pushPropIdName,
                                  strlen(pushPropIdName),
                                  &pushPropId));

    JsValueRef pushFunction = JS_INVALID_REFERENCE;
    CHECK_JSRT(JsGetProperty(nameArray, pushPropId, &pushFunction));

    const char* namePropIdName = "name";
    JsPropertyIdRef namePropId = JS_INVALID_REFERENCE;
    CHECK_JSRT(JsCreatePropertyId(namePropIdName,
                                  strlen(namePropIdName),
                                  &namePropId));

    bool hasProp = false;
    CHECK_JSRT(JsHasProperty(error, namePropId, &hasProp));

    JsValueRef nameValue = JS_INVALID_REFERENCE;
    std::array<JsValueRef, 2> args = { nameArray, JS_INVALID_REFERENCE };

    if (hasProp) {
      CHECK_JSRT(JsGetProperty(error, namePropId, &nameValue));

      args[1] = nameValue;
      CHECK_JSRT(JsCallFunction(pushFunction,
                                args.data(),
                                args.size(),
                                nullptr));
    }

    const char* openBracket = " [";
    JsValueRef openBracketValue = JS_INVALID_REFERENCE;
    CHECK_JSRT(JsCreateString(openBracket,
                              strlen(openBracket),
                              &openBracketValue));

    args[1] = openBracketValue;
    CHECK_JSRT(JsCallFunction(pushFunction, args.data(), args.size(), nullptr));

    args[1] = codeValue;
    CHECK_JSRT(JsCallFunction(pushFunction, args.data(), args.size(), nullptr));

    const char* closeBracket = "]";
    JsValueRef closeBracketValue = JS_INVALID_REFERENCE;
    CHECK_JSRT(JsCreateString(closeBracket,
                              strlen(closeBracket),
                              &closeBracketValue));

    args[1] = closeBracketValue;
    CHECK_JSRT(JsCallFunction(pushFunction, args.data(), args.size(), nullptr));

    JsValueRef emptyValue = JS_INVALID_REFERENCE;
    CHECK_JSRT(JsCreateString("", 0, &emptyValue));

    const char* joinPropIdName = "join";
    JsPropertyIdRef joinPropId = JS_INVALID_REFERENCE;
    CHECK_JSRT(JsCreatePropertyId(joinPropIdName,
                                  strlen(joinPropIdName),
                                  &joinPropId));

    JsValueRef joinFunction = JS_INVALID_REFERENCE;
    CHECK_JSRT(JsGetProperty(nameArray, joinPropId, &joinFunction));

    args[1] = emptyValue;
    CHECK_JSRT(JsCallFunction(joinFunction,
                              args.data(),
                              args.size(),
                              &nameValue));

    CHECK_JSRT(JsSetProperty(error, namePropId, nameValue, true));
  }
  return napi_ok;
}

napi_status ConcludeDeferred(napi_env env,
                             napi_deferred deferred,
                             const char* property,
                             napi_value result) {
  // We do not check if property is OK, because that's not coming from outside.
  CHECK_ARG(deferred);
  CHECK_ARG(result);

  napi_value container, resolver, js_null;
  napi_ref ref = reinterpret_cast<napi_ref>(deferred);

  CHECK_NAPI(napi_get_reference_value(env, ref, &container));
  CHECK_NAPI(napi_get_named_property(env, container, property, &resolver));
  CHECK_NAPI(napi_get_null(env, &js_null));
  CHECK_NAPI(napi_call_function(env, js_null, resolver, 1, &result, nullptr));
  CHECK_NAPI(napi_delete_reference(env, ref));

  return napi_ok;
}

}  // end of namespace jsrtimpl

// Intercepts the Node-V8 module registration callback. Converts parameters
// to NAPI equivalents and then calls the registration callback specified
// by the NAPI module.
void napi_module_register_cb(v8::Local<v8::Object> exports,
                             v8::Local<v8::Value> module,
                             v8::Local<v8::Context> context,
                             void* priv) {
  napi_module* mod = static_cast<napi_module*>(priv);

  // Create a new napi_env for this module. Once module unloading is supported
  // we shall have to call delete on this object from there.
  napi_env env = new napi_env__(context->GetIsolate());

  napi_value _exports =
    mod->nm_register_func(env, v8impl::JsValueFromV8LocalValue(exports));

  // If register function returned a non-null exports object different from
  // the exports object we passed it, set that as the "exports" property of
  // the module.
  if (_exports != nullptr &&
    _exports != v8impl::JsValueFromV8LocalValue(exports)) {
    napi_value _module = v8impl::JsValueFromV8LocalValue(module);
    napi_set_named_property(env, _module, "exports", _exports);
  }
}

}  // end of anonymous namespace

// Registers a NAPI module.
void napi_module_register(napi_module* mod) {
  // NAPI modules always work with the current node version.
  int module_version = NODE_MODULE_VERSION;

  node::node_module* nm = new node::node_module {
    module_version,
    mod->nm_flags,
    nullptr,
    mod->nm_filename,
    nullptr,
    napi_module_register_cb,
    mod->nm_modname,
    mod,  // priv
    nullptr,
  };
  node::node_module_register(nm);
}

// Static last error returned from napi_get_last_error_info
napi_extended_error_info static_last_error;

// Warning: Keep in-sync with napi_status enum
const char* error_messages[] = {
  nullptr,
  "Invalid pointer passed as argument",
  "An object was expected",
  "A string was expected",
  "A string or symbol was expected",
  "A function was expected",
  "A number was expected",
  "A boolean was expected",
  "An array was expected",
  "Unknown failure",
  "An exception is pending",
  "The async work item was cancelled",
  "napi_escape_handle already called on scope"
};

napi_status napi_get_last_error_info(napi_env env,
                                     const napi_extended_error_info** result) {
  CHECK_ARG(result);

  static_assert(
    node::arraysize(error_messages) == napi_escape_called_twice + 1,
    "Count of error messages must match count of error values");
  assert(static_last_error.error_code <= napi_escape_called_twice);

  // Wait until someone requests the last error information to fetch the error
  // message string
  static_last_error.error_message =
    error_messages[static_last_error.error_code];

  *result = &static_last_error;
  return napi_ok;
}

void napi_clear_last_error() {
  static_last_error.error_code = napi_ok;
  static_last_error.engine_error_code = 0;
  static_last_error.engine_reserved = nullptr;
}

// TODO(digitalinfinity): Does this need to be on the
// environment instead of a static error?
napi_status napi_set_last_error(napi_status error_code,
                                uint32_t engine_error_code,
                                void* engine_reserved) {
  static_last_error.error_code = error_code;
  static_last_error.engine_error_code = engine_error_code;
  static_last_error.engine_reserved = engine_reserved;

  return error_code;
}

napi_status napi_set_last_error(JsErrorCode jsError, void* engine_reserved) {
  napi_status status;
  switch (jsError) {
    case JsNoError: status = napi_ok; break;
    case JsErrorNullArgument:
    case JsErrorInvalidArgument: status = napi_invalid_arg; break;
    case JsErrorPropertyNotString: status = napi_string_expected; break;
    case JsErrorArgumentNotObject: status = napi_object_expected; break;
    case JsErrorScriptException:
    case JsErrorInExceptionState: status = napi_pending_exception; break;
    default: status = napi_generic_failure; break;
  }

  static_last_error.error_code = status;
  static_last_error.engine_error_code = jsError;
  static_last_error.engine_reserved = engine_reserved;
  return status;
}

NAPI_NO_RETURN void napi_fatal_error(const char* location,
                                     const char* message) {
  node::FatalError(location, message);
}

napi_status napi_create_function(napi_env env,
                                 const char* utf8name,
                                 napi_callback cb,
                                 void* callback_data,
                                 napi_value* result) {
  CHECK_ARG(result);

  jsrtimpl::ExternalCallback* externalCallback =
    new jsrtimpl::ExternalCallback(env, cb, callback_data);
  if (externalCallback == nullptr) {
    return napi_set_last_error(napi_generic_failure);
  }

  JsValueRef function;
  if (utf8name != nullptr) {
    JsValueRef name;
    CHECK_JSRT(JsCreateString(
      utf8name,
      strlen(utf8name),
      &name));
    CHECK_JSRT(JsCreateNamedFunction(
      name,
      jsrtimpl::ExternalCallback::Callback,
      externalCallback,
      &function));
  } else {
    CHECK_JSRT(JsCreateFunction(
      jsrtimpl::ExternalCallback::Callback,
      externalCallback,
      &function));
  }

  CHECK_JSRT(JsSetObjectBeforeCollectCallback(
    function, externalCallback, jsrtimpl::ExternalCallback::Finalize));

  *result = reinterpret_cast<napi_value>(function);
  return napi_ok;
}

static napi_status napi_create_property_function(napi_env env,
                                                 napi_value property_name,
                                                 napi_callback cb,
                                                 void* callback_data,
                                                 napi_value* result) {
  CHECK_ARG(result);

  jsrtimpl::ExternalCallback* externalCallback =
    new jsrtimpl::ExternalCallback(env, cb, callback_data);
  if (externalCallback == nullptr) {
    return napi_set_last_error(napi_generic_failure);
  }

  napi_valuetype nameType;
  CHECK_NAPI(napi_typeof(env, property_name, &nameType));

  JsValueRef function;
  if (nameType == napi_string) {
    CHECK_JSRT(JsCreateNamedFunction(
      property_name,
      jsrtimpl::ExternalCallback::Callback,
      externalCallback,
      &function));
  } else {
    CHECK_JSRT(JsCreateFunction(
      jsrtimpl::ExternalCallback::Callback,
      externalCallback,
      &function));
  }

  CHECK_JSRT(JsSetObjectBeforeCollectCallback(
    function, externalCallback, jsrtimpl::ExternalCallback::Finalize));

  *result = reinterpret_cast<napi_value>(function);
  return napi_ok;
}

napi_status napi_define_class(napi_env env,
                              const char* utf8name,
                              napi_callback cb,
                              void* data,
                              size_t property_count,
                              const napi_property_descriptor* properties,
                              napi_value* result) {
  CHECK_ARG(result);

  napi_value namestring;
  CHECK_NAPI(napi_create_string_utf8(env, utf8name, -1, &namestring));

  jsrtimpl::ExternalCallback* externalCallback =
    new jsrtimpl::ExternalCallback(env, cb, data);
  if (externalCallback == nullptr) {
    return napi_set_last_error(napi_generic_failure);
  }

  JsValueRef constructor;
  CHECK_JSRT(JsCreateNamedFunction(namestring,
                                   jsrtimpl::ExternalCallback::Callback,
                                   externalCallback,
                                   &constructor));

  CHECK_JSRT(JsSetObjectBeforeCollectCallback(
    constructor, externalCallback, jsrtimpl::ExternalCallback::Finalize));

  JsPropertyIdRef pid = nullptr;
  JsValueRef prototype = nullptr;
  CHECK_JSRT(JsCreatePropertyId("prototype", 10, &pid));
  CHECK_JSRT(JsGetProperty(constructor, pid, &prototype));

  CHECK_JSRT(JsCreatePropertyId("constructor", 12, &pid));
  CHECK_JSRT(JsSetProperty(prototype, pid, constructor, false));

  int instancePropertyCount = 0;
  int staticPropertyCount = 0;
  for (size_t i = 0; i < property_count; i++) {
    if ((properties[i].attributes & napi_static) != 0) {
      staticPropertyCount++;
    } else {
      instancePropertyCount++;
    }
  }

  std::vector<napi_property_descriptor> staticDescriptors;
  std::vector<napi_property_descriptor> instanceDescriptors;
  staticDescriptors.reserve(staticPropertyCount);
  instanceDescriptors.reserve(instancePropertyCount);

  for (size_t i = 0; i < property_count; i++) {
    if ((properties[i].attributes & napi_static) != 0) {
      staticDescriptors.push_back(properties[i]);
    } else {
      instanceDescriptors.push_back(properties[i]);
    }
  }

  if (staticPropertyCount > 0) {
    CHECK_NAPI(napi_define_properties(env,
                                      reinterpret_cast<napi_value>(constructor),
                                      staticDescriptors.size(),
                                      staticDescriptors.data()));
  }

  if (instancePropertyCount > 0) {
    CHECK_NAPI(napi_define_properties(env,
                                      reinterpret_cast<napi_value>(prototype),
                                      instanceDescriptors.size(),
                                      instanceDescriptors.data()));
  }

  *result = reinterpret_cast<napi_value>(constructor);
  return napi_ok;
}

napi_status napi_get_property_names(napi_env env,
                                    napi_value object,
                                    napi_value* result) {
  CHECK_ARG(result);
  JsValueRef obj = reinterpret_cast<JsValueRef>(object);
  JsValueRef propertyNames;
  CHECK_JSRT(JsGetOwnPropertyNames(obj, &propertyNames));
  *result = reinterpret_cast<napi_value>(propertyNames);
  return napi_ok;
}

napi_status napi_delete_property(napi_env env,
                                 napi_value object,
                                 napi_value key,
                                 bool* result) {
  CHECK_ARG(result);
  *result = false;

  JsValueRef obj = reinterpret_cast<JsValueRef>(object);
  JsPropertyIdRef propertyId;
  JsValueRef deletePropertyResult;
  CHECK_NAPI(jsrtimpl::JsPropertyIdFromKey(key, &propertyId));
  CHECK_JSRT(JsDeleteProperty(obj, propertyId, false /* isStrictMode */,
                              &deletePropertyResult));
  CHECK_JSRT(JsBooleanToBool(deletePropertyResult, result));

  return napi_ok;
}

NAPI_EXTERN napi_status napi_has_own_property(napi_env env,
                                              napi_value object,
                                              napi_value key,
                                              bool* result) {
  CHECK_ARG(result);
  JsPropertyIdRef propertyId;
  CHECK_NAPI(jsrtimpl::JsPropertyIdFromKey(key, &propertyId));
  JsValueRef obj = reinterpret_cast<JsValueRef>(object);
  CHECK_JSRT(JsHasOwnProperty(obj, propertyId, result));
  return napi_ok;
}

napi_status napi_set_named_property(napi_env env,
                                    napi_value object,
                                    const char* utf8name,
                                    napi_value value) {
  JsValueRef obj = reinterpret_cast<JsValueRef>(object);
  JsPropertyIdRef propertyId;
  CHECK_JSRT(JsCreatePropertyId(utf8name, strlen(utf8name), &propertyId));
  JsValueRef js_value = reinterpret_cast<JsValueRef>(value);
  CHECK_JSRT(JsSetProperty(obj, propertyId, js_value, true));
  return napi_ok;
}

napi_status napi_set_property(napi_env env,
                              napi_value object,
                              napi_value key,
                              napi_value value) {
  JsValueRef obj = reinterpret_cast<JsValueRef>(object);
  JsPropertyIdRef propertyId;
  CHECK_NAPI(jsrtimpl::JsPropertyIdFromKey(key, &propertyId));
  JsValueRef js_value = reinterpret_cast<JsValueRef>(value);
  CHECK_JSRT(JsSetProperty(obj, propertyId, js_value, true));
  return napi_ok;
}

napi_status napi_delete_element(napi_env env,
                                napi_value object,
                                uint32_t index,
                                bool* result) {
  CHECK_ARG(result);
  JsValueRef indexValue = nullptr;
  JsValueRef obj = reinterpret_cast<JsValueRef>(object);
  CHECK_JSRT(JsIntToNumber(index, &indexValue));
  CHECK_JSRT(JsDeleteIndexedProperty(obj, indexValue));
  *result = true;
  return napi_ok;
}

napi_status napi_define_properties(napi_env env,
                                   napi_value object,
                                   size_t property_count,
                                   const napi_property_descriptor* properties) {
  JsPropertyIdRef configurableProperty;
  CHECK_JSRT(JsCreatePropertyId("configurable", 12, &configurableProperty));

  JsPropertyIdRef enumerableProperty;
  CHECK_JSRT(JsCreatePropertyId("enumerable", 10, &enumerableProperty));

  for (size_t i = 0; i < property_count; i++) {
    const napi_property_descriptor* p = properties + i;

    JsValueRef descriptor;
    CHECK_JSRT(JsCreateObject(&descriptor));

    JsValueRef configurable;
    CHECK_JSRT(
      JsBoolToBoolean((p->attributes & napi_configurable), &configurable));
    CHECK_JSRT(
      JsSetProperty(descriptor, configurableProperty, configurable, true));

    JsValueRef enumerable;
    CHECK_JSRT(JsBoolToBoolean((p->attributes & napi_enumerable), &enumerable));
    CHECK_JSRT(JsSetProperty(descriptor, enumerableProperty, enumerable, true));

    if (p->getter != nullptr || p->setter != nullptr) {
      napi_value property_name;
      CHECK_NAPI(
        jsrtimpl::JsNameValueFromPropertyDescriptor(p, &property_name));

      if (p->getter != nullptr) {
        JsPropertyIdRef getProperty;
        CHECK_JSRT(JsCreatePropertyId("get", 3, &getProperty));
        JsValueRef getter;
        CHECK_NAPI(napi_create_property_function(env, property_name,
          p->getter, p->data, reinterpret_cast<napi_value*>(&getter)));
        CHECK_JSRT(JsSetProperty(descriptor, getProperty, getter, true));
      }

      if (p->setter != nullptr) {
        JsPropertyIdRef setProperty;
        CHECK_JSRT(JsCreatePropertyId("set", 5, &setProperty));
        JsValueRef setter;
        CHECK_NAPI(napi_create_property_function(env, property_name,
          p->setter, p->data, reinterpret_cast<napi_value*>(&setter)));
        CHECK_JSRT(JsSetProperty(descriptor, setProperty, setter, true));
      }
    } else if (p->method != nullptr) {
      napi_value property_name;
      CHECK_NAPI(
        jsrtimpl::JsNameValueFromPropertyDescriptor(p, &property_name));

      JsPropertyIdRef valueProperty;
      CHECK_JSRT(JsCreatePropertyId("value", 5, &valueProperty));
      JsValueRef method;
      CHECK_NAPI(napi_create_property_function(env, property_name,
        p->method, p->data, reinterpret_cast<napi_value*>(&method)));
      CHECK_JSRT(JsSetProperty(descriptor, valueProperty, method, true));
    } else {
      RETURN_STATUS_IF_FALSE(p->value != nullptr, napi_invalid_arg);

      JsPropertyIdRef writableProperty;
      CHECK_JSRT(JsCreatePropertyId("writable", 8, &writableProperty));
      JsValueRef writable;
      CHECK_JSRT(JsBoolToBoolean((p->attributes & napi_writable), &writable));
      CHECK_JSRT(JsSetProperty(descriptor, writableProperty, writable, true));

      JsPropertyIdRef valueProperty;
      CHECK_JSRT(JsCreatePropertyId("value", 5, &valueProperty));
      CHECK_JSRT(JsSetProperty(descriptor, valueProperty,
        reinterpret_cast<JsValueRef>(p->value), true));
    }

    JsPropertyIdRef nameProperty;
    CHECK_NAPI(jsrtimpl::JsPropertyIdFromPropertyDescriptor(p, &nameProperty));
    bool result;
    CHECK_JSRT(JsDefineProperty(
      reinterpret_cast<JsValueRef>(object),
      reinterpret_cast<JsPropertyIdRef>(nameProperty),
      reinterpret_cast<JsValueRef>(descriptor),
      &result));
  }

  return napi_ok;
}

napi_status napi_instanceof(napi_env env,
                            napi_value object,
                            napi_value c,
                            bool* result) {
  CHECK_ARG(result);
  JsValueRef obj = reinterpret_cast<JsValueRef>(object);
  JsValueRef constructor = reinterpret_cast<JsValueRef>(c);

  // FIXME: Remove this type check when we switch to a version of Chakracore
  // where passing an integer into JsInstanceOf as the constructor parameter
  // does not cause a segfault. The need for this if-statement is removed in at
  // least Chakracore 1.4.0, but maybe in an earlier version too.
  napi_valuetype valuetype;
  CHECK_NAPI(napi_typeof(env, c, &valuetype));
  if (valuetype != napi_function) {
    napi_throw_type_error(env,
                          "ERR_NAPI_CONS_FUNCTION",
                          "constructor must be a function");

    return napi_set_last_error(napi_invalid_arg);
  }

  CHECK_JSRT(JsInstanceOf(obj, constructor, result));
  return napi_ok;
}

napi_status napi_has_named_property(napi_env env,
                                    napi_value object,
                                    const char* utf8name,
                                    bool* result) {
  CHECK_ARG(result);
  JsPropertyIdRef propertyId;
  CHECK_JSRT(JsCreatePropertyId(utf8name, strlen(utf8name), &propertyId));
  JsValueRef obj = reinterpret_cast<JsValueRef>(object);
  CHECK_JSRT(JsHasProperty(obj, propertyId, result));
  return napi_ok;
}

napi_status napi_has_property(napi_env env,
                              napi_value object,
                              napi_value key,
                              bool* result) {
  CHECK_ARG(result);
  JsPropertyIdRef propertyId;
  CHECK_NAPI(jsrtimpl::JsPropertyIdFromKey(key, &propertyId));
  JsValueRef obj = reinterpret_cast<JsValueRef>(object);
  CHECK_JSRT(JsHasProperty(obj, propertyId, result));
  return napi_ok;
}

napi_status napi_get_named_property(napi_env env,
                                    napi_value object,
                                    const char* utf8name,
                                    napi_value* result) {
  CHECK_ARG(result);
  JsValueRef obj = reinterpret_cast<JsValueRef>(object);
  JsPropertyIdRef propertyId;
  CHECK_JSRT(JsCreatePropertyId(utf8name, strlen(utf8name), &propertyId));
  CHECK_JSRT(
    JsGetProperty(obj, propertyId, reinterpret_cast<JsValueRef*>(result)));
  return napi_ok;
}

napi_status napi_get_property(napi_env env,
                              napi_value object,
                              napi_value key,
                              napi_value* result) {
  CHECK_ARG(result);
  JsValueRef obj = reinterpret_cast<JsValueRef>(object);
  JsPropertyIdRef propertyId;
  CHECK_NAPI(jsrtimpl::JsPropertyIdFromKey(key, &propertyId));
  CHECK_JSRT(
    JsGetProperty(obj, propertyId, reinterpret_cast<JsValueRef*>(result)));
  return napi_ok;
}

napi_status napi_set_element(napi_env env,
                             napi_value object,
                             uint32_t i,
                             napi_value v) {
  JsValueRef index = nullptr;
  JsValueRef obj = reinterpret_cast<JsValueRef>(object);
  JsValueRef value = reinterpret_cast<JsValueRef>(v);
  CHECK_JSRT(JsIntToNumber(i, &index));
  CHECK_JSRT(JsSetIndexedProperty(obj, index, value));
  return napi_ok;
}

napi_status napi_has_element(napi_env env,
                             napi_value object,
                             uint32_t i,
                             bool* result) {
  CHECK_ARG(result);
  JsValueRef index = nullptr;
  CHECK_JSRT(JsIntToNumber(i, &index));
  JsValueRef obj = reinterpret_cast<JsValueRef>(object);
  CHECK_JSRT(JsHasIndexedProperty(obj, index, result));
  return napi_ok;
}

napi_status napi_get_element(napi_env env,
                             napi_value object,
                             uint32_t i,
                             napi_value* result) {
  CHECK_ARG(result);
  JsValueRef index = nullptr;
  JsValueRef obj = reinterpret_cast<JsValueRef>(object);
  CHECK_JSRT(JsIntToNumber(i, &index));
  CHECK_JSRT(
    JsGetIndexedProperty(obj, index, reinterpret_cast<JsValueRef*>(result)));
  return napi_ok;
}

napi_status napi_is_array(napi_env env, napi_value v, bool* result) {
  CHECK_ARG(result);
  JsValueRef value = reinterpret_cast<JsValueRef>(v);
  JsValueType type = JsUndefined;
  CHECK_JSRT(JsGetValueType(value, &type));
  *result = (type == JsArray);
  return napi_ok;
}

napi_status napi_get_array_length(napi_env env,
                                  napi_value v,
                                  uint32_t* result) {
  CHECK_ARG(result);
  JsPropertyIdRef propertyIdRef;
  CHECK_JSRT(JsCreatePropertyId("length", 7, &propertyIdRef));
  JsValueRef lengthRef;
  JsValueRef arrayRef = reinterpret_cast<JsValueRef>(v);
  CHECK_JSRT(JsGetProperty(arrayRef, propertyIdRef, &lengthRef));
  double sizeInDouble;
  CHECK_JSRT(JsNumberToDouble(lengthRef, &sizeInDouble));
  *result = static_cast<unsigned int>(sizeInDouble);
  return napi_ok;
}

napi_status napi_strict_equals(napi_env env,
                               napi_value lhs,
                               napi_value rhs,
                               bool* result) {
  CHECK_ARG(result);
  JsValueRef object1 = reinterpret_cast<JsValueRef>(lhs);
  JsValueRef object2 = reinterpret_cast<JsValueRef>(rhs);
  CHECK_JSRT(JsStrictEquals(object1, object2, result));
  return napi_ok;
}

napi_status napi_get_prototype(napi_env env,
                               napi_value object,
                               napi_value* result) {
  CHECK_ARG(result);
  JsValueRef obj = reinterpret_cast<JsValueRef>(object);
  CHECK_JSRT(JsGetPrototype(obj, reinterpret_cast<JsValueRef*>(result)));
  return napi_ok;
}

napi_status napi_create_object(napi_env env, napi_value* result) {
  CHECK_ARG(result);
  CHECK_JSRT(JsCreateObject(reinterpret_cast<JsValueRef*>(result)));
  return napi_ok;
}

napi_status napi_create_array(napi_env env, napi_value* result) {
  CHECK_ARG(result);
  unsigned int length = 0;
  CHECK_JSRT(JsCreateArray(length, reinterpret_cast<JsValueRef*>(result)));
  return napi_ok;
}

napi_status napi_create_array_with_length(napi_env env,
                                          size_t length,
                                          napi_value* result) {
  CHECK_ARG(result);
  CHECK_JSRT(JsCreateArray(length, reinterpret_cast<JsValueRef*>(result)));
  return napi_ok;
}

napi_status napi_create_string_latin1(napi_env env,
                                      const char* str,
                                      size_t length,
                                      napi_value* result) {
  // TODO(boingoing): Convert from Latin1 to UTF-8 encoding.
  CHECK_ARG(result);
  CHECK_JSRT(JsCreateString(
    str,
    length,
    reinterpret_cast<JsValueRef*>(result)));
  return napi_ok;
}

napi_status napi_create_string_utf8(napi_env env,
                                    const char* str,
                                    size_t length,
                                    napi_value* result) {
  CHECK_ARG(result);
  CHECK_JSRT(JsCreateString(
    str,
    length,
    reinterpret_cast<JsValueRef*>(result)));
  return napi_ok;
}

napi_status napi_create_string_utf16(napi_env env,
                                     const char16_t* str,
                                     size_t length,
                                     napi_value* result) {
  CHECK_ARG(result);
  CHECK_JSRT(JsCreateStringUtf16(
    reinterpret_cast<const uint16_t*>(str),
    length,
    reinterpret_cast<JsValueRef*>(result)));
  return napi_ok;
}

napi_status napi_create_double(napi_env env,
                               double value,
                               napi_value* result) {
  CHECK_ARG(result);
  CHECK_JSRT(JsDoubleToNumber(value, reinterpret_cast<JsValueRef*>(result)));
  return napi_ok;
}

napi_status napi_create_int32(napi_env env,
                              int32_t value,
                              napi_value* result) {
  CHECK_ARG(result);
  CHECK_JSRT(JsIntToNumber(value, reinterpret_cast<JsValueRef*>(result)));
  return napi_ok;
}

napi_status napi_create_uint32(napi_env env,
                               uint32_t value,
                               napi_value* result) {
  CHECK_ARG(result);
  CHECK_JSRT(JsDoubleToNumber(static_cast<double>(value),
                              reinterpret_cast<JsValueRef*>(result)));
  return napi_ok;
}

napi_status napi_create_int64(napi_env env,
                              int64_t value,
                              napi_value* result) {
  CHECK_ARG(result);
  CHECK_JSRT(JsDoubleToNumber(static_cast<double>(value),
                              reinterpret_cast<JsValueRef*>(result)));
  return napi_ok;
}

napi_status napi_get_boolean(napi_env env, bool value, napi_value* result) {
  CHECK_ARG(result);
  CHECK_JSRT(JsBoolToBoolean(value, reinterpret_cast<JsValueRef*>(result)));
  return napi_ok;
}

napi_status napi_create_symbol(napi_env env,
                               napi_value description,
                               napi_value* result) {
  CHECK_ARG(result);
  JsValueRef js_description = reinterpret_cast<JsValueRef>(description);
  CHECK_JSRT(
    JsCreateSymbol(js_description, reinterpret_cast<JsValueRef*>(result)));
  return napi_ok;
}

napi_status napi_create_error(napi_env env,
                              napi_value code,
                              napi_value msg,
                              napi_value* result) {
  CHECK_ARG(result);
  JsValueRef message = reinterpret_cast<JsValueRef>(msg);

  JsValueRef error = JS_INVALID_REFERENCE;
  CHECK_JSRT(JsCreateError(message, &error));
  CHECK_NAPI(jsrtimpl::SetErrorCode(error, code, nullptr));

  *result = reinterpret_cast<napi_value>(error);
  return napi_ok;
}

napi_status napi_create_type_error(napi_env env,
                                   napi_value code,
                                   napi_value msg,
                                   napi_value* result) {
  CHECK_ARG(result);
  JsValueRef message = reinterpret_cast<JsValueRef>(msg);

  JsValueRef error = JS_INVALID_REFERENCE;
  CHECK_JSRT(JsCreateTypeError(message,  &error));
  CHECK_NAPI(jsrtimpl::SetErrorCode(error, code, nullptr));

  *result = reinterpret_cast<napi_value>(error);
  return napi_ok;
}

napi_status napi_create_range_error(napi_env env,
                                    napi_value code,
                                    napi_value msg,
                                    napi_value* result) {
  CHECK_ARG(result);
  JsValueRef message = reinterpret_cast<JsValueRef>(msg);

  JsValueRef error = JS_INVALID_REFERENCE;
  CHECK_JSRT(
    JsCreateRangeError(message,  &error));
  CHECK_NAPI(jsrtimpl::SetErrorCode(error, code, nullptr));

  *result = reinterpret_cast<napi_value>(error);
  return napi_ok;
}

napi_status napi_typeof(napi_env env, napi_value vv, napi_valuetype* result) {
  CHECK_ARG(result);
  JsValueRef value = reinterpret_cast<JsValueRef>(vv);
  JsValueType valueType = JsUndefined;
  CHECK_JSRT(JsGetValueType(value, &valueType));

  switch (valueType) {
    case JsUndefined: *result = napi_undefined; break;
    case JsNull: *result = napi_null; break;
    case JsNumber: *result = napi_number; break;
    case JsString: *result = napi_string; break;
    case JsBoolean: *result = napi_boolean; break;
    case JsFunction: *result = napi_function; break;
    case JsSymbol: *result = napi_symbol; break;
    case JsError: *result = napi_object; break;

    default:
      bool hasExternalData;
      if (JsHasExternalData(value, &hasExternalData) != JsNoError) {
        hasExternalData = false;
      }

      *result = hasExternalData ? napi_external : napi_object;
      break;
  }
  return napi_ok;
}

napi_status napi_get_undefined(napi_env env, napi_value* result) {
  CHECK_ARG(result);
  CHECK_JSRT(JsGetUndefinedValue(reinterpret_cast<JsValueRef*>(result)));
  return napi_ok;
}

napi_status napi_get_null(napi_env env, napi_value* result) {
  CHECK_ARG(result);
  CHECK_JSRT(JsGetNullValue(reinterpret_cast<JsValueRef*>(result)));
  return napi_ok;
}

napi_status napi_get_cb_info(
    napi_env env,               // [in] NAPI environment handle
    napi_callback_info cbinfo,  // [in] Opaque callback-info handle
    size_t* argc,      // [in-out] Specifies the size of the provided argv array
                       // and receives the actual count of args.
    napi_value* argv,  // [out] Array of values
    napi_value* this_arg,  // [out] Receives the JS 'this' arg for the call
    void** data) {         // [out] Receives the data pointer for the callback.
  CHECK_ARG(cbinfo);
  const CallbackInfo *info = reinterpret_cast<CallbackInfo*>(cbinfo);

  if (argv != nullptr) {
    CHECK_ARG(argc);

    size_t i = 0;
    size_t min = std::min(*argc, static_cast<size_t>(info->argc));

    for (; i < min; i++) {
      argv[i] = info->argv[i];
    }

    if (i < *argc) {
      napi_value undefined;
      CHECK_JSRT(
        JsGetUndefinedValue(reinterpret_cast<JsValueRef*>(&undefined)));
      for (; i < *argc; i++) {
        argv[i] = undefined;
      }
    }
  }

  if (argc != nullptr) {
    *argc = info->argc;
  }

  if (this_arg != nullptr) {
    *this_arg = info->thisArg;
  }

  if (data != nullptr) {
    *data = info->data;
  }

  return napi_ok;
}

napi_status napi_get_new_target(napi_env env,
                                napi_callback_info cbinfo,
                                napi_value* result) {
  CHECK_ARG(cbinfo);
  CHECK_ARG(result);
  const CallbackInfo *info = reinterpret_cast<CallbackInfo*>(cbinfo);
  *result = info->newTarget;
  return napi_ok;
}

napi_status napi_call_function(napi_env env,
                               napi_value recv,
                               napi_value func,
                               size_t argc,
                               const napi_value* argv,
                               napi_value* result) {
  JsValueRef object = reinterpret_cast<JsValueRef>(recv);
  JsValueRef function = reinterpret_cast<JsValueRef>(func);
  std::vector<JsValueRef> args(argc + 1);
  args[0] = object;
  for (size_t i = 0; i < argc; i++) {
    args[i + 1] = reinterpret_cast<JsValueRef>(argv[i]);
  }
  JsValueRef returnValue;
  CHECK_JSRT(JsCallFunction(
    function,
    args.data(),
    static_cast<uint16_t>(argc + 1),
    &returnValue));
  if (result != nullptr) {
    *result = reinterpret_cast<napi_value>(returnValue);
  }
  return napi_ok;
}

napi_status napi_get_global(napi_env env, napi_value* result) {
  CHECK_ARG(result);
  CHECK_JSRT(JsGetGlobalObject(reinterpret_cast<JsValueRef*>(result)));
  return napi_ok;
}

napi_status napi_throw(napi_env env, napi_value error) {
  JsValueRef exception = reinterpret_cast<JsValueRef>(error);
  CHECK_JSRT(JsSetException(exception));
  return napi_ok;
}

napi_status napi_throw_error(napi_env env,
                             const char* code,
                             const char* msg) {
  JsValueRef strRef;
  JsValueRef exception;
  size_t length = strlen(msg);
  CHECK_JSRT(JsCreateString(msg, length, &strRef));
  CHECK_JSRT(JsCreateError(strRef, &exception));
  CHECK_NAPI(jsrtimpl::SetErrorCode(exception, nullptr, code));
  CHECK_JSRT(JsSetException(exception));
  return napi_ok;
}

napi_status napi_throw_type_error(napi_env env,
                                  const char* code,
                                  const char* msg) {
  JsValueRef strRef;
  JsValueRef exception;
  size_t length = strlen(msg);
  CHECK_JSRT(JsCreateString(msg, length, &strRef));
  CHECK_JSRT(JsCreateTypeError(strRef, &exception));
  CHECK_NAPI(jsrtimpl::SetErrorCode(exception, nullptr, code));
  CHECK_JSRT(JsSetException(exception));
  return napi_ok;
}

napi_status napi_throw_range_error(napi_env env,
                                   const char* code,
                                   const char* msg) {
  JsValueRef strRef;
  JsValueRef exception;
  size_t length = strlen(msg);
  CHECK_JSRT(JsCreateString(msg, length, &strRef));
  CHECK_JSRT(JsCreateRangeError(strRef, &exception));
  CHECK_NAPI(jsrtimpl::SetErrorCode(exception, nullptr, code));
  CHECK_JSRT(JsSetException(exception));
  return napi_ok;
}

napi_status napi_is_error(napi_env env, napi_value value, bool* result) {
  CHECK_ARG(result);
  JsValueType valueType;
  CHECK_JSRT(JsGetValueType(value, &valueType));
  *result = (valueType == JsError);
  return napi_ok;
}

napi_status napi_get_value_double(napi_env env, napi_value v, double* result) {
  CHECK_ARG(result);
  JsValueRef value = reinterpret_cast<JsValueRef>(v);
  CHECK_JSRT_EXPECTED(JsNumberToDouble(value, result), napi_number_expected);
  return napi_ok;
}

napi_status napi_get_value_int32(napi_env env, napi_value v, int32_t* result) {
  CHECK_ARG(result);
  JsValueRef value = reinterpret_cast<JsValueRef>(v);
  int valueInt;
  CHECK_JSRT_EXPECTED(JsNumberToInt(value, &valueInt), napi_number_expected);
  *result = static_cast<int32_t>(valueInt);
  return napi_ok;
}

napi_status napi_get_value_uint32(napi_env env,
                                  napi_value v,
                                  uint32_t* result) {
  CHECK_ARG(result);
  JsValueRef value = reinterpret_cast<JsValueRef>(v);
  int valueInt;
  CHECK_JSRT_EXPECTED(JsNumberToInt(value, &valueInt), napi_number_expected);
  *result = static_cast<uint32_t>(valueInt);
  return napi_ok;
}

napi_status napi_get_value_int64(napi_env env, napi_value v, int64_t* result) {
  CHECK_ARG(result);
  JsValueRef value = reinterpret_cast<JsValueRef>(v);
  int valueInt;
  CHECK_JSRT_EXPECTED(JsNumberToInt(value, &valueInt), napi_number_expected);
  *result = static_cast<int64_t>(valueInt);
  return napi_ok;
}

napi_status napi_get_value_bool(napi_env env, napi_value v, bool* result) {
  CHECK_ARG(result);
  JsValueRef value = reinterpret_cast<JsValueRef>(v);
  CHECK_JSRT_EXPECTED(JsBooleanToBool(value, result), napi_boolean_expected);
  return napi_ok;
}

// Copies a JavaScript string into a LATIN-1 string buffer. The result is the
// number of bytes (excluding the null terminator) copied into buf.
// A sufficient buffer size should be greater than the length of string,
// reserving space for null terminator.
// If bufsize is insufficient, the string will be truncated and null terminated.
// If buf is NULL, this method returns the length of the string (in bytes)
// via the result parameter.
// The result argument is optional unless buf is NULL.
napi_status napi_get_value_string_latin1(napi_env env,
                                         napi_value value,
                                         char* buf,
                                         size_t bufsize,
                                         size_t* result) {
  CHECK_ARG(value);
  JsValueRef js_value = reinterpret_cast<JsValueRef>(value);

  // TODO(boingoing): Convert from UTF-8 to Latin1 encoding.
  if (!buf) {
    CHECK_ARG(result);

    JsErrorCode err = JsCopyString(js_value, nullptr, 0, result);
    if (err != JsErrorInvalidArgument) {
      return napi_set_last_error(err);
    }
  } else {
    size_t count = 0;
    CHECK_JSRT_EXPECTED(
      JsCopyString(js_value, nullptr, 0, &count),
      napi_string_expected);

    if (bufsize <= count) {
      // if bufsize == count there is no space for null terminator
      // Slow path: must implement truncation here.
      char* fullBuffer = static_cast<char *>(malloc(count));
      CHAKRA_VERIFY(fullBuffer != nullptr);

      CHECK_JSRT_EXPECTED(
        JsCopyString(js_value, fullBuffer, count, nullptr),
        napi_string_expected);
      memmove(buf, fullBuffer, sizeof(char) * bufsize);
      free(fullBuffer);

      // Truncate string to the start of the last codepoint
      if (bufsize > 0 &&
          (((buf[bufsize-1] & 0x80) == 0)
            || UTF8_MULTIBYTE_START(buf[bufsize-1]))
        ) {
        // Last byte is a single byte codepoint or
        // starts a multibyte codepoint
        bufsize -= 1;
      } else if (bufsize > 1 && UTF8_MULTIBYTE_START(buf[bufsize-2])) {
        // Second last byte starts a multibyte codepoint,
        bufsize -= 2;
      } else if (bufsize > 2 && UTF8_MULTIBYTE_START(buf[bufsize-3])) {
        // Third last byte starts a multibyte codepoint
        bufsize -= 3;
      } else if (bufsize > 3 && UTF8_MULTIBYTE_START(buf[bufsize-4])) {
        // Fourth last byte starts a multibyte codepoint
        bufsize -= 4;
      }

      buf[bufsize] = '\0';

      if (result) {
        *result = bufsize;
      }

      return napi_ok;
    }

    // Fastpath, result fits in the buffer
    CHECK_JSRT_EXPECTED(
      JsCopyString(js_value, buf, bufsize-1, &count),
      napi_string_expected);

    buf[count] = 0;

    if (result != nullptr) {
      *result = count;
    }
  }

  return napi_ok;
}

// Copies a JavaScript string into a UTF-8 string buffer. The result is the
// number of bytes (excluding the null terminator) copied into buf.
// A sufficient buffer size should be greater than the length of string,
// reserving space for null terminator.
// If bufsize is insufficient, the string will be truncated and null terminated.
// If buf is NULL, this method returns the length of the string (in bytes)
// via the result parameter.
// The result argument is optional unless buf is NULL.
napi_status napi_get_value_string_utf8(napi_env env,
                                       napi_value value,
                                       char* buf,
                                       size_t bufsize,
                                       size_t* result) {
  CHECK_ARG(value);
  JsValueRef js_value = reinterpret_cast<JsValueRef>(value);

  if (!buf) {
    CHECK_ARG(result);

    JsErrorCode err = JsCopyString(js_value, nullptr, 0, result);
    if (err != JsErrorInvalidArgument) {
      return napi_set_last_error(err);
    }
  } else {
    size_t count = 0;
    CHECK_JSRT_EXPECTED(
      JsCopyString(js_value, nullptr, 0, &count),
      napi_string_expected);

    if (bufsize <= count) {
      // if bufsize == count there is no space for null terminator
      // Slow path: must implement truncation here.
      char* fullBuffer = static_cast<char *>(malloc(count));
      CHAKRA_VERIFY(fullBuffer != nullptr);

      CHECK_JSRT_EXPECTED(
        JsCopyString(js_value, fullBuffer, count, nullptr),
        napi_string_expected);
      memmove(buf, fullBuffer, sizeof(char) * bufsize);
      free(fullBuffer);

      // Truncate string to the start of the last codepoint
      if (bufsize > 0 &&
          (((buf[bufsize-1] & 0x80) == 0)
           || UTF8_MULTIBYTE_START(buf[bufsize-1]))
        ) {
        // Last byte is a single byte codepoint or
        // starts a multibyte codepoint
        bufsize -= 1;
      } else if (bufsize > 1 && UTF8_MULTIBYTE_START(buf[bufsize-2])) {
        // Second last byte starts a multibyte codepoint,
        bufsize -= 2;
      } else if (bufsize > 2 && UTF8_MULTIBYTE_START(buf[bufsize-3])) {
        // Third last byte starts a multibyte codepoint
        bufsize -= 3;
      } else if (bufsize > 3 && UTF8_MULTIBYTE_START(buf[bufsize-4])) {
        // Fourth last byte starts a multibyte codepoint
        bufsize -= 4;
      }

      buf[bufsize] = '\0';

      if (result) {
        *result = bufsize;
      }

      return napi_ok;
    }

    // Fastpath, result fits in the buffer
    CHECK_JSRT_EXPECTED(
      JsCopyString(js_value, buf, bufsize-1, &count),
      napi_string_expected);

    buf[count] = 0;

    if (result != nullptr) {
      *result = count;
    }
  }

  return napi_ok;
}

// Copies a JavaScript string into a UTF-16 string buffer. The result is the
// number of 2-byte code units (excluding the null terminator) copied into buf.
// A sufficient buffer size should be greater than the length of string,
// reserving space for null terminator.
// If bufsize is insufficient, the string will be truncated and null terminated.
// If buf is NULL, this method returns the length of the string (in 2-byte
// code units) via the result parameter.
// The result argument is optional unless buf is NULL.
napi_status napi_get_value_string_utf16(napi_env env,
                                        napi_value value,
                                        char16_t* buf,
                                        size_t bufsize,
                                        size_t* result) {
  CHECK_ARG(value);
  JsValueRef js_value = reinterpret_cast<JsValueRef>(value);

  if (!buf) {
    CHECK_ARG(result);

    JsErrorCode err = JsCopyStringUtf16(js_value, 0, -1, nullptr, result);
    if (err != JsErrorInvalidArgument) {
      return napi_set_last_error(err);
    }
  } else {
    size_t copied = 0;
    CHECK_JSRT_EXPECTED(
      JsCopyStringUtf16(
        js_value,
        0,
        static_cast<int>(bufsize - 1),
        reinterpret_cast<uint16_t*>(buf),
        &copied),
      napi_string_expected);

    if (copied < bufsize - 1) {
      buf[copied] = 0;
    } else {
      buf[bufsize - 1] = 0;
    }

    if (result != nullptr) {
      *result = copied;
    }
  }

  return napi_ok;
}

napi_status napi_coerce_to_number(napi_env env,
                                  napi_value v,
                                  napi_value* result) {
  CHECK_ARG(result);
  JsValueRef value = reinterpret_cast<JsValueRef>(v);
  CHECK_JSRT(
    JsConvertValueToNumber(value, reinterpret_cast<JsValueRef*>(result)));
  return napi_ok;
}

napi_status napi_coerce_to_bool(napi_env env,
                                napi_value v,
                                napi_value* result) {
  CHECK_ARG(result);
  JsValueRef value = reinterpret_cast<JsValueRef>(v);
  CHECK_JSRT(
    JsConvertValueToBoolean(value, reinterpret_cast<JsValueRef*>(result)));
  return napi_ok;
}

napi_status napi_coerce_to_object(napi_env env,
                                  napi_value v,
                                  napi_value* result) {
  CHECK_ARG(result);
  JsValueRef value = reinterpret_cast<JsValueRef>(v);
  CHECK_JSRT(
    JsConvertValueToObject(value, reinterpret_cast<JsValueRef*>(result)));
  return napi_ok;
}

napi_status napi_coerce_to_string(napi_env env,
                                  napi_value v,
                                  napi_value* result) {
  CHECK_ARG(result);
  JsValueRef value = reinterpret_cast<JsValueRef>(v);
  CHECK_JSRT(
    JsConvertValueToString(value, reinterpret_cast<JsValueRef*>(result)));
  return napi_ok;
}

napi_status napi_wrap(napi_env env,
                      napi_value js_object,
                      void* native_object,
                      napi_finalize finalize_cb,
                      void* finalize_hint,
                      napi_ref* result) {
  JsValueRef value = reinterpret_cast<JsValueRef>(js_object);

  JsValueRef wrapper = JS_INVALID_REFERENCE;
  CHECK_NAPI(jsrtimpl::FindWrapper(value, &wrapper));
  RETURN_STATUS_IF_FALSE(wrapper == JS_INVALID_REFERENCE, napi_invalid_arg);

  jsrtimpl::ExternalData* externalData = new jsrtimpl::ExternalData(
    env, native_object, finalize_cb, finalize_hint);
  if (externalData == nullptr) return napi_set_last_error(napi_generic_failure);

  // Create an external object that will hold the external data pointer.
  JsValueRef external = JS_INVALID_REFERENCE;
  CHECK_JSRT(JsCreateExternalObject(
    externalData, jsrtimpl::ExternalData::Finalize, &external));

  // Insert the external object into the value's prototype chain.
  JsValueRef valuePrototype = JS_INVALID_REFERENCE;
  CHECK_JSRT(JsGetPrototype(value, &valuePrototype));
  CHECK_JSRT(JsSetPrototype(external, valuePrototype));
  CHECK_JSRT(JsSetPrototype(value, external));

  if (result != nullptr) {
    CHECK_NAPI(napi_create_reference(env, js_object, 0, result));
  }

  return napi_ok;
}

napi_status napi_unwrap(napi_env env, napi_value js_object, void** result) {
  JsValueRef value = reinterpret_cast<JsValueRef>(js_object);

  jsrtimpl::ExternalData* externalData = nullptr;
  CHECK_NAPI(jsrtimpl::Unwrap(value, &externalData));

  *result = (externalData != nullptr ? externalData->Data() : nullptr);

  return napi_ok;
}

napi_status napi_remove_wrap(napi_env env, napi_value js_object,
                             void** result) {
  JsValueRef value = reinterpret_cast<JsValueRef>(js_object);

  jsrtimpl::ExternalData* externalData = nullptr;
  JsValueRef parent = JS_INVALID_REFERENCE;
  JsValueRef wrapper = JS_INVALID_REFERENCE;
  CHECK_NAPI(jsrtimpl::Unwrap(value, &externalData, &wrapper, &parent));
  RETURN_STATUS_IF_FALSE(parent != JS_INVALID_REFERENCE, napi_invalid_arg);
  RETURN_STATUS_IF_FALSE(wrapper != JS_INVALID_REFERENCE, napi_invalid_arg);

  // Remove the external from the prototype chain
  JsValueRef wrapperProto = JS_INVALID_REFERENCE;
  CHECK_JSRT(JsGetPrototype(wrapper, &wrapperProto));
  CHECK_JSRT(JsSetPrototype(parent, wrapperProto));

  // Clear the external data from the object
  CHECK_JSRT(JsSetExternalData(wrapper, nullptr));

  if (externalData != nullptr) {
    *result = externalData->Data();
    delete externalData;
  } else {
    *result = nullptr;
  }

  return napi_ok;
}

napi_status napi_create_external(napi_env env,
                                 void* data,
                                 napi_finalize finalize_cb,
                                 void* finalize_hint,
                                 napi_value* result) {
  CHECK_ARG(result);

  jsrtimpl::ExternalData* externalData = new jsrtimpl::ExternalData(
    env, data, finalize_cb, finalize_hint);
  if (externalData == nullptr) return napi_set_last_error(napi_generic_failure);

  CHECK_JSRT(JsCreateExternalObject(
    externalData,
    jsrtimpl::ExternalData::Finalize,
    reinterpret_cast<JsValueRef*>(result)));

  return napi_ok;
}

napi_status napi_get_value_external(napi_env env, napi_value v, void** result) {
  CHECK_ARG(result);

  jsrtimpl::ExternalData* externalData;
  CHECK_JSRT(JsGetExternalData(
    reinterpret_cast<JsValueRef>(v),
    reinterpret_cast<void**>(&externalData)));

  *result = (externalData != nullptr ? externalData->Data() : nullptr);

  return napi_ok;
}

// Set initial_refcount to 0 for a weak reference, >0 for a strong reference.
napi_status napi_create_reference(napi_env env,
                                  napi_value v,
                                  uint32_t initial_refcount,
                                  napi_ref* result) {
  CHECK_ARG(result);

  JsValueRef strongRef = reinterpret_cast<JsValueRef>(v);
  JsWeakRef weakRef = nullptr;
  CHECK_JSRT(JsCreateWeakReference(strongRef, &weakRef));

  // Prevent the reference itself from being collected until it is explicitly
  // deleted.
  CHECK_JSRT(JsAddRef(weakRef, nullptr));

  // Apply the initial refcount to the target value.
  for (uint32_t i = 0; i < initial_refcount; i++) {
    CHECK_JSRT(JsAddRef(strongRef, nullptr));

    // Also increment the refcount of the reference by the same amount,
    // to enable reverting the target's refcount when the reference is deleted.
    CHECK_JSRT(JsAddRef(weakRef, nullptr));
  }

  *result = reinterpret_cast<napi_ref>(weakRef);
  return napi_ok;
}

// Deletes a reference. The referenced value is released, and may be GC'd
// unless there are other references to it.
napi_status napi_delete_reference(napi_env env, napi_ref ref) {
  JsRef weakRef = reinterpret_cast<JsRef>(ref);

  unsigned int count;
  CHECK_JSRT(JsRelease(weakRef, &count));

  if (count > 0) {
    // Revert this reference's contribution to the target's refcount.
    JsValueRef target;
    CHECK_JSRT(JsGetWeakReferenceValue(weakRef, &target));

    do {
      CHECK_JSRT(JsRelease(weakRef, &count));
      CHECK_JSRT(JsRelease(target, nullptr));
    } while (count > 0);
  }

  return napi_ok;
}

// Increments the reference count, optionally returning the resulting count.
// After this call the reference will be a strong reference because its refcount
// is >0, and the referenced object is effectively "pinned". Calling this when
// the refcount is 0 and the target is unavailable results in an error.
napi_status napi_reference_ref(napi_env env, napi_ref ref, uint32_t* result) {
  JsRef weakRef = reinterpret_cast<JsRef>(ref);

  JsValueRef target;
  CHECK_JSRT(JsGetWeakReferenceValue(weakRef, &target));

  if (target == JS_INVALID_REFERENCE) {
    // Called napi_reference_addref when the target is unavailable!
    return napi_set_last_error(napi_generic_failure);
  }

  CHECK_JSRT(JsAddRef(target, nullptr));

  uint32_t count;
  CHECK_JSRT(JsAddRef(weakRef, &count));

  if (result != nullptr) {
    *result = count - 1;
  }

  return napi_ok;
}

// Decrements the reference count, optionally returning the resulting count.
// If the result is 0 the reference is now weak and the object may be GC'd at
// any time if there are no other references. Calling this when the refcount
// is already 0 results in an error.
napi_status napi_reference_unref(napi_env env, napi_ref ref, uint32_t* result) {
  JsRef weakRef = reinterpret_cast<JsRef>(ref);

  JsValueRef target;
  CHECK_JSRT(JsGetWeakReferenceValue(weakRef, &target));

  if (target == JS_INVALID_REFERENCE) {
    return napi_set_last_error(napi_generic_failure);
  }

  uint32_t count;
  CHECK_JSRT(JsRelease(weakRef, &count));
  if (count == 0) {
    // Called napi_release_reference too many times on a reference!
    return napi_set_last_error(napi_generic_failure);
  }

  CHECK_JSRT(JsRelease(target, nullptr));

  if (result != nullptr) {
    *result = count - 1;
  }

  return napi_ok;
}

// Attempts to get a referenced value. If the reference is weak, the value
// might no longer be available, in that case the call is still successful but
// the result is NULL.
napi_status napi_get_reference_value(napi_env env,
                                     napi_ref ref,
                                     napi_value* result) {
  CHECK_ARG(result);
  JsWeakRef weakRef = reinterpret_cast<JsWeakRef>(ref);
  CHECK_JSRT(
    JsGetWeakReferenceValue(weakRef, reinterpret_cast<JsValueRef*>(result)));
  return napi_ok;
}

/*********Stub implementation of handle scope apis' for JSRT***********/
napi_status napi_open_handle_scope(napi_env env, napi_handle_scope* result) {
  CHECK_ARG(result);
  *result = nullptr;
  return napi_ok;
}

napi_status napi_close_handle_scope(napi_env env, napi_handle_scope) {
  return napi_ok;
}

napi_status napi_open_escapable_handle_scope(napi_env env,
  napi_escapable_handle_scope* result) {
  CHECK_ARG(result);
  *result = nullptr;
  return napi_ok;
}

napi_status napi_close_escapable_handle_scope(napi_env env,
  napi_escapable_handle_scope scope) {
  return napi_ok;
}

// This one will return escapee value as this is called from leveldown db.
napi_status napi_escape_handle(napi_env env,
                               napi_escapable_handle_scope scope,
                               napi_value escapee,
                               napi_value* result) {
  CHECK_ARG(result);
  *result = escapee;
  return napi_ok;
}
/**************************************************************/

napi_status napi_new_instance(napi_env env,
                              napi_value constructor,
                              size_t argc,
                              const napi_value* argv,
                              napi_value* result) {
  CHECK_ARG(result);
  JsValueRef function = reinterpret_cast<JsValueRef>(constructor);
  std::vector<JsValueRef> args(argc + 1);
  CHECK_JSRT(JsGetUndefinedValue(&args[0]));
  for (size_t i = 0; i < argc; i++) {
    args[i + 1] = reinterpret_cast<JsValueRef>(argv[i]);
  }
  CHECK_JSRT(JsConstructObject(
    function,
    args.data(),
    static_cast<uint16_t>(argc + 1),
    reinterpret_cast<JsValueRef*>(result)));
  return napi_ok;
}

napi_status napi_make_external(napi_env env, napi_value v, napi_value* result) {
  CHECK_ARG(result);
  JsValueRef externalObj;
  CHECK_JSRT(JsCreateExternalObject(NULL, NULL, &externalObj));
  CHECK_JSRT(JsSetPrototype(externalObj, reinterpret_cast<JsValueRef>(v)));
  *result = reinterpret_cast<napi_value>(externalObj);
  return napi_ok;
}

napi_status napi_async_init(napi_env env,
  napi_value async_resource,
  napi_value async_resource_name,
  napi_async_context* result) {
  CHECK_ENV(env);
  CHECK_ARG(async_resource_name);
  CHECK_ARG(result);

  v8::Isolate* isolate = env->isolate;

  v8::Local<v8::Object> resource;
  if (async_resource != nullptr) {
    resource =
      v8impl::V8LocalValueFromJsValue(async_resource).As<v8::Object>();
  } else {
    resource = v8::Object::New(isolate);
  }

  v8::Local<v8::String> resource_name =
    v8impl::V8LocalValueFromJsValue(async_resource_name).As<v8::String>();

  // TODO(jasongin): Consider avoiding allocation here by using
  // a tagged pointer with 2^31 bit fields instead.
  node::async_context* async_context = new node::async_context();

  // TODO(digitalinfinty): EmitAsyncInit might be better served if it
  // tool in napi_values instead of v8 locals?
  *async_context = node::EmitAsyncInit(isolate, resource, resource_name);
  *result = reinterpret_cast<napi_async_context>(async_context);

  napi_clear_last_error();
  return napi_ok;
}

napi_status napi_async_destroy(napi_env env,
  napi_async_context async_context) {
  CHECK_ENV(env);
  CHECK_ARG(async_context);

  v8::Isolate* isolate = env->isolate;
  node::async_context* node_async_context =
    reinterpret_cast<node::async_context*>(async_context);
  node::EmitAsyncDestroy(isolate, *node_async_context);

  napi_clear_last_error();
  return napi_ok;
}

napi_status napi_make_callback(napi_env env,
                               napi_async_context async_context,
                               napi_value recv,
                               napi_value func,
                               size_t argc,
                               const napi_value* argv,
                               napi_value* result) {
  v8::Isolate* isolate = v8impl::V8IsolateFromJsEnv(env);
  v8::Local<v8::Object> v8recv =
    v8impl::V8LocalValueFromJsValue(recv).As<v8::Object>();
  v8::Local<v8::Function> v8func =
    v8impl::V8LocalValueFromJsValue(func).As<v8::Function>();
  v8::Local<v8::Value>* v8argv =
    reinterpret_cast<v8::Local<v8::Value>*>(const_cast<napi_value*>(argv));

  node::async_context* node_async_context =
    reinterpret_cast<node::async_context*>(async_context);
  if (node_async_context == nullptr) {
    static node::async_context empty_context = { 0, 0 };
    node_async_context = &empty_context;
  }

  // TODO(jasongin): Expose JSRT or N-API version of node::MakeCallback?
  v8::MaybeLocal<v8::Value> v8result =
    node::MakeCallback(isolate, v8recv, v8func, argc, v8argv,
                       *node_async_context);

  if (v8result.IsEmpty()) {
      return napi_set_last_error(napi_generic_failure);
  }

  if (result != nullptr) {
    *result = v8impl::JsValueFromV8LocalValue(v8result.ToLocalChecked());
  }

  return napi_ok;
}

struct ArrayBufferFinalizeInfo {
  void *data;

  void Free() {
    free(data);
    delete this;
  }
};

void CALLBACK ExternalArrayBufferFinalizeCallback(void *data) {
  static_cast<ArrayBufferFinalizeInfo*>(data)->Free();
}

napi_status napi_create_buffer(napi_env env,
                               size_t length,
                               void** data,
                               napi_value* result) {
  CHECK_ARG(result);
  // TODO(tawoll): Replace v8impl with jsrt-based version.

  v8::MaybeLocal<v8::Object> maybe =
    node::Buffer::New(v8impl::V8IsolateFromJsEnv(env), length);
  if (maybe.IsEmpty()) {
    return napi_generic_failure;
  }

  v8::Local<v8::Object> buffer = maybe.ToLocalChecked();
  if (data != nullptr) {
    *data = node::Buffer::Data(buffer);
  }

  *result = v8impl::JsValueFromV8LocalValue(buffer);
  return napi_ok;
}

napi_status napi_create_external_buffer(napi_env env,
                                        size_t length,
                                        void* data,
                                        napi_finalize finalize_cb,
                                        void* finalize_hint,
                                        napi_value* result) {
  CHECK_ARG(result);
  // TODO(tawoll): Replace v8impl with jsrt-based version.

  jsrtimpl::ExternalData* externalData = new jsrtimpl::ExternalData(
    env, data, finalize_cb, finalize_hint);
  v8::MaybeLocal<v8::Object> maybe = node::Buffer::New(
    v8impl::V8IsolateFromJsEnv(env),
    static_cast<char*>(data),
    length,
    jsrtimpl::ExternalData::FinalizeBuffer,
    externalData);

  if (maybe.IsEmpty()) {
    return napi_generic_failure;
  }

  *result = v8impl::JsValueFromV8LocalValue(maybe.ToLocalChecked());
  return napi_ok;
}

napi_status napi_create_buffer_copy(napi_env env,
                                    size_t length,
                                    const void* data,
                                    void** result_data,
                                    napi_value* result) {
  CHECK_ARG(result);
  // TODO(tawoll): Implement node::Buffer in terms of napi to avoid using
  // chakra shim here.

  v8::MaybeLocal<v8::Object> maybe = node::Buffer::Copy(
    v8impl::V8IsolateFromJsEnv(env), static_cast<const char*>(data), length);
  if (maybe.IsEmpty()) {
    return napi_generic_failure;
  }

  v8::Local<v8::Object> buffer = maybe.ToLocalChecked();
  *result = v8impl::JsValueFromV8LocalValue(buffer);

  if (result_data != nullptr) {
    *result_data = node::Buffer::Data(buffer);
  }

  return napi_ok;
}

napi_status napi_is_buffer(napi_env env, napi_value v, bool* result) {
  CHECK_ARG(result);
  JsValueRef typedArray = reinterpret_cast<JsValueRef>(v);
  JsValueType objectType;
  CHECK_JSRT(JsGetValueType(typedArray, &objectType));
  if (objectType != JsTypedArray) {
    *result = false;
    return napi_ok;
  }
  JsTypedArrayType arrayType;
  CHECK_JSRT(
    JsGetTypedArrayInfo(typedArray, &arrayType, nullptr, nullptr, nullptr));
  *result = (arrayType == JsArrayTypeUint8);
  return napi_ok;
}

napi_status napi_get_buffer_info(napi_env env,
                                 napi_value value,
                                 void** data,
                                 size_t* length) {
  JsValueRef typedArray = reinterpret_cast<JsValueRef>(value);
  JsValueRef arrayBuffer;
  unsigned int byteOffset;
  ChakraBytePtr buffer;
  unsigned int bufferLength;
  CHECK_JSRT(JsGetTypedArrayInfo(
    typedArray, nullptr, &arrayBuffer, &byteOffset, nullptr));
  CHECK_JSRT(JsGetArrayBufferStorage(arrayBuffer, &buffer, &bufferLength));

  if (data != nullptr) {
    *data = static_cast<void*>(buffer + byteOffset);
  }

  if (length != nullptr) {
    *length = static_cast<size_t>(bufferLength);
  }

  return napi_ok;
}

napi_status napi_is_exception_pending(napi_env env, bool* result) {
  CHECK_ARG(result);
  CHECK_JSRT(JsHasException(result));
  return napi_ok;
}

napi_status napi_get_and_clear_last_exception(napi_env env,
                                              napi_value* result) {
  CHECK_ARG(result);
  CHECK_JSRT(JsGetAndClearException(reinterpret_cast<JsValueRef*>(result)));
  if (*result == nullptr) {
    // Is this necessary?
    CHECK_NAPI(napi_get_undefined(env, result));
  }
  return napi_ok;
}

napi_status napi_is_arraybuffer(napi_env env, napi_value value, bool* result) {
  CHECK_ARG(result);

  JsValueRef jsValue = reinterpret_cast<JsValueRef>(value);
  JsValueType valueType;
  CHECK_JSRT(JsGetValueType(jsValue, &valueType));

  *result = (valueType == JsArrayBuffer);
  return napi_ok;
}

napi_status napi_create_arraybuffer(napi_env env,
                                    size_t byte_length,
                                    void** data,
                                    napi_value* result) {
  CHECK_ARG(result);

  JsValueRef arrayBuffer;
  CHECK_JSRT(
    JsCreateArrayBuffer(static_cast<unsigned int>(byte_length), &arrayBuffer));

  if (data != nullptr) {
    CHECK_JSRT(JsGetArrayBufferStorage(
      arrayBuffer,
      reinterpret_cast<ChakraBytePtr*>(data),
      reinterpret_cast<unsigned int*>(&byte_length)));
  }

  *result = reinterpret_cast<napi_value>(arrayBuffer);
  return napi_ok;
}

napi_status napi_create_external_arraybuffer(napi_env env,
                                             void* external_data,
                                             size_t byte_length,
                                             napi_finalize finalize_cb,
                                             void* finalize_hint,
                                             napi_value* result) {
  CHECK_ARG(result);

  jsrtimpl::ExternalData* externalData = new jsrtimpl::ExternalData(
    env, external_data, finalize_cb, finalize_hint);
  if (externalData == nullptr) return napi_set_last_error(napi_generic_failure);

  JsValueRef arrayBuffer;
  CHECK_JSRT(JsCreateExternalArrayBuffer(
    external_data,
    static_cast<unsigned int>(byte_length),
    jsrtimpl::ExternalData::Finalize,
    externalData,
    &arrayBuffer));

  *result = reinterpret_cast<napi_value>(arrayBuffer);
  return napi_ok;
}

napi_status napi_get_arraybuffer_info(napi_env env,
                                      napi_value arraybuffer,
                                      void** data,
                                      size_t* byte_length) {
  ChakraBytePtr storageData;
  unsigned int storageLength;
  CHECK_JSRT(JsGetArrayBufferStorage(
    reinterpret_cast<JsValueRef>(arraybuffer),
    &storageData,
    &storageLength));

  if (data != nullptr) {
    *data = reinterpret_cast<void*>(storageData);
  }

  if (byte_length != nullptr) {
    *byte_length = static_cast<size_t>(storageLength);
  }

  return napi_ok;
}

napi_status napi_is_typedarray(napi_env env, napi_value value, bool* result) {
  CHECK_ARG(result);

  JsValueRef jsValue = reinterpret_cast<JsValueRef>(value);
  JsValueType valueType;
  CHECK_JSRT(JsGetValueType(jsValue, &valueType));

  *result = (valueType == JsTypedArray);
  return napi_ok;
}

napi_status napi_create_typedarray(napi_env env,
                                   napi_typedarray_type type,
                                   size_t length,
                                   napi_value arraybuffer,
                                   size_t byte_offset,
                                   napi_value* result) {
  CHECK_ARG(result);

  JsTypedArrayType jsType;
  switch (type) {
    case napi_int8_array:
      jsType = JsArrayTypeInt8;
      break;
    case napi_uint8_array:
      jsType = JsArrayTypeUint8;
      break;
    case napi_uint8_clamped_array:
      jsType = JsArrayTypeUint8Clamped;
      break;
    case napi_int16_array:
      jsType = JsArrayTypeInt16;
      break;
    case napi_uint16_array:
      jsType = JsArrayTypeUint16;
      break;
    case napi_int32_array:
      jsType = JsArrayTypeInt32;
      break;
    case napi_uint32_array:
      jsType = JsArrayTypeUint32;
      break;
    case napi_float32_array:
      jsType = JsArrayTypeFloat32;
      break;
    case napi_float64_array:
      jsType = JsArrayTypeFloat64;
      break;
    default:
      return napi_set_last_error(napi_invalid_arg);
  }

  JsValueRef jsArrayBuffer = reinterpret_cast<JsValueRef>(arraybuffer);

  CHECK_JSRT(JsCreateTypedArray(
    jsType,
    jsArrayBuffer,
    static_cast<unsigned int>(byte_offset),
    static_cast<unsigned int>(length),
    reinterpret_cast<JsValueRef*>(result)));

  return napi_ok;
}

napi_status napi_get_typedarray_info(napi_env env,
                                     napi_value typedarray,
                                     napi_typedarray_type* type,
                                     size_t* length,
                                     void** data,
                                     napi_value* arraybuffer,
                                     size_t* byte_offset) {
  JsTypedArrayType jsType;
  JsValueRef jsArrayBuffer;
  unsigned int byteOffset;
  unsigned int byteLength;
  ChakraBytePtr bufferData;
  unsigned int bufferLength;
  int elementSize;

  CHECK_JSRT(JsGetTypedArrayInfo(
    reinterpret_cast<JsValueRef>(typedarray),
    &jsType,
    &jsArrayBuffer,
    &byteOffset,
    &byteLength));

  CHECK_JSRT(JsGetTypedArrayStorage(
    reinterpret_cast<JsValueRef>(typedarray),
    &bufferData,
    &bufferLength,
    &jsType,
    &elementSize));

  if (type != nullptr) {
    switch (jsType) {
      case JsArrayTypeInt8:
        *type = napi_int8_array;
        break;
      case JsArrayTypeUint8:
        *type = napi_uint8_array;
        break;
      case JsArrayTypeUint8Clamped:
        *type = napi_uint8_clamped_array;
        break;
      case JsArrayTypeInt16:
        *type = napi_int16_array;
        break;
      case JsArrayTypeUint16:
        *type = napi_uint16_array;
        break;
      case JsArrayTypeInt32:
        *type = napi_int32_array;
        break;
      case JsArrayTypeUint32:
        *type = napi_uint32_array;
        break;
      case JsArrayTypeFloat32:
        *type = napi_float32_array;
        break;
      case JsArrayTypeFloat64:
        *type = napi_float64_array;
        break;
      default:
        return napi_set_last_error(napi_generic_failure);
    }
  }

  if (length != nullptr) {
    *length = static_cast<size_t>(byteLength / elementSize);
  }

  if (data != nullptr) {
    *data = static_cast<uint8_t*>(bufferData);
  }

  if (arraybuffer != nullptr) {
    *arraybuffer = reinterpret_cast<napi_value>(jsArrayBuffer);
  }

  if (byte_offset != nullptr) {
    *byte_offset = static_cast<size_t>(byteOffset);
  }

  return napi_ok;
}

napi_status napi_create_dataview(napi_env env,
                                 size_t byte_length,
                                 napi_value arraybuffer,
                                 size_t byte_offset,
                                 napi_value* result) {
  CHECK_ARG(result);

  JsValueRef jsArrayBuffer = reinterpret_cast<JsValueRef>(arraybuffer);

  CHECK_JSRT(JsCreateDataView(
    jsArrayBuffer,
    static_cast<unsigned int>(byte_offset),
    static_cast<unsigned int>(byte_length),
    reinterpret_cast<JsValueRef*>(result)));

  return napi_ok;
}

napi_status napi_is_dataview(napi_env env, napi_value value, bool* result) {
  CHECK_ARG(result);

  JsValueRef jsValue = reinterpret_cast<JsValueRef>(value);
  JsValueType valueType;
  CHECK_JSRT(JsGetValueType(jsValue, &valueType));

  *result = (valueType == JsDataView);
  return napi_ok;
}

napi_status napi_get_dataview_info(napi_env env,
                                   napi_value dataview,
                                   size_t* byte_length,
                                   void** data,
                                   napi_value* arraybuffer,
                                   size_t* byte_offset) {
  JsValueRef jsArrayBuffer = JS_INVALID_REFERENCE;
  unsigned int byteOffset = 0;
  unsigned int byteLength = 0;
  ChakraBytePtr bufferData = nullptr;
  unsigned int bufferLength = 0;

  JsValueRef jsDataView = reinterpret_cast<JsValueRef>(dataview);

  CHECK_JSRT(JsGetDataViewInfo(
    jsDataView,
    &jsArrayBuffer,
    &byteOffset,
    &byteLength));

  CHECK_JSRT(JsGetDataViewStorage(
    jsDataView,
    &bufferData,
    &bufferLength));

  if (byte_length != nullptr) {
    *byte_length = static_cast<size_t>(byteLength);
  }

  if (data != nullptr) {
    *data = static_cast<uint8_t*>(bufferData);
  }

  if (arraybuffer != nullptr) {
    *arraybuffer = reinterpret_cast<napi_value>(jsArrayBuffer);
  }

  if (byte_offset != nullptr) {
    *byte_offset = static_cast<size_t>(byteOffset);
  }

  return napi_ok;
}

napi_status napi_get_version(napi_env env, uint32_t* result) {
  CHECK_ARG(result);
  *result = NAPI_VERSION;
  return napi_ok;
}

napi_status napi_get_node_version(napi_env env,
                                  const napi_node_version** result) {
  CHECK_ARG(result);
  static const napi_node_version version = {
    NODE_MAJOR_VERSION,
    NODE_MINOR_VERSION,
    NODE_PATCH_VERSION,
    NODE_RELEASE
  };
  *result = &version;
  return napi_ok;
}

napi_status napi_adjust_external_memory(napi_env env,
                                        int64_t change_in_bytes,
                                        int64_t* adjusted_value) {
  CHECK_ARG(change_in_bytes);
  CHECK_ARG(adjusted_value);

  // TODO(jackhorton): Determine if Chakra needs or is able to do anything here

  return napi_ok;
}

JsValueRef runScriptSourceUrl = JS_INVALID_REFERENCE;

napi_status napi_run_script(napi_env env,
                            napi_value script,
                            napi_value* result) {
  CHECK_ARG(script);
  CHECK_ARG(result);
  JsValueRef scriptVar = reinterpret_cast<JsValueRef>(script);

  if (runScriptSourceUrl == JS_INVALID_REFERENCE) {
    const char * napiScriptString = "NAPI run script";
    CHECK_JSRT(JsCreateString(napiScriptString,
                   strlen(napiScriptString),
                   &runScriptSourceUrl));
  }

  CHECK_JSRT_EXPECTED(JsRun(scriptVar,
                            JS_SOURCE_CONTEXT_NONE,
                            runScriptSourceUrl,
                            JsParseScriptAttributeNone,
                            reinterpret_cast<JsValueRef*>(result)),
                      napi_string_expected);

  return napi_ok;
}

namespace uvimpl {

napi_status ConvertUVErrorCode(int code) {
  switch (code) {
  case 0:
    return napi_ok;
  case UV_EINVAL:
    return napi_invalid_arg;
  case UV_ECANCELED:
    return napi_cancelled;
  }

  return napi_generic_failure;
}

// Wrapper around uv_work_t which calls user-provided callbacks.
class Work: public node::AsyncResource {
 private:
  explicit Work(napi_env env,
    v8::Local<v8::Object> async_resource,
    v8::Local<v8::String> async_resource_name,
    napi_async_execute_callback execute = nullptr,
    napi_async_complete_callback complete = nullptr,
    void* data = nullptr)
    : AsyncResource(env->isolate,  async_resource, async_resource_name),
    _env(env),
    _data(data),
    _execute(execute),
    _complete(complete) {
    memset(&_request, 0, sizeof(_request));
    _request.data = this;
  }

  ~Work() { }

 public:
  static Work* New(napi_env env,
    napi_value async_resource,
    napi_value async_resource_name,
    napi_async_execute_callback execute,
    napi_async_complete_callback complete,
    void* data) {
    v8::Local<v8::Object> async_resource_local =
      v8impl::V8LocalValueFromJsValue(async_resource).As<v8::Object>();
    v8::Local<v8::String> async_resource_name_local =
      v8impl::V8LocalValueFromJsValue(async_resource_name).As<v8::String>();

    return new Work(env, async_resource_local, async_resource_name_local,
      execute, complete, data);
  }

  static void Delete(Work* work) {
    delete work;
  }

  static void ExecuteCallback(uv_work_t* req) {
    Work* work = static_cast<Work*>(req->data);
    work->_execute(work->_env, work->_data);
  }

  static void Fatal() {
    napi_fatal_error(nullptr, "[napi] CompleteCallback failed "
      "but exception could not be propagated");
  }

  static void CompleteCallback(uv_work_t* req, int status) {
    Work* work = static_cast<Work*>(req->data);

    if (work->_complete != nullptr) {
      CallbackScope callback_scope(work);

      work->_complete(work->_env, ConvertUVErrorCode(status), work->_data);

      bool hasException;
      JsErrorCode errorCode = JsHasException(&hasException);
      if (errorCode != JsNoError || !hasException) {
        return;
      }

      JsValueRef metadata;
      if (JsGetAndClearExceptionWithMetadata(&metadata) != JsNoError) {
        Fatal();
        return;
      }

      JsValueRef exception;
      JsPropertyIdRef exProp;

      if (JsCreatePropertyId("exception", -1, &exProp) != JsNoError) {
        Fatal();
        return;
      }

      if (JsGetProperty(metadata, exProp, &exception) != JsNoError) {
        Fatal();
        return;
      }

      // TODO(digitalinfinity):
      // Taking a dependency on v8::Local::New for expediency here
      // Remove this once node::FatalException is clean
      v8::Local<v8::Message> message =
        v8::Local<v8::Message>::New(metadata);
      v8::Local<v8::Object> exceptionObject =
        v8impl::V8LocalValueFromJsValue(
          reinterpret_cast<napi_value>(exception)).As<v8::Object>();

      node::FatalException(work->_env->isolate, exceptionObject, message);
    }
  }

  uv_work_t* Request() {
    return &_request;
  }

 private:
  napi_env _env;
  void* _data;
  uv_work_t _request;
  napi_async_execute_callback _execute;
  napi_async_complete_callback _complete;
};

}  // end of namespace uvimpl

#define CALL_UV(condition)                                              \
  do {                                                                  \
    int result = (condition);                                           \
    napi_status status = uvimpl::ConvertUVErrorCode(result);            \
    if (status != napi_ok) {                                            \
      return napi_set_last_error(status, result);                       \
    }                                                                   \
  } while (0)

napi_status napi_create_async_work(napi_env env,
  napi_value async_resource,
  napi_value async_resource_name,
  napi_async_execute_callback execute,
  napi_async_complete_callback complete,
  void* data,
  napi_async_work* result) {
  CHECK_ARG(execute);
  CHECK_ARG(result);

  napi_value resource;
  if (async_resource != nullptr) {
    CHECK_NAPI(napi_coerce_to_object(env, async_resource, &resource));
  } else {
    CHECK_NAPI(napi_create_object(env, &resource));
  }

  napi_value resource_name;
  CHECK_NAPI(napi_coerce_to_string(env, async_resource_name, &resource_name));

  uvimpl::Work* work =
    uvimpl::Work::New(env, resource, resource_name, execute, complete, data);

  *result = reinterpret_cast<napi_async_work>(work);

  return napi_ok;
}

napi_status napi_delete_async_work(napi_env env, napi_async_work work) {
  CHECK_ARG(work);

  uvimpl::Work::Delete(reinterpret_cast<uvimpl::Work*>(work));

  return napi_ok;
}

napi_status napi_queue_async_work(napi_env env, napi_async_work work) {
  CHECK_ARG(work);

  // Consider: Encapsulate the uv_loop_t into an opaque pointer parameter.
  // Currently the environment event loop is the same as the UV default loop.
  // Someday (if node ever supports multiple isolates), it may be better to get
  // the loop from node::Environment::GetCurrent(env->isolate)->event_loop();
  uv_loop_t* event_loop = uv_default_loop();

  uvimpl::Work* w = reinterpret_cast<uvimpl::Work*>(work);

  CALL_UV(uv_queue_work(event_loop,
    w->Request(),
    uvimpl::Work::ExecuteCallback,
    uvimpl::Work::CompleteCallback));

  return napi_ok;
}

napi_status napi_cancel_async_work(napi_env env, napi_async_work work) {
  CHECK_ARG(work);

  uvimpl::Work* w = reinterpret_cast<uvimpl::Work*>(work);

  CALL_UV(uv_cancel(reinterpret_cast<uv_req_t*>(w->Request())));

  return napi_ok;
}

NAPI_EXTERN napi_status napi_create_promise(napi_env env,
                                            napi_deferred* deferred,
                                            napi_value* promise) {
  CHECK_ARG(deferred);
  CHECK_ARG(promise);

  JsValueRef js_promise, resolve, reject, container;
  napi_ref ref;
  napi_value js_deferred;

  CHECK_JSRT(JsCreatePromise(&js_promise, &resolve, &reject));

  CHECK_JSRT(JsCreateObject(&container));
  js_deferred = reinterpret_cast<napi_value>(container);

  CHECK_NAPI(napi_set_named_property(env, js_deferred, "resolve",
    reinterpret_cast<napi_value>(resolve)));
  CHECK_NAPI(napi_set_named_property(env, js_deferred, "reject",
    reinterpret_cast<napi_value>(reject)));

  CHECK_NAPI(napi_create_reference(env, js_deferred, 1, &ref));

  *deferred = reinterpret_cast<napi_deferred>(ref);
  *promise = reinterpret_cast<napi_value>(js_promise);

  return napi_ok;
}

NAPI_EXTERN napi_status napi_resolve_deferred(napi_env env,
                                              napi_deferred deferred,
                                              napi_value resolution) {
  return jsrtimpl::ConcludeDeferred(env, deferred, "resolve", resolution);
}

NAPI_EXTERN napi_status napi_reject_deferred(napi_env env,
                                             napi_deferred deferred,
                                             napi_value rejection) {
  return jsrtimpl::ConcludeDeferred(env, deferred, "reject", rejection);
}

NAPI_EXTERN napi_status napi_is_promise(napi_env env,
                                        napi_value promise,
                                        bool* is_promise) {
  CHECK_ARG(promise);
  CHECK_ARG(is_promise);

  napi_value global, promise_ctor;

  CHECK_NAPI(napi_get_global(env, &global));
  CHECK_NAPI(napi_get_named_property(env, global, "Promise", &promise_ctor));
  CHECK_NAPI(napi_instanceof(env, promise, promise_ctor, is_promise));

  return napi_ok;
}
