// LibreSprite Scripting Library
// Copyright (C) 2021  LibreSprite contributors
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#if SCRIPT_ENGINE_V8

#include "base/convert_to.h"
#include "base/exception.h"
#include "base/fs.h"
#include "base/memory.h"
#include "script/engine.h"
#include "script/engine_delegate.h"

#include <map>
#include <iostream>
#include <string>
#include <unordered_map>

#include <v8.h>
#include <libplatform/libplatform.h>

using namespace script;

class V8Engine : public Engine {
public:
  inject<EngineDelegate> m_delegate;
  v8::Local<v8::Context> *m_context;
  bool m_printLastResult = false;
  v8::Isolate* m_isolate = nullptr;

  V8Engine() {
    InternalScriptObject::setDefault("V8ScriptObject");
    initV8();
  }

  v8::Local<v8::Context>& context() { return *m_context; }

  void initV8() {
    static std::unique_ptr<v8::Platform> m_platform;
    if (!m_platform) {
      // Conflicting documentation. Not sure if this is actually needed.
      // v8::V8::InitializeICUDefaultLocation(base::get_app_path().c_str());
      // v8::V8::InitializeExternalStartupData(base::get_app_path().c_str());
      v8::V8::InitializeICU();

      m_platform = v8::platform::NewDefaultPlatform();
      v8::V8::InitializePlatform(m_platform.get());
      v8::V8::Initialize();
    }

    v8::Isolate::CreateParams params;
    params.array_buffer_allocator = v8::ArrayBuffer::Allocator::NewDefaultAllocator();
    m_isolate = v8::Isolate::New(params);
  }

  void printLastResult() override {
    m_printLastResult = true;
  }

  bool eval(const std::string& code) override {
    bool errFlag = true;
    try {
      v8::Isolate::Scope isolatescope(m_isolate);
      // Create a stack-allocated handle scope.
      v8::HandleScope handle_scope(m_isolate);
      // Create a new context.
      v8::Local<v8::Context> context = v8::Context::New(m_isolate);
      m_context = &context;
      // Enter the context for compiling and running the hello world script.
      v8::Context::Scope context_scope(context);

      v8::TryCatch trycatch(m_isolate);

      initGlobals();

      // Create a string containing the JavaScript source code.
      v8::Local<v8::String> source = v8::String::NewFromUtf8(m_isolate, code.c_str()).ToLocalChecked();

      // Compile the source code.
      v8::Local<v8::Script> script =
        v8::Script::Compile(context, source).ToLocalChecked();

      // Run the script to get the result.
      v8::MaybeLocal<v8::Value> result = script->Run(context);
      if (result.IsEmpty()) {
        if (trycatch.HasCaught()) {
          v8::Local<v8::Value> exception = trycatch.Exception();
          v8::String::Utf8Value utf8(m_isolate, exception);
          m_delegate->onConsolePrint(*utf8);
        }
      } else if (m_printLastResult) {
        v8::String::Utf8Value utf8(m_isolate, result.ToLocalChecked());
        m_delegate->onConsolePrint(*utf8);
      }

      errFlag = false;
    } catch (const std::exception& ex) {
      std::string err = "Error: ";
      err += ex.what();
      m_delegate->onConsolePrint(err.c_str());
      errFlag = true;
    }
    return errFlag;
  }
};

static Engine::Regular<V8Engine> registration("js", {"js"});

class V8ScriptObject : public InternalScriptObject {
public:

  static Value getValue(v8::Isolate *isolate, v8::Local<v8::Value> local) {
    if (local->IsNullOrUndefined())
      return {};

    if (local->IsString()) {
      v8::String::Utf8Value utf8(isolate, local);
      return {*utf8};
    }

    if (local->IsNumber())
      return local.As<v8::Number>()->Value();

    if (local->IsUint32())
      return local.As<v8::Uint32>()->Value();

    if (local->IsInt32())
      return local.As<v8::Int32>()->Value();

    if (local->IsBoolean())
      return local.As<v8::Boolean>()->Value();

    v8::String::Utf8Value utf8(isolate, local->TypeOf(isolate));
    printf("Unknown type: [%s]\n", *utf8);

    return {};
  }

  static v8::Local<v8::Value> returnValue(v8::Isolate* isolate, const Value& value) {
    switch (value.type) {
    case Value::Type::UNDEFINED:
      return v8::Local<v8::Value>();

    case Value::Type::INT:
      return v8::Int32::New(isolate, value);

    case Value::Type::DOUBLE:
      return v8::Number::New(isolate, value);

    case Value::Type::STRING:
      return v8::String::NewFromUtf8(isolate, value).ToLocalChecked();

    case Value::Type::OBJECT:
      if (auto object = static_cast<ScriptObject*>(value)) {
        return static_cast<V8ScriptObject*>(object->getInternalScriptObject())->makeLocal();
      }
      return {};
    }
    return {};
  }

  static void callFunc(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto isolate = args.GetIsolate();
    v8::HandleScope handle_scope(isolate);
    auto data = args.Data().As<v8::External>();
    auto& func = *reinterpret_cast<script::Function*>(data->Value());

    for (int i = 0; i < args.Length(); i++) {
      func.arguments.push_back(getValue(isolate, args[i]));
    }

    func();

    args.GetReturnValue().Set(returnValue(isolate, func.result));
  }

  void pushFunctions(v8::Local<v8::Object>& object) {
    auto isolate = m_engine.get<V8Engine>()->m_isolate;
    auto& context = m_engine.get<V8Engine>()->context();
    for (auto& entry : functions) {
      auto tpl = v8::FunctionTemplate::New(isolate, callFunc, v8::External::New(isolate, &entry.second));
      auto func = tpl->GetFunction(context).ToLocalChecked();
      object->Set(context,
                  v8::String::NewFromUtf8(isolate, entry.first.c_str()).ToLocalChecked(),
                  func).Check();
    }
  }

  static void getterFunc(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto isolate = args.GetIsolate();
    v8::HandleScope handle_scope(isolate);
    auto data = args.Data().As<v8::External>();
    auto& func = *reinterpret_cast<script::Function*>(data->Value());
    func();
    args.GetReturnValue().Set(returnValue(isolate, func.result));
  }

  static void setterFunc(const v8::FunctionCallbackInfo<v8::Value>& args) {
    if (args.Length() != 1)
      return;
    auto isolate = args.GetIsolate();
    v8::HandleScope handle_scope(isolate);
    auto data = args.Data().As<v8::External>();
    auto& func = *reinterpret_cast<script::Function*>(data->Value());
    func.arguments.push_back(getValue(isolate, args[0]));
    func();
    args.GetReturnValue().Set(returnValue(isolate, func.result));
  }

  void pushProperties(v8::Local<v8::Object>& object) {
    auto& isolate = m_engine.get<V8Engine>()->m_isolate;
    auto& context = m_engine.get<V8Engine>()->context();

    for (auto& entry : properties) {
      auto getterTpl = v8::FunctionTemplate::New(isolate, getterFunc, v8::External::New(isolate, &entry.second));
      auto getter = getterTpl->GetFunction(context).ToLocalChecked();

      auto setterTpl = v8::FunctionTemplate::New(isolate, setterFunc, v8::External::New(isolate, &entry.second));
      auto setter = setterTpl->GetFunction(context).ToLocalChecked();

      v8::PropertyDescriptor descriptor(getter, setter);
      object->DefineProperty(context,
                             v8::String::NewFromUtf8(isolate, entry.first.c_str()).ToLocalChecked(),
                             descriptor).Check();
    }
  }

  v8::Local<v8::Object> makeLocal() {
    auto isolate = m_engine.get<V8Engine>()->m_isolate;
    auto local = v8::Object::New(isolate);
    pushFunctions(local);
    pushProperties(local);
    return local;
  }

  void makeGlobal(const std::string& name) override {
    auto& isolate = m_engine.get<V8Engine>()->m_isolate;
    auto& context = m_engine.get<V8Engine>()->context();
    context->Global()->Set(context,
                           v8::String::NewFromUtf8(isolate, name.c_str()).ToLocalChecked(),
                           makeLocal()).Check();
  }
};

static InternalScriptObject::Regular<V8ScriptObject> v8SO("V8ScriptObject");

#endif
