cmake_minimum_required(VERSION 3.20)

project(TinyRenderer
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(tinyrenderer_options INTERFACE)
target_compile_features(tinyrenderer_options INTERFACE cxx_std_20)

if(MSVC)
    target_compile_options(tinyrenderer_options INTERFACE /utf-8 /EHsc /W3)
else()
    target_compile_options(tinyrenderer_options INTERFACE -Wall -Wextra -Wpedantic)
endif()

set(TINYRENDERER_LIB_SOURCES
    Project1/image_io.cpp
    Project1/model.cpp
    Project1/our_gl.cpp
    Project1/scene.cpp
    Project1/shaders.cpp
    Project1/texture.cpp
    Project1/tgaimage.cpp
)

set(TINYRENDERER_HEADERS
    Project1/geometry.h
    Project1/image_io.h
    Project1/model.h
    Project1/our_gl.h
    Project1/scene.h
    Project1/shaders.h
    Project1/texture.h
    Project1/tgaimage.h
)

add_library(tinyrenderer_lib
    ${TINYRENDERER_LIB_SOURCES}
    ${TINYRENDERER_HEADERS}
)

target_link_libraries(tinyrenderer_lib PUBLIC tinyrenderer_options)
target_include_directories(tinyrenderer_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Project1
)

set_target_properties(tinyrenderer_lib PROPERTIES
    CXX_EXTENSIONS OFF
)

add_executable(renderer
    Project1/main.cpp
)

target_link_libraries(renderer PRIVATE tinyrenderer_lib)
set_target_properties(renderer PROPERTIES
    OUTPUT_NAME renderer
    CXX_EXTENSIONS OFF
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/Project1 PREFIX Project1 FILES
    ${TINYRENDERER_LIB_SOURCES}
    ${TINYRENDERER_HEADERS}
    Project1/main.cpp
)
