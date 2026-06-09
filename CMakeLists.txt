cmake_minimum_required(VERSION 3.20)
project(ort_webgpu_bench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ORT_ROOT "" CACHE PATH "ONNX Runtime install or build output directory")
set(ORT_SOURCE_ROOT "" CACHE PATH "Optional ONNX Runtime source directory for headers")
set(ORT_WEBGPU_PLUGIN "" CACHE FILEPATH "Optional ONNX Runtime WebGPU plugin library")
set(COPY_ORT_RUNTIME ON CACHE BOOL "Copy ONNX Runtime shared libraries next to the benchmark executable")

find_path(ORT_INCLUDE_DIR
  NAMES onnxruntime_cxx_api.h
  HINTS
    "${ORT_ROOT}/include"
    "${ORT_ROOT}/include/onnxruntime"
    "${ORT_ROOT}/include/onnxruntime/core/session"
    "${ORT_ROOT}/../../include/onnxruntime/core/session"
    "${ORT_SOURCE_ROOT}/include/onnxruntime/core/session"
)

find_library(ORT_LIBRARY
  NAMES onnxruntime libonnxruntime
  HINTS
    "${ORT_ROOT}/lib"
    "${ORT_ROOT}/capi"
    "${ORT_ROOT}"
)

if(WIN32)
  find_file(ORT_RUNTIME_LIBRARY
    NAMES onnxruntime.dll
    HINTS
      "${ORT_ROOT}/bin"
      "${ORT_ROOT}/lib"
      "${ORT_ROOT}/capi"
      "${ORT_ROOT}"
  )

  find_file(ORT_WEBGPU_PLUGIN_FOUND
    NAMES onnxruntime_providers_webgpu.dll
    HINTS
      "${ORT_ROOT}/bin"
      "${ORT_ROOT}/lib"
      "${ORT_ROOT}/capi"
      "${ORT_ROOT}"
  )
else()
  set(ORT_RUNTIME_LIBRARY "${ORT_LIBRARY}" CACHE FILEPATH "ONNX Runtime runtime library" FORCE)

  find_library(ORT_WEBGPU_PLUGIN_FOUND
    NAMES onnxruntime_providers_webgpu libonnxruntime_providers_webgpu
    HINTS
      "${ORT_ROOT}/lib"
      "${ORT_ROOT}/capi"
      "${ORT_ROOT}"
  )
endif()

if(NOT ORT_INCLUDE_DIR OR NOT ORT_LIBRARY)
  message(FATAL_ERROR
    "Set -DORT_ROOT=/path/to/onnxruntime install/build output and optionally "
    "-DORT_SOURCE_ROOT=/path/to/onnxruntime/source. Missing include/lib.")
endif()

if(WIN32 AND NOT ORT_RUNTIME_LIBRARY)
  message(FATAL_ERROR
    "Found ONNX Runtime import library, but not onnxruntime.dll. "
    "Set -DORT_ROOT to the ORT build output or install root containing the runtime DLL.")
endif()

if(NOT ORT_WEBGPU_PLUGIN AND ORT_WEBGPU_PLUGIN_FOUND)
  set(ORT_WEBGPU_PLUGIN "${ORT_WEBGPU_PLUGIN_FOUND}" CACHE FILEPATH "ONNX Runtime WebGPU plugin library" FORCE)
endif()

add_executable(ort_webgpu_bench
  src/audio_io.cpp
  src/audio_io.h
  src/features.cpp
  src/features.h
  src/main.cpp
)
target_include_directories(ort_webgpu_bench PRIVATE "${ORT_INCLUDE_DIR}")
target_link_libraries(ort_webgpu_bench PRIVATE "${ORT_LIBRARY}")

if(MSVC)
  target_compile_options(ort_webgpu_bench PRIVATE /W4 /permissive-)
else()
  target_compile_options(ort_webgpu_bench PRIVATE -Wall -Wextra -Wpedantic)
endif()

get_filename_component(ORT_LIB_DIR "${ORT_LIBRARY}" DIRECTORY)

if(APPLE)
  set_target_properties(ort_webgpu_bench PROPERTIES
    BUILD_RPATH "${ORT_LIB_DIR}"
    INSTALL_RPATH "@loader_path;${ORT_LIB_DIR}"
  )
elseif(UNIX)
  set_target_properties(ort_webgpu_bench PROPERTIES
    BUILD_RPATH "${ORT_LIB_DIR}"
    INSTALL_RPATH "$ORIGIN;${ORT_LIB_DIR}"
  )
endif()

if(COPY_ORT_RUNTIME)
  add_custom_command(TARGET ort_webgpu_bench POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${ORT_RUNTIME_LIBRARY}"
            "$<TARGET_FILE_DIR:ort_webgpu_bench>"
    VERBATIM)

  if(ORT_WEBGPU_PLUGIN)
    add_custom_command(TARGET ort_webgpu_bench POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different
              "${ORT_WEBGPU_PLUGIN}"
              "$<TARGET_FILE_DIR:ort_webgpu_bench>"
      VERBATIM)
  endif()

  if(WIN32)
    get_filename_component(ORT_RUNTIME_DIR "${ORT_RUNTIME_LIBRARY}" DIRECTORY)
    file(GLOB ORT_RUNTIME_DLLS
      "${ORT_LIB_DIR}/onnxruntime.dll"
      "${ORT_LIB_DIR}/onnxruntime_providers_webgpu.dll"
      "${ORT_LIB_DIR}/webgpu_dawn.dll"
      "${ORT_LIB_DIR}/dxcompiler.dll"
      "${ORT_LIB_DIR}/dxil.dll"
      "${ORT_RUNTIME_DIR}/onnxruntime.dll"
      "${ORT_RUNTIME_DIR}/onnxruntime_providers_webgpu.dll"
      "${ORT_RUNTIME_DIR}/webgpu_dawn.dll"
      "${ORT_RUNTIME_DIR}/dxcompiler.dll"
      "${ORT_RUNTIME_DIR}/dxil.dll"
    )
    foreach(runtime_dll IN LISTS ORT_RUNTIME_DLLS)
      add_custom_command(TARGET ort_webgpu_bench POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${runtime_dll}"
                "$<TARGET_FILE_DIR:ort_webgpu_bench>"
        VERBATIM)
    endforeach()
  endif()
endif()

message(STATUS "ORT include: ${ORT_INCLUDE_DIR}")
message(STATUS "ORT library: ${ORT_LIBRARY}")
message(STATUS "ORT runtime library: ${ORT_RUNTIME_LIBRARY}")
message(STATUS "ORT WebGPU plugin: ${ORT_WEBGPU_PLUGIN}")
