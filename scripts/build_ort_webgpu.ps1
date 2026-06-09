param(
  [string]$OrtSrc = "",
  [string]$OrtBuildDir = "",
  [string]$Config = "Release",
  [ValidateSet("d3d12", "vulkan")]
  [string]$DawnBackend = "d3d12",
  [string]$CMakeGenerator = "Ninja",
  [string]$MsvcToolset = "",
  [string]$BenchmarkBuildDir = "",
  [int]$OrtBuildRetries = 3,
  [switch]$SkipBenchmark,
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

if ([string]::IsNullOrWhiteSpace($BenchmarkBuildDir)) {
  $BenchmarkBuildDir = Join-Path $ProjectDir "build\windows-release"
}

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OrtSrc), $OrtBuildDir | Out-Null

function Invoke-Checked {
  param(
    [Parameter(Mandatory = $true)]
    [string]$FilePath,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Arguments
  )

  & $FilePath @Arguments
  if ($LASTEXITCODE -ne 0) {
    throw "'$FilePath' failed with exit code $LASTEXITCODE."
  }
}

function Import-VsDevEnvironment {
  if ((Get-Command cl.exe -ErrorAction SilentlyContinue) -and $env:VCToolsInstallDir) {
    return
  }

  $candidates = @()
  if ($env:VSINSTALLDIR) {
    $candidates += Join-Path $env:VSINSTALLDIR "Common7\Tools\VsDevCmd.bat"
  }

  $vsRoots = @(
    "${env:ProgramFiles}\Microsoft Visual Studio",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio"
  )

  foreach ($root in $vsRoots) {
    if (Test-Path $root) {
      $candidates += Get-ChildItem -Path $root -Recurse -Filter "VsDevCmd.bat" -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending |
        ForEach-Object { $_.FullName }
    }
  }

  $vsDevCmd = $candidates | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
  if (-not $vsDevCmd) {
    throw "MSVC compiler environment was not found. Install Visual Studio C++ build tools, or run this script from a Developer PowerShell."
  }

  cmd /c "`"$vsDevCmd`" -arch=x64 -host_arch=x64 >nul && set" | ForEach-Object {
    if ($_ -match "^(.*?)=(.*)$") {
      Set-Item -Path "Env:$($matches[1])" -Value $matches[2]
    }
  }

  if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw "VsDevCmd.bat was found at '$vsDevCmd', but cl.exe is still unavailable."
  }
}

Import-VsDevEnvironment

$cl = Get-Command cl.exe -ErrorAction Stop
$env:CC = $cl.Source
$env:CXX = $cl.Source

if ([string]::IsNullOrWhiteSpace($MsvcToolset) -and $env:VCToolsInstallDir) {
  $msvcVersion = Split-Path -Leaf ($env:VCToolsInstallDir.TrimEnd('\'))
  if ($msvcVersion -match "^(\d+\.\d+)") {
    $MsvcToolset = $matches[1]
  }
}

$vsNinja = Join-Path $env:VSINSTALLDIR "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if (Test-Path $vsNinja) {
  $env:CMAKE_MAKE_PROGRAM = $vsNinja
  $env:PATH = "$(Split-Path -Parent $vsNinja);$env:PATH"
}

function Clear-CMakeGeneratorCacheIfNeeded {
  param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDir,
    [Parameter(Mandatory = $true)]
    [string]$ExpectedGenerator
  )

  $cachePath = Join-Path $BuildDir "CMakeCache.txt"
  if (-not (Test-Path $cachePath)) {
    return
  }

  $generatorLine = Get-Content -Path $cachePath | Where-Object { $_ -like "CMAKE_GENERATOR:INTERNAL=*" } | Select-Object -First 1
  if (-not $generatorLine) {
    return
  }

  $actualGenerator = $generatorLine.Substring("CMAKE_GENERATOR:INTERNAL=".Length)
  if ($actualGenerator -eq $ExpectedGenerator) {
    return
  }

  Write-Host "CMake generator changed from '$actualGenerator' to '$ExpectedGenerator'; removing stale CMake configure cache."
  Remove-Item -LiteralPath $cachePath -Force

  $cmakeFiles = Join-Path $BuildDir "CMakeFiles"
  if (Test-Path $cmakeFiles) {
    Remove-Item -LiteralPath $cmakeFiles -Recurse -Force
  }
}

Clear-CMakeGeneratorCacheIfNeeded -BuildDir (Join-Path $OrtBuildDir $Config) -ExpectedGenerator $CMakeGenerator

function Clear-OrtTransientBuildState {
  $vcpkgDir = Join-Path $OrtBuildDir "vcpkg"
  if (Test-Path $vcpkgDir) {
    $vcpkgToolchain = Join-Path $vcpkgDir "scripts\buildsystems\vcpkg.cmake"
    if (-not (Test-Path $vcpkgToolchain)) {
      Write-Host "Removing incomplete vcpkg checkout."
      Remove-Item -LiteralPath $vcpkgDir -Recurse -Force
    }
  }

  $detectCompilerBuildTree = Join-Path $OrtBuildDir "vcpkg\buildtrees\detect_compiler"
  if (Test-Path $detectCompilerBuildTree) {
    Write-Host "Removing stale vcpkg detect_compiler cache."
    Remove-Item -LiteralPath $detectCompilerBuildTree -Recurse -Force
  }
}

Clear-OrtTransientBuildState

if (-not (Test-Path (Join-Path $OrtSrc ".git"))) {
  Invoke-Checked git clone --depth 1 https://github.com/microsoft/onnxruntime.git $OrtSrc
} else {
  Invoke-Checked git -C $OrtSrc fetch --depth 1 origin main
  Invoke-Checked git -C $OrtSrc checkout FETCH_HEAD
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

if ($env:CMAKE_MAKE_PROGRAM) {
  $cmakeDefines += "CMAKE_MAKE_PROGRAM=$env:CMAKE_MAKE_PROGRAM"
}

$cmakeDefines += "CMAKE_C_COMPILER=$env:CC"
$cmakeDefines += "CMAKE_CXX_COMPILER=$env:CXX"

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
  "--cmake_generator", $CMakeGenerator
)

if (-not [string]::IsNullOrWhiteSpace($MsvcToolset)) {
  $buildArgs += "--msvc_toolset"
  $buildArgs += $MsvcToolset
}

if (-not $NoVcpkg) {
  $buildArgs += "--use_vcpkg"
}

$buildArgs += "--cmake_extra_defines"
$buildArgs += $cmakeDefines

if ($OrtBuildRetries -lt 1) {
  throw "-OrtBuildRetries must be at least 1."
}

for ($attempt = 1; $attempt -le $OrtBuildRetries; ++$attempt) {
  try {
    Invoke-Checked python @buildArgs
    break
  } catch {
    if ($attempt -ge $OrtBuildRetries) {
      throw
    }

    Write-Warning "ORT build attempt $attempt failed: $($_.Exception.Message)"
    Clear-OrtTransientBuildState
    Start-Sleep -Seconds ([Math]::Min(30, 5 * $attempt))
    Write-Host "Retrying ORT build ($($attempt + 1)/$OrtBuildRetries)."
  }
}

$ortOut = Join-Path $OrtBuildDir $Config
$webgpuPlugin = Join-Path $ortOut "onnxruntime_providers_webgpu.dll"

if (-not $SkipBenchmark) {
  Clear-CMakeGeneratorCacheIfNeeded -BuildDir $BenchmarkBuildDir -ExpectedGenerator $CMakeGenerator

  $benchmarkConfigureArgs = @(
    "-S", $ProjectDir,
    "-B", $BenchmarkBuildDir,
    "-G", $CMakeGenerator,
    "-DORT_ROOT=$ortOut",
    "-DORT_SOURCE_ROOT=$OrtSrc",
    "-DORT_WEBGPU_PLUGIN=$webgpuPlugin"
  )

  Invoke-Checked cmake @benchmarkConfigureArgs
  Invoke-Checked cmake --build $BenchmarkBuildDir --config $Config
}

Write-Host ""
Write-Host "ORT build output: $ortOut"
Write-Host "Use these values in CLion/CMake:"
Write-Host "  ORT_ROOT=$ortOut"
Write-Host "  ORT_SOURCE_ROOT=$OrtSrc"
Write-Host "  ORT_WEBGPU_PLUGIN=$webgpuPlugin"
Write-Host ""
Write-Host "Benchmark build:"
Write-Host "  cmake -S `"$ProjectDir`" -B `"$BenchmarkBuildDir`" -G `"$CMakeGenerator`" -DORT_ROOT=`"$ortOut`" -DORT_SOURCE_ROOT=`"$OrtSrc`" -DORT_WEBGPU_PLUGIN=`"$webgpuPlugin`""
Write-Host "  cmake --build `"$BenchmarkBuildDir`" --config $Config"
Write-Host ""
Write-Host "Example run:"
if ($CMakeGenerator -like "Visual Studio*") {
  $benchExe = Join-Path $BenchmarkBuildDir "$Config\ort_webgpu_bench.exe"
} else {
  $benchExe = Join-Path $BenchmarkBuildDir "ort_webgpu_bench.exe"
}
Write-Host "  `"$benchExe`" --model `"$RepoRoot\pc_nsf_hifigan.onnx`" --provider webgpu --dawn-backend $DawnBackend --allow-cpu-fallback"
