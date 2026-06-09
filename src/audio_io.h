#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

struct AudioBuffer {
  int sample_rate = 0;
  std::vector<float> samples;
};

AudioBuffer ReadWavMono(const std::filesystem::path& path);
void WriteWavMono16(const std::filesystem::path& path, int sample_rate, const std::vector<float>& samples);
std::vector<float> ResampleLinear(const std::vector<float>& input, int input_rate, int output_rate);
