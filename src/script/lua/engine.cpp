// LibreSprite Scripting Library
// Copyright (C) 2021  LibreSprite contributors
//
// This file is released under the terms of the MIT license.
// Read LICENSE.txt for more information.


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#if SCRIPT_ENGINE_LUA

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

extern "C" {

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

}

using namespace script;

class LuaEngine : public Engine {
public:
  inject<EngineDelegate> m_delegate;

  bool m_printLastResult = false;
  lua_State* L = nullptr;

  LuaEngine() {
    InternalScriptObject::setDefault("LuaScriptObject");
    L = luaL_newstate();
    luaL_openlibs(L);
    initGlobals();
  }

  ~LuaEngine() {
    if (L)
      lua_close(L);
  }

  void printLastResult() override {
    m_printLastResult = true;
  }

  bool raiseEvent(const std::string& event) override {
    return eval("if onEvent~=nil then onEvent(\"" + event + "\") end");
  }

  bool eval(const std::string& code) override {
    bool success = true;
    try {

      if (luaL_loadstring(L, code.c_str()) == LUA_OK) {
        if (lua_pcall(L, 0, 0, 0) == LUA_OK) {
          lua_pop(L, lua_gettop(L));
        } else success = false;
      } success = false;

    } catch (const std::exception& ex) {
      std::string err = "Error: ";
      err += ex.what();
      m_delegate->onConsolePrint(err.c_str());
      success = false;
    }
    execAfterEval(success);
    return success;
  }
};

static Engine::Regular<LuaEngine> registration("lua", {"lua"});

class LuaScriptObject : public InternalScriptObject {
public:

  static Value getValue(lua_State* L, int index) {
    auto type = lua_type(L, index);
    if (type == LUA_TNIL) return {};
    if (type == LUA_TNUMBER) return lua_tonumber(L, index);
    if (type == LUA_TBOOLEAN) return lua_toboolean(L, index);
    if (type == LUA_TSTRING) return {lua_tostring(L, index)};
    // LUA_TTABLE, LUA_TFUNCTION, LUA_TUSERDATA, LUA_TTHREAD, and LUA_TLIGHTUSERDATA
    return {};
  }

  static int returnValue(lua_State* L, const Value& value) {
    switch (value.type) {
    case Value::Type::UNDEFINED:
      return 0;

    case Value::Type::INT:
      lua_pushinteger(L, (int) value);
      return 1;

    case Value::Type::DOUBLE:
      lua_pushnumber(L, value);
      return 1;

    case Value::Type::STRING:
      lua_pushstring(L, value);
      return 1;

    case Value::Type::OBJECT:
      if (auto object = static_cast<ScriptObject*>(value)) {
        static_cast<LuaScriptObject*>(object->getInternalScriptObject())->makeLocal(L);
        return 1;
      }
      return 0;
    }
    return 0;
  }

  static int callFunc(lua_State* L) {
    int n = lua_gettop(L);    /* number of arguments */
    auto funcptr = reinterpret_cast<script::Function*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!funcptr) return 0;
    auto& func = *funcptr;

    for (int i = 1; i <= n; i++) {
      func.arguments.push_back(getValue(L, i));
    }
    func();

    return returnValue(L, func.result);
  }

  void pushFunctions(lua_State* L) {
    for (auto& entry : functions) {
      script::Function* ptr = &entry.second;
      lua_pushlightuserdata(L, ptr);
      lua_pushcclosure(L, callFunc, 1);
      lua_setfield(L, -2, entry.first.c_str());
    }
  }

  static int getset(lua_State* L) {
    int n = lua_gettop(L);    /* number of arguments */
    auto prop = reinterpret_cast<script::ObjectProperty*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (!prop) return 0;

    auto& func = n ? prop->setter : prop->getter;
    for (int i = 1; i <= n; i++) {
      func.arguments.push_back(getValue(L, i));
    }
    func();
    return returnValue(L, func.result);
  }

  void pushProperties(lua_State* L) {
    for (auto& entry : properties) {
      lua_pushlightuserdata(L, &entry.second);
      lua_pushcclosure(L, getset, 1);
      lua_setfield(L, -2, entry.first.c_str());
    }
  }

  int makeLocal(lua_State* L) {
    lua_newtable(L);
    int tableIndex = lua_gettop(L);
    pushFunctions(L);
    pushProperties(L);
    return tableIndex;
  }

  void makeGlobal(const std::string& name) override {
    auto L = m_engine.get<LuaEngine>()->L;
    lua_pushvalue(L, makeLocal(L));
    lua_setglobal(L, name.c_str());
  }
};

static InternalScriptObject::Regular<LuaScriptObject> luaSO("LuaScriptObject");

#endif
