# Aseprite Scripting Library
# Copyright (C) 2015-2016  David Capello

include_directories(${DUKTAPE_DIR})

add_library(duktape ${DUKTAPE_DIR}/duktape.c)
add_library(script-lib
  duktape/engine.cpp
  v8/engine.cpp
  lua/engine.cpp
  cout_delegate.cpp)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++14")

if(UNIX)
  target_link_libraries(duktape m)
endif()

target_link_libraries(script-lib duktape)
