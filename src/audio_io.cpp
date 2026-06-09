#include "audio_io.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

uint16_t ReadU16(std::istream& in) {
  std::array<unsigned char, 2> b{};
  in.read(reinterpret_cast<char*>(b.data()), b.size());
  if (!in) {
    throw std::runtime_error("unexpected end of wav file");
  }
  return static_cast<uint16_t>(b[0] | (b[1] << 8));
}

uint32_t ReadU32(std::istream& in) {
  std::array<unsigned char, 4> b{};
  in.read(reinterpret_cast<char*>(b.data()), b.size());
  if (!in) {
    throw std::runtime_error("unexpected end of wav file");
  }
  return static_cast<uint32_t>(b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24));
}

std::string ReadTag(std::istream& in) {
  std::array<char, 4> tag{};
  in.read(tag.data(), tag.size());
  if (!in) {
    throw std::runtime_error("unexpected end of wav file");
  }
  return std::string(tag.data(), tag.size());
}

void WriteU16(std::ostream& out, uint16_t value) {
  const std::array<unsigned char, 2> b{
      static_cast<unsigned char>(value & 0xff),
      static_cast<unsigned char>((value >> 8) & 0xff),
  };
  out.write(reinterpret_cast<const char*>(b.data()), b.size());
}

void WriteU32(std::ostream& out, uint32_t value) {
  const std::array<unsigned char, 4> b{
      static_cast<unsigned char>(value & 0xff),
      static_cast<unsigned char>((value >> 8) & 0xff),
      static_cast<unsigned char>((value >> 16) & 0xff),
      static_cast<unsigned char>((value >> 24) & 0xff),
  };
  out.write(reinterpret_cast<const char*>(b.data()), b.size());
}

float ReadPcmSample(const unsigned char* p, int bits_per_sample) {
  if (bits_per_sample == 8) {
    return (static_cast<int>(p[0]) - 128) / 128.0f;
  }
  if (bits_per_sample == 16) {
    const int16_t v = static_cast<int16_t>(p[0] | (p[1] << 8));
    return std::max(-1.0f, static_cast<float>(v) / 32768.0f);
  }
  if (bits_per_sample == 24) {
    int32_t v = static_cast<int32_t>(p[0] | (p[1] << 8) | (p[2] << 16));
    if (v & 0x800000) {
      v |= ~0xFFFFFF;
    }
    return std::max(-1.0f, static_cast<float>(v) / 8388608.0f);
  }
  if (bits_per_sample == 32) {
    const int32_t v = static_cast<int32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
    return std::max(-1.0f, static_cast<float>(v) / 2147483648.0f);
  }
  throw std::runtime_error("unsupported PCM bit depth");
}

float ReadFloatSample(const unsigned char* p, int bits_per_sample) {
  if (bits_per_sample != 32) {
    throw std::runtime_error("only 32-bit float WAV is supported for IEEE float");
  }
  float value = 0.0f;
  std::copy(p, p + sizeof(float), reinterpret_cast<unsigned char*>(&value));
  return std::clamp(value, -1.0f, 1.0f);
}

}  // namespace

AudioBuffer ReadWavMono(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to open wav file: " + path.u8string());
  }

  if (ReadTag(in) != "RIFF") {
    throw std::runtime_error("not a RIFF wav file");
  }
  (void)ReadU32(in);
  if (ReadTag(in) != "WAVE") {
    throw std::runtime_error("not a WAVE file");
  }

  uint16_t audio_format = 0;
  uint16_t channels = 0;
  uint32_t sample_rate = 0;
  uint16_t bits_per_sample = 0;
  std::vector<unsigned char> data;

  while (in && (!sample_rate || data.empty())) {
    const std::string tag = ReadTag(in);
    const uint32_t chunk_size = ReadU32(in);
    const std::streampos next = in.tellg() + static_cast<std::streamoff>(chunk_size + (chunk_size & 1));

    if (tag == "fmt ") {
      audio_format = ReadU16(in);
      channels = ReadU16(in);
      sample_rate = ReadU32(in);
      (void)ReadU32(in);
      (void)ReadU16(in);
      bits_per_sample = ReadU16(in);
    } else if (tag == "data") {
      data.resize(chunk_size);
      in.read(reinterpret_cast<char*>(data.data()), data.size());
      if (!in) {
        throw std::runtime_error("unexpected end of wav data");
      }
    }

    in.seekg(next);
  }

  if (audio_format != 1 && audio_format != 3) {
    throw std::runtime_error("unsupported wav format: expected PCM or IEEE float");
  }
  if (channels == 0 || sample_rate == 0 || bits_per_sample == 0 || data.empty()) {
    throw std::runtime_error("invalid or incomplete wav file");
  }

  const size_t bytes_per_sample = bits_per_sample / 8;
  if (bytes_per_sample == 0 || data.size() % (bytes_per_sample * channels) != 0) {
    throw std::runtime_error("invalid wav data size");
  }
  const size_t frames = data.size() / (bytes_per_sample * channels);
  std::vector<float> mono(frames, 0.0f);
  for (size_t i = 0; i < frames; ++i) {
    double sum = 0.0;
    for (uint16_t ch = 0; ch < channels; ++ch) {
      const unsigned char* p = data.data() + (i * channels + ch) * bytes_per_sample;
      sum += audio_format == 3 ? ReadFloatSample(p, bits_per_sample) : ReadPcmSample(p, bits_per_sample);
    }
    mono[i] = static_cast<float>(sum / channels);
  }

  return AudioBuffer{static_cast<int>(sample_rate), std::move(mono)};
}

void WriteWavMono16(const std::filesystem::path& path, int sample_rate, const std::vector<float>& samples) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("failed to write wav file: " + path.u8string());
  }

  const uint16_t channels = 1;
  const uint16_t bits_per_sample = 16;
  const uint16_t block_align = channels * bits_per_sample / 8;
  const uint32_t byte_rate = static_cast<uint32_t>(sample_rate * block_align);
  const uint32_t data_size = static_cast<uint32_t>(samples.size() * block_align);
  const uint32_t riff_size = 4 + (8 + 16) + (8 + data_size);

  out.write("RIFF", 4);
  WriteU32(out, riff_size);
  out.write("WAVE", 4);
  out.write("fmt ", 4);
  WriteU32(out, 16);
  WriteU16(out, 1);
  WriteU16(out, channels);
  WriteU32(out, static_cast<uint32_t>(sample_rate));
  WriteU32(out, byte_rate);
  WriteU16(out, block_align);
  WriteU16(out, bits_per_sample);
  out.write("data", 4);
  WriteU32(out, data_size);

  for (float sample : samples) {
    const float clamped = std::clamp(sample, -1.0f, 1.0f);
    const auto v = static_cast<int16_t>(std::lrint(clamped * 32767.0f));
    WriteU16(out, static_cast<uint16_t>(v));
  }
}

std::vector<float> ResampleLinear(const std::vector<float>& input, int input_rate, int output_rate) {
  if (input_rate <= 0 || output_rate <= 0) {
    throw std::runtime_error("invalid sample rate");
  }
  if (input.empty() || input_rate == output_rate) {
    return input;
  }

  const double ratio = static_cast<double>(output_rate) / input_rate;
  const size_t output_size = std::max<size_t>(1, static_cast<size_t>(std::llround(input.size() * ratio)));
  std::vector<float> output(output_size);
  for (size_t i = 0; i < output_size; ++i) {
    const double src = static_cast<double>(i) / ratio;
    const size_t i0 = std::min(static_cast<size_t>(src), input.size() - 1);
    const size_t i1 = std::min(i0 + 1, input.size() - 1);
    const float frac = static_cast<float>(src - i0);
    output[i] = input[i0] * (1.0f - frac) + input[i1] * frac;
  }
  return output;
}
