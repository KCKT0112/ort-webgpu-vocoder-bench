#include "features.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <stdexcept>

namespace {

constexpr int kSampleRate = 44100;
constexpr int kNumMels = 128;
constexpr int kFftSize = 2048;
constexpr int kWinSize = 2048;
constexpr int kHopSize = 512;
constexpr float kFMin = 40.0f;
constexpr float kFMax = 16000.0f;
constexpr float kF0Min = 65.0f;
constexpr float kF0Max = 1100.0f;
constexpr float kPi = 3.14159265358979323846f;

float HzToMel(float hz) {
  return 2595.0f * std::log10(1.0f + hz / 700.0f);
}

float MelToHz(float mel) {
  return 700.0f * (std::pow(10.0f, mel / 2595.0f) - 1.0f);
}

int ReflectIndex(int index, int size) {
  if (size <= 1) {
    return 0;
  }
  while (index < 0 || index >= size) {
    if (index < 0) {
      index = -index;
    }
    if (index >= size) {
      index = 2 * size - index - 2;
    }
  }
  return index;
}

std::vector<float> ReflectPad(const std::vector<float>& input, int left, int right) {
  std::vector<float> output(static_cast<size_t>(left + input.size() + right));
  for (int i = 0; i < static_cast<int>(output.size()); ++i) {
    output[static_cast<size_t>(i)] = input[static_cast<size_t>(ReflectIndex(i - left, static_cast<int>(input.size())))];
  }
  return output;
}

std::vector<float> HannWindow(int size) {
  std::vector<float> window(size);
  for (int i = 0; i < size; ++i) {
    window[static_cast<size_t>(i)] = 0.5f - 0.5f * std::cos(2.0f * kPi * i / size);
  }
  return window;
}

std::vector<float> CreateMelFilterbank() {
  const int freq_bins = kFftSize / 2 + 1;
  std::vector<float> filters(static_cast<size_t>(kNumMels * freq_bins), 0.0f);

  const float mel_min = HzToMel(kFMin);
  const float mel_max = HzToMel(kFMax);
  std::vector<float> hz_points(kNumMels + 2);
  std::vector<int> bins(kNumMels + 2);
  for (int i = 0; i < kNumMels + 2; ++i) {
    const float mel = mel_min + (mel_max - mel_min) * i / (kNumMels + 1);
    hz_points[static_cast<size_t>(i)] = MelToHz(mel);
    bins[static_cast<size_t>(i)] = static_cast<int>(std::floor((kFftSize + 1) * hz_points[static_cast<size_t>(i)] / kSampleRate));
    bins[static_cast<size_t>(i)] = std::clamp(bins[static_cast<size_t>(i)], 0, freq_bins - 1);
  }

  for (int m = 1; m <= kNumMels; ++m) {
    const int left = bins[static_cast<size_t>(m - 1)];
    const int center = bins[static_cast<size_t>(m)];
    const int right = bins[static_cast<size_t>(m + 1)];
    if (center <= left || right <= center) {
      continue;
    }
    for (int k = left; k < center; ++k) {
      filters[static_cast<size_t>((m - 1) * freq_bins + k)] = static_cast<float>(k - left) / (center - left);
    }
    for (int k = center; k < right; ++k) {
      filters[static_cast<size_t>((m - 1) * freq_bins + k)] = static_cast<float>(right - k) / (right - center);
    }
  }

  return filters;
}

void Fft(std::vector<std::complex<float>>& values) {
  const size_t n = values.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) {
      j ^= bit;
    }
    j ^= bit;
    if (i < j) {
      std::swap(values[i], values[j]);
    }
  }

  for (size_t len = 2; len <= n; len <<= 1) {
    const float angle = -2.0f * kPi / static_cast<float>(len);
    const std::complex<float> wlen(std::cos(angle), std::sin(angle));
    for (size_t i = 0; i < n; i += len) {
      std::complex<float> w(1.0f, 0.0f);
      for (size_t j = 0; j < len / 2; ++j) {
        const std::complex<float> u = values[i + j];
        const std::complex<float> v = values[i + j + len / 2] * w;
        values[i + j] = u + v;
        values[i + j + len / 2] = u - v;
        w *= wlen;
      }
    }
  }
}

void FftMagnitude(const std::vector<float>& frame, std::vector<float>& magnitude) {
  const int freq_bins = kFftSize / 2 + 1;
  magnitude.assign(freq_bins, 0.0f);
  std::vector<std::complex<float>> values(static_cast<size_t>(kFftSize));
  for (int i = 0; i < kFftSize; ++i) {
    values[static_cast<size_t>(i)] = std::complex<float>(frame[static_cast<size_t>(i)], 0.0f);
  }
  Fft(values);
  for (int k = 0; k < freq_bins; ++k) {
    magnitude[static_cast<size_t>(k)] = std::abs(values[static_cast<size_t>(k)]);
  }
}

float EstimateF0ForFrame(const std::vector<float>& audio, int center) {
  const int min_lag = static_cast<int>(std::floor(kSampleRate / kF0Max));
  const int max_lag = static_cast<int>(std::ceil(kSampleRate / kF0Min));
  const int half_window = kWinSize / 2;
  const int start = center - half_window;

  double energy = 0.0;
  for (int i = 0; i < kWinSize; ++i) {
    const float x = audio[static_cast<size_t>(ReflectIndex(start + i, static_cast<int>(audio.size())))];
    energy += x * x;
  }
  if (energy < 1e-6) {
    return 0.0f;
  }

  int best_lag = 0;
  double best_corr = 0.0;
  for (int lag = min_lag; lag <= max_lag; ++lag) {
    double corr = 0.0;
    double e0 = 0.0;
    double e1 = 0.0;
    const int count = kWinSize - lag;
    for (int i = 0; i < count; ++i) {
      const float a = audio[static_cast<size_t>(ReflectIndex(start + i, static_cast<int>(audio.size())))];
      const float b = audio[static_cast<size_t>(ReflectIndex(start + i + lag, static_cast<int>(audio.size())))];
      corr += a * b;
      e0 += a * a;
      e1 += b * b;
    }
    const double denom = std::sqrt(e0 * e1) + 1e-9;
    const double norm_corr = corr / denom;
    if (norm_corr > best_corr) {
      best_corr = norm_corr;
      best_lag = lag;
    }
  }

  if (best_lag == 0 || best_corr < 0.25) {
    return 0.0f;
  }
  return static_cast<float>(kSampleRate) / best_lag;
}

}  // namespace

VocoderFeatures ExtractVocoderFeatures(const std::vector<float>& audio, int sample_rate) {
  if (sample_rate != kSampleRate) {
    throw std::runtime_error("ExtractVocoderFeatures expects 44100 Hz audio");
  }
  if (audio.empty()) {
    throw std::runtime_error("empty audio input");
  }

  const int left_pad = (kWinSize - kHopSize) / 2;
  const int right_pad = (kWinSize - kHopSize + 1) / 2;
  const std::vector<float> padded = ReflectPad(audio, left_pad, right_pad);
  if (static_cast<int>(padded.size()) < kFftSize) {
    throw std::runtime_error("audio is too short for feature extraction");
  }

  const int frames = 1 + (static_cast<int>(padded.size()) - kFftSize) / kHopSize;
  const int freq_bins = kFftSize / 2 + 1;
  const auto window = HannWindow(kWinSize);
  const auto mel_filters = CreateMelFilterbank();

  VocoderFeatures features;
  features.frames = frames;
  features.mel.assign(static_cast<size_t>(kNumMels * frames), 0.0f);
  features.f0.assign(static_cast<size_t>(frames), 0.0f);

  std::vector<float> frame(kFftSize, 0.0f);
  std::vector<float> magnitude;
  for (int t = 0; t < frames; ++t) {
    const int offset = t * kHopSize;
    std::fill(frame.begin(), frame.end(), 0.0f);
    for (int i = 0; i < kWinSize; ++i) {
      frame[static_cast<size_t>(i)] = padded[static_cast<size_t>(offset + i)] * window[static_cast<size_t>(i)];
    }

    FftMagnitude(frame, magnitude);
    for (int m = 0; m < kNumMels; ++m) {
      double value = 0.0;
      for (int k = 0; k < freq_bins; ++k) {
        value += mel_filters[static_cast<size_t>(m * freq_bins + k)] * magnitude[static_cast<size_t>(k)];
      }
      features.mel[static_cast<size_t>(m * frames + t)] =
          static_cast<float>(std::log(std::max(value, 1e-9)));
    }

    features.f0[static_cast<size_t>(t)] = EstimateF0ForFrame(audio, t * kHopSize);
  }

  return features;
}
