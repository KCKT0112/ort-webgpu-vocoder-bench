#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_ROOT="$(cd "${PROJECT_DIR}/.." && pwd)"

case "$(uname -s)" in
  Darwin)
    DEFAULT_ARCH="$(uname -m)"
    USE_VCPKG_DEFAULT=1
    ;;
  Linux)
    DEFAULT_ARCH=""
    USE_VCPKG_DEFAULT=0
    ;;
  *)
    echo "Unsupported OS for this shell script. Use build_ort_webgpu.ps1 on Windows." >&2
    exit 1
    ;;
esac

ORT_SRC="${ORT_SRC:-${PROJECT_DIR}/.deps/onnxruntime-webgpu-src}"
ORT_BUILD_DIR="${ORT_BUILD_DIR:-${PROJECT_DIR}/artifacts/onnxruntime-webgpu-build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
USE_VCPKG="${USE_VCPKG:-${USE_VCPKG_DEFAULT}}"
CMAKE_OSX_ARCHITECTURES="${CMAKE_OSX_ARCHITECTURES:-${DEFAULT_ARCH}}"

mkdir -p "$(dirname "${ORT_SRC}")" "${ORT_BUILD_DIR}"

if [[ ! -d "${ORT_SRC}/.git" ]]; then
  git clone --depth 1 https://github.com/microsoft/onnxruntime.git "${ORT_SRC}"
else
  git -C "${ORT_SRC}" fetch --depth 1 origin main
  git -C "${ORT_SRC}" checkout FETCH_HEAD
fi

cmake_defines=(
  onnxruntime_BUILD_UNIT_TESTS=OFF
  FETCHCONTENT_TRY_FIND_PACKAGE_MODE=NEVER
)

if [[ "$(uname -s)" == "Darwin" && -n "${CMAKE_OSX_ARCHITECTURES}" ]]; then
  cmake_defines+=(CMAKE_OSX_ARCHITECTURES="${CMAKE_OSX_ARCHITECTURES}")
fi

if [[ "$(uname -s)" == "Linux" ]]; then
  cmake_defines+=(
    onnxruntime_ENABLE_DAWN_BACKEND_VULKAN=ON
  )
fi

build_args=(
  "${ORT_SRC}/tools/ci_build/build.py"
  --build_dir "${ORT_BUILD_DIR}"
  --config "${BUILD_TYPE}"
  --build_shared_lib
  --use_webgpu shared_lib
  --wgsl_template static
  --disable_rtti
  --parallel
  --update
  --build
  --skip_tests
)

if [[ "${USE_VCPKG}" == "1" ]]; then
  build_args+=(--use_vcpkg)
fi

build_args+=(--cmake_extra_defines "${cmake_defines[@]}")

python3 "${build_args[@]}"

ORT_OUT="${ORT_BUILD_DIR}/${BUILD_TYPE}"
echo
echo "ORT build output: ${ORT_OUT}"
echo "Use these values in CLion/CMake:"
echo "  ORT_ROOT=${ORT_OUT}"
echo "  ORT_SOURCE_ROOT=${ORT_SRC}"
echo "  ORT_WEBGPU_PLUGIN=${ORT_OUT}/$( [[ "$(uname -s)" == "Darwin" ]] && echo libonnxruntime_providers_webgpu.dylib || echo libonnxruntime_providers_webgpu.so )"
echo
echo "Benchmark build:"
echo "  cmake -S \"${PROJECT_DIR}\" -B \"${PROJECT_DIR}/build/release\" -DORT_ROOT=\"${ORT_OUT}\" -DORT_SOURCE_ROOT=\"${ORT_SRC}\""
echo "  cmake --build \"${PROJECT_DIR}/build/release\" --config Release"
echo
echo "Example run:"
echo "  \"${PROJECT_DIR}/build/release/ort_webgpu_bench\" --model \"${REPO_ROOT}/pc_nsf_hifigan.onnx\" --provider webgpu --allow-cpu-fallback"
