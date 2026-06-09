param(
  [string]$OrtSrc = "",
  [string]$OrtBuildDir = "",
  [string]$Config = "Release",
  [ValidateSet("d3d12", "vulkan")]
  [string]$DawnBackend = "d3d12",
  [switch]$NoVcpkg
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectDir = Resolve-Path (Join-Path $ScriptDir "..")
$RepoRoot = Resolve-Path (Join-Path $ProjectDir "..")

if ([string]::IsNullOrWhiteSpace($OrtSrc)) {
  $OrtSrc = Join-Path $ProjectDir ".deps\onnxruntime-webgpu-src"
}

if ([string]::IsNullOrWhiteSpace($OrtBuildDir)) {
  $OrtBuildDir = Join-Path $ProjectDir "artifacts\onnxruntime-webgpu-build"
}

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OrtSrc), $OrtBuildDir | Out-Null

if (-not (Test-Path (Join-Path $OrtSrc ".git"))) {
  git clone --depth 1 https://github.com/microsoft/onnxruntime.git $OrtSrc
} else {
  git -C $OrtSrc fetch --depth 1 origin main
  git -C $OrtSrc checkout FETCH_HEAD
}

$cmakeDefines = @(
  "onnxruntime_BUILD_UNIT_TESTS=OFF",
  "FETCHCONTENT_TRY_FIND_PACKAGE_MODE=NEVER"
)

if ($DawnBackend -eq "d3d12") {
  $cmakeDefines += "onnxruntime_ENABLE_DAWN_BACKEND_D3D12=ON"
  $cmakeDefines += "onnxruntime_ENABLE_DAWN_BACKEND_VULKAN=OFF"
} else {
  $cmakeDefines += "onnxruntime_ENABLE_DAWN_BACKEND_D3D12=OFF"
  $cmakeDefines += "onnxruntime_ENABLE_DAWN_BACKEND_VULKAN=ON"
}

$buildArgs = @(
  "$OrtSrc\tools\ci_build\build.py",
  "--build_dir", $OrtBuildDir,
  "--config", $Config,
  "--build_shared_lib",
  "--use_webgpu", "shared_lib",
  "--wgsl_template", "static",
  "--disable_rtti",
  "--parallel",
  "--update",
  "--build",
  "--skip_tests",
  "--cmake_generator", "Visual Studio 17 2022"
)

if (-not $NoVcpkg) {
  $buildArgs += "--use_vcpkg"
}

$buildArgs += "--cmake_extra_defines"
$buildArgs += $cmakeDefines

python $buildArgs

$ortOut = Join-Path $OrtBuildDir $Config
Write-Host ""
Write-Host "ORT build output: $ortOut"
Write-Host "Use these values in CLion/CMake:"
Write-Host "  ORT_ROOT=$ortOut"
Write-Host "  ORT_SOURCE_ROOT=$OrtSrc"
Write-Host "  ORT_WEBGPU_PLUGIN=$(Join-Path $ortOut 'onnxruntime_providers_webgpu.dll')"
Write-Host ""
Write-Host "Benchmark build:"
Write-Host "  cmake -S `"$ProjectDir`" -B `"$ProjectDir\build\windows-release`" -G `"Visual Studio 17 2022`" -A x64 -DORT_ROOT=`"$ortOut`" -DORT_SOURCE_ROOT=`"$OrtSrc`""
Write-Host "  cmake --build `"$ProjectDir\build\windows-release`" --config Release"
Write-Host ""
Write-Host "Example run:"
Write-Host "  `"$ProjectDir\build\windows-release\Release\ort_webgpu_bench.exe`" --model `"$RepoRoot\pc_nsf_hifigan.onnx`" --provider webgpu --dawn-backend $DawnBackend --allow-cpu-fallback"
