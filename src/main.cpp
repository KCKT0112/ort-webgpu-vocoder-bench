#include "audio_io.h"
#include "features.h"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

struct Args {
  fs::path model_path = "pc_nsf_hifigan.onnx";
  fs::path input_wav;
  fs::path output_wav;
  std::string provider = "webgpu";
  fs::path plugin_path;
  std::string dawn_backend;
  std::string power_preference = "high-performance";
  int frames = 256;
  int start_frame = 0;
  int max_frames = 0;
  int sample_rate = 44100;
  int warmup = 5;
  int runs = 30;
  int threads = 1;
  bool disable_cpu_fallback = true;
  bool print_feature_stats = false;
};

void PrintUsage(const char* argv0) {
  std::cerr
      << "Usage: " << argv0 << " [options]\n"
      << "  --model PATH                 ONNX model path (default: pc_nsf_hifigan.onnx)\n"
      << "  --input-wav PATH             input WAV; when set, mel/f0 are extracted from audio\n"
      << "  --output-wav PATH            output WAV path for audio inference\n"
      << "  --provider NAME              webgpu or cpu (default: webgpu)\n"
      << "  --plugin PATH                WebGPU EP plugin library path\n"
      << "  --dawn-backend NAME          auto, d3d12, or vulkan (default: auto)\n"
      << "  --power-preference NAME      high-performance or low-power (default: high-performance)\n"
      << "  --frames N                   mel/f0 frame count (default: 256)\n"
      << "  --start-frame N              first extracted WAV feature frame to run (default: 0)\n"
      << "  --max-frames N               truncate extracted WAV features to N frames (default: no limit)\n"
      << "  --sample-rate N              output sample rate for WAV mode (default: 44100)\n"
      << "  --warmup N                   warmup runs (default: 5)\n"
      << "  --runs N                     measured runs (default: 30)\n"
      << "  --threads N                  ORT intra/inter op threads for CPU work (default: 1)\n"
      << "  --allow-cpu-fallback         allow ORT to place unsupported nodes on CPU\n"
      << "  --print-feature-stats        print extracted WAV mel/F0 statistics\n";
}

std::string Lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

Args ParseArgs(int argc, char** argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    auto need_value = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        throw std::runtime_error(std::string("missing value for ") + name);
      }
      return argv[++i];
    };

    if (key == "--model") {
      args.model_path = need_value("--model");
    } else if (key == "--input-wav") {
      args.input_wav = need_value("--input-wav");
    } else if (key == "--output-wav") {
      args.output_wav = need_value("--output-wav");
    } else if (key == "--provider") {
      args.provider = need_value("--provider");
    } else if (key == "--plugin") {
      args.plugin_path = need_value("--plugin");
    } else if (key == "--dawn-backend") {
      args.dawn_backend = need_value("--dawn-backend");
    } else if (key == "--power-preference") {
      args.power_preference = need_value("--power-preference");
    } else if (key == "--frames") {
      args.frames = std::stoi(need_value("--frames"));
    } else if (key == "--start-frame") {
      args.start_frame = std::stoi(need_value("--start-frame"));
    } else if (key == "--max-frames") {
      args.max_frames = std::stoi(need_value("--max-frames"));
    } else if (key == "--sample-rate") {
      args.sample_rate = std::stoi(need_value("--sample-rate"));
    } else if (key == "--warmup") {
      args.warmup = std::stoi(need_value("--warmup"));
    } else if (key == "--runs") {
      args.runs = std::stoi(need_value("--runs"));
    } else if (key == "--threads") {
      args.threads = std::stoi(need_value("--threads"));
    } else if (key == "--allow-cpu-fallback") {
      args.disable_cpu_fallback = false;
    } else if (key == "--print-feature-stats") {
      args.print_feature_stats = true;
    } else if (key == "--help" || key == "-h") {
      PrintUsage(argv[0]);
      std::exit(0);
    } else {
      throw std::runtime_error("unknown argument: " + key);
    }
  }

  args.provider = Lowercase(args.provider);
  args.dawn_backend = Lowercase(args.dawn_backend);
  args.power_preference = Lowercase(args.power_preference);

  if (args.frames <= 0 || args.start_frame < 0 || args.max_frames < 0 || args.sample_rate <= 0 ||
      args.warmup < 0 || args.runs <= 0 || args.threads <= 0) {
    throw std::runtime_error(
        "frames/sample-rate/runs/threads must be positive, start-frame/max-frames/warmup must be non-negative");
  }
  if (args.provider != "webgpu" && args.provider != "cpu") {
    throw std::runtime_error("unsupported provider: " + args.provider);
  }
  if (!args.dawn_backend.empty() && args.dawn_backend != "auto" &&
      args.dawn_backend != "d3d12" && args.dawn_backend != "vulkan") {
    throw std::runtime_error("unsupported dawn backend: " + args.dawn_backend);
  }
  if (args.power_preference != "high-performance" && args.power_preference != "low-power") {
    throw std::runtime_error("unsupported power preference: " + args.power_preference);
  }
  if (!args.output_wav.empty() && args.input_wav.empty()) {
    throw std::runtime_error("--output-wav requires --input-wav");
  }
  return args;
}

std::basic_string<ORTCHAR_T> ToOrtPath(const fs::path& path) {
#ifdef _WIN32
  return path.wstring();
#else
  return path.string();
#endif
}

std::string NativePathString(const fs::path& path) {
  return path.u8string();
}

fs::path ExecutableDir(const char* argv0) {
#if defined(_WIN32)
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size == 0) {
      break;
    }
    if (size < buffer.size() - 1) {
      buffer.resize(size);
      return fs::path(buffer).parent_path();
    }
    buffer.resize(buffer.size() * 2);
  }
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buffer(size, '\0');
  if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
    return fs::weakly_canonical(fs::path(buffer)).parent_path();
  }
#elif defined(__linux__)
  std::string buffer(4096, '\0');
  const ssize_t size = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (size > 0) {
    buffer.resize(static_cast<size_t>(size));
    return fs::path(buffer).parent_path();
  }
#endif
  fs::path fallback(argv0 ? argv0 : "");
  if (!fallback.parent_path().empty()) {
    return fs::weakly_canonical(fallback).parent_path();
  }
  return fs::current_path();
}

fs::path DefaultWebGpuPluginPath(const fs::path& executable_dir) {
#if defined(_WIN32)
  return executable_dir / "onnxruntime_providers_webgpu.dll";
#elif defined(__APPLE__)
  return executable_dir / "libonnxruntime_providers_webgpu.dylib";
#else
  return executable_dir / "libonnxruntime_providers_webgpu.so";
#endif
}

std::string DawnBackendOptionValue(const std::string& backend) {
  if (backend.empty() || backend == "auto") {
    return "";
  }
  if (backend == "d3d12") {
    return "D3D12";
  }
  if (backend == "vulkan") {
    return "Vulkan";
  }
  throw std::runtime_error("unsupported dawn backend: " + backend);
}

std::vector<float> MakeMel(int frames) {
  std::vector<float> mel(static_cast<size_t>(128) * frames);
  for (int c = 0; c < 128; ++c) {
    for (int t = 0; t < frames; ++t) {
      const float x = static_cast<float>(std::sin(0.013 * c + 0.031 * t));
      mel[static_cast<size_t>(c) * frames + t] = x * 0.5f;
    }
  }
  return mel;
}

std::vector<float> MakeF0(int frames) {
  std::vector<float> f0(frames);
  for (int t = 0; t < frames; ++t) {
    f0[t] = 180.0f + 40.0f * static_cast<float>(std::sin(0.05 * t));
  }
  return f0;
}

struct InputTensors {
  int frames = 0;
  std::vector<float> mel;
  std::vector<float> f0;
};

InputTensors MakeSyntheticInputs(int frames) {
  return InputTensors{frames, MakeMel(frames), MakeF0(frames)};
}

void PrintVectorStats(const char* name, const std::vector<float>& values) {
  float min_v = values.empty() ? 0.0f : values[0];
  float max_v = values.empty() ? 0.0f : values[0];
  double sum = 0.0;
  for (float x : values) {
    min_v = std::min(min_v, x);
    max_v = std::max(max_v, x);
    sum += x;
  }
  std::cout << name << "_min=" << min_v
            << " " << name << "_max=" << max_v
            << " " << name << "_mean=" << (values.empty() ? 0.0 : sum / values.size()) << '\n';
}

InputTensors MakeAudioInputs(const Args& args) {
  AudioBuffer input = ReadWavMono(args.input_wav);
  std::vector<float> audio = ResampleLinear(input.samples, input.sample_rate, 44100);
  VocoderFeatures features = ExtractVocoderFeatures(audio, 44100, args.start_frame, args.max_frames);
  if (args.print_feature_stats) {
    PrintVectorStats("mel", features.mel);
    PrintVectorStats("f0", features.f0);
  }
  return InputTensors{features.frames, std::move(features.mel), std::move(features.f0)};
}

void PrintStats(const std::vector<double>& ms, int frames) {
  std::vector<double> sorted = ms;
  std::sort(sorted.begin(), sorted.end());
  const double sum = std::accumulate(sorted.begin(), sorted.end(), 0.0);
  const double mean = sum / sorted.size();
  const double p50 = sorted[sorted.size() / 2];
  const double p90 = sorted[static_cast<size_t>((sorted.size() - 1) * 0.90)];
  const double min_v = sorted.front();
  const double max_v = sorted.back();
  const double audio_ms = static_cast<double>(frames) * 512.0 / 44100.0 * 1000.0;

  std::cout << std::fixed << std::setprecision(3);
  std::cout << "latency_ms mean=" << mean
            << " p50=" << p50
            << " p90=" << p90
            << " min=" << min_v
            << " max=" << max_v << '\n';
  std::cout << "audio_ms=" << audio_ms
            << " realtime_factor=" << (mean / audio_ms)
            << " throughput_x_realtime=" << (audio_ms / mean) << '\n';
}

Ort::ConstEpDevice FindWebGpuDevice(Ort::Env& env) {
  auto devices = env.GetEpDevices();
  std::cout << "ep_devices=" << devices.size() << '\n';
  for (const auto& device : devices) {
    const char* name = device.EpName();
    const char* vendor = device.EpVendor();
    std::cout << "  ep=" << (name ? name : "")
              << " vendor=" << (vendor ? vendor : "") << '\n';
    if (name != nullptr && std::strcmp(name, "WebGpuExecutionProvider") == 0) {
      return device;
    }
  }
  throw std::runtime_error("no WebGpuExecutionProvider OrtEpDevice found");
}

std::unordered_map<std::string, std::string> MakeWebGpuOptions(const Args& args) {
  std::unordered_map<std::string, std::string> options;
  options["ep.webgpuexecutionprovider.powerPreference"] = args.power_preference;
  const std::string dawn_backend = DawnBackendOptionValue(args.dawn_backend);
  if (!dawn_backend.empty()) {
    options["ep.webgpuexecutionprovider.dawnBackendType"] = dawn_backend;
  }
  return options;
}

Ort::SessionOptions MakeSessionOptions(Ort::Env& env, const Args& args, const fs::path& executable_dir) {
  Ort::SessionOptions options;
  options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  options.SetIntraOpNumThreads(args.threads);
  options.SetInterOpNumThreads(args.threads);

  if (args.provider != "cpu" && args.disable_cpu_fallback) {
    options.AddConfigEntry("session.disable_cpu_ep_fallback", "1");
  }

  if (args.provider == "webgpu") {
    const fs::path plugin_path = args.plugin_path.empty() ? DefaultWebGpuPluginPath(executable_dir) : args.plugin_path;
    const std::basic_string<ORTCHAR_T> ort_plugin_path = ToOrtPath(plugin_path);
    env.RegisterExecutionProviderLibrary("webgpu_plugin", ort_plugin_path);
    auto device = FindWebGpuDevice(env);
    options.AppendExecutionProvider_V2(env, std::vector<Ort::ConstEpDevice>{device}, MakeWebGpuOptions(args));
  } else if (args.provider == "cpu") {
    // CPUExecutionProvider is added by ONNX Runtime by default.
  }

  return options;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Args args = ParseArgs(argc, argv);
    const fs::path executable_dir = ExecutableDir(argv[0]);

    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "ort_webgpu_bench");
    Ort::SessionOptions options = MakeSessionOptions(env, args, executable_dir);

    const auto session_create_begin = std::chrono::steady_clock::now();
    const std::basic_string<ORTCHAR_T> model_path = ToOrtPath(args.model_path);
    Ort::Session session(env, model_path.c_str(), options);
    const auto session_create_end = std::chrono::steady_clock::now();
    const double session_create_ms =
        std::chrono::duration<double, std::milli>(session_create_end - session_create_begin).count();

    InputTensors input_tensors = args.input_wav.empty()
                                    ? MakeSyntheticInputs(args.frames)
                                    : MakeAudioInputs(args);

    Ort::AllocatorWithDefaultOptions allocator;
    const size_t input_count = session.GetInputCount();
    const size_t output_count = session.GetOutputCount();
    std::cout << "provider=" << args.provider
              << " model=" << NativePathString(args.model_path)
              << " input_wav=" << (args.input_wav.empty() ? "" : NativePathString(args.input_wav))
              << " output_wav=" << (args.output_wav.empty() ? "" : NativePathString(args.output_wav))
              << " frames=" << input_tensors.frames
              << " start_frame=" << args.start_frame
              << " max_frames=" << args.max_frames
              << " warmup=" << (args.input_wav.empty() ? args.warmup : 0)
              << " runs=" << (args.input_wav.empty() ? args.runs : 1)
              << " threads=" << args.threads
              << " disable_cpu_fallback=" << (args.disable_cpu_fallback ? "true" : "false") << '\n';
    if (args.provider == "webgpu") {
      const fs::path plugin_path = args.plugin_path.empty() ? DefaultWebGpuPluginPath(executable_dir) : args.plugin_path;
      std::cout << "webgpu_plugin=" << NativePathString(plugin_path)
                << " dawn_backend=" << (args.dawn_backend.empty() ? "auto" : args.dawn_backend)
                << " power_preference=" << args.power_preference << '\n';
    }
    std::cout << "session_create_ms=" << std::fixed << std::setprecision(3) << session_create_ms << '\n';

    for (size_t i = 0; i < input_count; ++i) {
      auto name = session.GetInputNameAllocated(i, allocator);
      auto info = session.GetInputTypeInfo(i).GetTensorTypeAndShapeInfo();
      std::cout << "input[" << i << "] " << name.get() << " dims=";
      for (auto dim : info.GetShape()) {
        std::cout << dim << ' ';
      }
      std::cout << '\n';
    }
    for (size_t i = 0; i < output_count; ++i) {
      auto name = session.GetOutputNameAllocated(i, allocator);
      auto info = session.GetOutputTypeInfo(i).GetTensorTypeAndShapeInfo();
      std::cout << "output[" << i << "] " << name.get() << " dims=";
      for (auto dim : info.GetShape()) {
        std::cout << dim << ' ';
      }
      std::cout << '\n';
    }

    std::array<int64_t, 3> mel_shape{1, 128, input_tensors.frames};
    std::array<int64_t, 2> f0_shape{1, input_tensors.frames};

    Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value mel_value = Ort::Value::CreateTensor<float>(memory_info, input_tensors.mel.data(), input_tensors.mel.size(),
                                                           mel_shape.data(), mel_shape.size());
    Ort::Value f0_value = Ort::Value::CreateTensor<float>(memory_info, input_tensors.f0.data(), input_tensors.f0.size(),
                                                          f0_shape.data(), f0_shape.size());
    std::vector<Ort::Value> inputs;
    inputs.emplace_back(std::move(mel_value));
    inputs.emplace_back(std::move(f0_value));

    const char* input_names[] = {"mel", "f0"};
    const char* output_names[] = {"audio"};

    auto run_once = [&]() {
      return session.Run(Ort::RunOptions{nullptr},
                         input_names, inputs.data(), inputs.size(),
                         output_names, 1);
    };

    std::vector<double> timings;
    timings.reserve(args.input_wav.empty() ? args.runs : 1);
    Ort::Value last_output{nullptr};

    if (args.input_wav.empty()) {
      for (int i = 0; i < args.warmup; ++i) {
        auto outputs = run_once();
        (void)outputs;
      }
    }

    const int run_count = args.input_wav.empty() ? args.runs : 1;
    for (int i = 0; i < run_count; ++i) {
      const auto begin = std::chrono::steady_clock::now();
      auto outputs = run_once();
      const auto end = std::chrono::steady_clock::now();
      timings.push_back(std::chrono::duration<double, std::milli>(end - begin).count());
      last_output = std::move(outputs[0]);
    }

    auto out_info = last_output.GetTensorTypeAndShapeInfo();
    auto out_shape = out_info.GetShape();
    const float* out = last_output.GetTensorData<float>();
    const size_t out_count = out_info.GetElementCount();
    float min_v = out[0];
    float max_v = out[0];
    double mean = 0.0;
    for (size_t i = 0; i < out_count; ++i) {
      min_v = std::min(min_v, out[i]);
      max_v = std::max(max_v, out[i]);
      mean += out[i];
    }
    mean /= static_cast<double>(out_count);

    std::cout << "output_shape=";
    for (auto dim : out_shape) {
      std::cout << dim << ' ';
    }
    std::cout << " output_min=" << min_v
              << " output_max=" << max_v
              << " output_mean=" << mean << '\n';
    PrintStats(timings, input_tensors.frames);

    if (!args.output_wav.empty()) {
      std::vector<float> audio(out, out + out_count);
      WriteWavMono16(args.output_wav, args.sample_rate, audio);
      std::cout << "wrote_wav=" << NativePathString(args.output_wav)
                << " sample_rate=" << args.sample_rate
                << " samples=" << audio.size() << '\n';
    }

    return 0;
  } catch (const Ort::Exception& e) {
    std::cerr << "ORT error: " << e.what() << '\n';
    return 2;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << '\n';
    PrintUsage(argv[0]);
    return 1;
  }
}
