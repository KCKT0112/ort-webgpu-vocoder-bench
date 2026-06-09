# ORT Native WebGPU Vocoder Benchmark

Small native C++ benchmark for running `pc_nsf_hifigan.onnx` with ONNX Runtime WebGPU EP.

There is no browser, WebAssembly, JavaScript, or Node.js runtime in this project. It uses ONNX Runtime's native C/C++ API and the WebGPU plugin EP library.

## What It Runs

The benchmark generates deterministic test tensors:

```text
mel:   float32 [1, 128, frames]
f0:    float32 [1, frames]
audio: float32 [1, 1, frames * 512]
```

It reports session creation time, output shape/range, latency mean/p50/p90/min/max, realtime factor, and throughput relative to 44.1 kHz audio.

It can also read a WAV file, extract mel/F0 features, run the ONNX vocoder, and write the generated audio as a mono 16-bit WAV.

## Project Layout

```text
ort_webgpu_bench/
  CMakeLists.txt
  CMakePresets.json
  src/main.cpp
  src/audio_io.*
  src/features.*
  scripts/build_ort_webgpu.sh
  scripts/build_ort_webgpu.ps1
```

## Requirements

Common:

- CMake 3.20+
- C++17 compiler
- Python 3
- Git
- A recent ONNX Runtime source tree built with `--use_webgpu shared_lib`

macOS:

- Xcode Command Line Tools
- macOS with Metal support

Linux:

- Vulkan-capable GPU and driver
- Vulkan loader/development package, for example `libvulkan-dev` on Ubuntu
- Usual native build tools: `build-essential`, `ninja-build` or Make

Windows:

- Visual Studio 2022 C++ toolchain
- Windows 10/11 with D3D12-capable GPU and current driver
- PowerShell

## Build ONNX Runtime WebGPU

The helper scripts clone or update ONNX Runtime `main` and build `libonnxruntime` plus the WebGPU plugin EP.

macOS/Linux:

```bash
cd ort_webgpu_bench
scripts/build_ort_webgpu.sh
```

Useful environment variables:

```bash
ORT_SRC=/tmp/onnxruntime-webgpu-src
ORT_BUILD_DIR=/tmp/onnxruntime-webgpu-build
BUILD_TYPE=Release
USE_VCPKG=1
CMAKE_OSX_ARCHITECTURES=arm64
```

Defaults:

- macOS: uses `/private/tmp`, enables `--use_vcpkg`, builds Dawn Metal.
- Linux: uses `/tmp`, does not enable `--use_vcpkg` by default, builds Dawn Vulkan.

Windows:

```powershell
cd ort_webgpu_bench
.\scripts\build_ort_webgpu.ps1
```

Windows defaults to Dawn D3D12. To build Vulkan instead:

```powershell
.\scripts\build_ort_webgpu.ps1 -DawnBackend vulkan
```

After build, set these variables or pass equivalent CMake options:

```text
ORT_ROOT=<onnxruntime build dir>/<Release>
ORT_SOURCE_ROOT=<onnxruntime source dir>
ORT_WEBGPU_PLUGIN=<path to onnxruntime_providers_webgpu library>
```

Example macOS values:

```bash
export ORT_ROOT=/private/tmp/onnxruntime-webgpu-build/Release
export ORT_SOURCE_ROOT=/private/tmp/onnxruntime-webgpu-src
export ORT_WEBGPU_PLUGIN=/private/tmp/onnxruntime-webgpu-build/Release/libonnxruntime_providers_webgpu.dylib
```

## Build The Benchmark

Command line:

```bash
cmake -S ort_webgpu_bench -B ort_webgpu_bench/build/release \
  -DORT_ROOT="$ORT_ROOT" \
  -DORT_SOURCE_ROOT="$ORT_SOURCE_ROOT" \
  -DORT_WEBGPU_PLUGIN="$ORT_WEBGPU_PLUGIN"
cmake --build ort_webgpu_bench/build/release --config Release
```

Windows with Visual Studio generator:

```powershell
cmake -S ort_webgpu_bench -B ort_webgpu_bench\build\windows-release `
  -G "Visual Studio 17 2022" -A x64 `
  -DORT_ROOT="$env:ORT_ROOT" `
  -DORT_SOURCE_ROOT="$env:ORT_SOURCE_ROOT" `
  -DORT_WEBGPU_PLUGIN="$env:ORT_WEBGPU_PLUGIN"
cmake --build ort_webgpu_bench\build\windows-release --config Release
```

CMake copies the ORT shared library and WebGPU plugin next to the executable when `COPY_ORT_RUNTIME=ON` (default). On Windows it also attempts to copy `webgpu_dawn.dll`, `dxcompiler.dll`, and `dxil.dll` if they exist beside `onnxruntime.dll`.

## CLion

Open the `ort_webgpu_bench` directory directly in CLion.

Recommended setup:

1. Build ONNX Runtime with one of the scripts above.
2. In CLion, set environment variables for the CMake profile:

```text
ORT_ROOT=/path/to/onnxruntime-webgpu-build/Release
ORT_SOURCE_ROOT=/path/to/onnxruntime-webgpu-src
ORT_WEBGPU_PLUGIN=/path/to/libonnxruntime_providers_webgpu.dylib
```

Use `.so` on Linux and `.dll` on Windows.

3. Select the matching preset:

- `macos-release`
- `linux-release`
- `windows-msvc-release`

4. Build target `ort_webgpu_bench`.

If CLion shows an empty `ORT_ROOT`, pass `-DORT_ROOT=...` and `-DORT_SOURCE_ROOT=...` in the CMake profile cache variables instead of environment variables.

## Run

WebGPU, strict mode with CPU fallback disabled:

```bash
./ort_webgpu_bench --model /path/to/pc_nsf_hifigan.onnx --provider webgpu
```

Mixed WebGPU path with CPU fallback allowed:

```bash
./ort_webgpu_bench \
  --model /path/to/pc_nsf_hifigan.onnx \
  --provider webgpu \
  --frames 256 \
  --warmup 5 \
  --runs 30 \
  --allow-cpu-fallback
```

WAV to WAV inference:

```bash
./ort_webgpu_bench \
  --model /path/to/pc_nsf_hifigan.onnx \
  --provider webgpu \
  --input-wav /path/to/input.wav \
  --output-wav /path/to/output.wav \
  --allow-cpu-fallback
```

Quick smoke test using only the first 32 frames:

```bash
./ort_webgpu_bench \
  --model /path/to/pc_nsf_hifigan.onnx \
  --provider webgpu \
  --input-wav /path/to/input.wav \
  --output-wav /path/to/output.wav \
  --max-frames 32 \
  --allow-cpu-fallback
```

Explicit plugin path:

```bash
./ort_webgpu_bench \
  --model /path/to/pc_nsf_hifigan.onnx \
  --provider webgpu \
  --plugin /path/to/libonnxruntime_providers_webgpu.so \
  --allow-cpu-fallback
```

CPU baseline:

```bash
./ort_webgpu_bench \
  --model /path/to/pc_nsf_hifigan.onnx \
  --provider cpu \
  --frames 256 \
  --warmup 5 \
  --runs 30 \
  --threads 1
```

Windows D3D12:

```powershell
.\ort_webgpu_bench.exe --model C:\path\pc_nsf_hifigan.onnx --provider webgpu --dawn-backend d3d12 --allow-cpu-fallback
```

Linux Vulkan:

```bash
./ort_webgpu_bench --model /path/to/pc_nsf_hifigan.onnx --provider webgpu --dawn-backend vulkan --allow-cpu-fallback
```

## CLI Options

```text
--model PATH
--input-wav PATH
--output-wav PATH
--provider webgpu|cpu
--plugin PATH
--dawn-backend auto|d3d12|vulkan
--power-preference high-performance|low-power
--frames N
--max-frames N
--sample-rate N
--warmup N
--runs N
--threads N
--allow-cpu-fallback
```

`--plugin` defaults to the platform library name in the executable directory:

- macOS: `libonnxruntime_providers_webgpu.dylib`
- Linux: `libonnxruntime_providers_webgpu.so`
- Windows: `onnxruntime_providers_webgpu.dll`

## WAV Feature Extraction Notes

The WAV path is intentionally dependency-free:

- Input WAV is converted to mono.
- PCM 8/16/24/32-bit and 32-bit IEEE float WAV are supported.
- Non-44.1 kHz input is linearly resampled to 44.1 kHz.
- Mel uses 128 bins, `n_fft=2048`, `win=2048`, `hop=512`, `fmin=40`, `fmax=16000`, reflect padding, and natural-log compression.
- F0 is estimated with a lightweight autocorrelation method.

The built-in F0 estimator is a practical baseline for standalone C++ inference. It is not a replacement for higher-quality pitch extractors such as RMVPE, parselmouth, or WORLD harvest.

## Current pc_nsf_hifigan Result

On the tested macOS machine, `pc_nsf_hifigan.onnx` is not fully covered by WebGPU EP. With CPU fallback disabled, ORT reports that some graph nodes are assigned to the default CPU EP. Use `--allow-cpu-fallback` to run the current mixed WebGPU path.

Measured with `frames=256`, `warmup=5`, `runs=30`:

```text
ONNX Runtime CPU: mean=1708.989 ms, RTF=0.575, 1.739x realtime
ONNX Runtime WebGPU + CPU fallback: mean=190.153 ms, RTF=0.064, 15.630x realtime
```

WAV smoke test with `28.wav`, `--max-frames 32`, and WebGPU + CPU fallback:

```text
output_shape=1 1 16384
latency_ms mean=141.244
wrote_wav=/private/tmp/ort_webgpu_out_webgpu.wav sample_rate=44100 samples=16384
```

For reference, the matching PyTorch checkpoint benchmark in the parent repository measured:

```text
PyTorch ckpt CPU, remove weight norm: mean=619.262 ms, RTF=0.208, 4.800x realtime
PyTorch ckpt MPS, remove weight norm: mean=125.904 ms, RTF=0.042, 23.607x realtime
```

## Troubleshooting

`No supported adapters`:

- On macOS, run outside sandboxed shells if they block Metal access.
- On Linux, verify Vulkan works with tools such as `vulkaninfo`.
- On Windows, verify the GPU supports D3D12 and try `--dawn-backend d3d12`.

`This session contains graph nodes that are assigned to the default CPU EP`:

- The model is not fully supported by WebGPU EP. Add `--allow-cpu-fallback` to benchmark mixed execution.

Plugin registration fails:

- Ensure the WebGPU plugin library and its runtime dependencies are next to the executable or pass `--plugin`.
- On Windows, keep `onnxruntime.dll`, `onnxruntime_providers_webgpu.dll`, `webgpu_dawn.dll`, `dxcompiler.dll`, and `dxil.dll` together when present.

CLion cannot find ORT headers:

- Set `ORT_ROOT` to the ORT build output directory and `ORT_SOURCE_ROOT` to the ORT source checkout. Current ORT build outputs often do not contain a full install-style include directory.
