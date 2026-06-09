#include "features.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
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

float HzToMelSlaney(float hz) {
  constexpr float f_sp = 200.0f / 3.0f;
  constexpr float min_log_hz = 1000.0f;
  constexpr float min_log_mel = min_log_hz / f_sp;
  constexpr float logstep = 0.06875177742094912f;  // ln(6.4) / 27
  if (hz < min_log_hz) {
    return hz / f_sp;
  }
  return min_log_mel + std::log(hz / min_log_hz) / logstep;
}

float MelToHzSlaney(float mel) {
  constexpr float f_sp = 200.0f / 3.0f;
  constexpr float min_log_hz = 1000.0f;
  constexpr float min_log_mel = min_log_hz / f_sp;
  constexpr float logstep = 0.06875177742094912f;  // ln(6.4) / 27
  if (mel < min_log_mel) {
    return mel * f_sp;
  }
  return min_log_hz * std::exp(logstep * (mel - min_log_mel));
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

  const float mel_min = HzToMelSlaney(kFMin);
  const float mel_max = HzToMelSlaney(kFMax);
  std::vector<float> hz_points(kNumMels + 2);
  for (int i = 0; i < kNumMels + 2; ++i) {
    const float mel = mel_min + (mel_max - mel_min) * i / (kNumMels + 1);
    hz_points[static_cast<size_t>(i)] = MelToHzSlaney(mel);
  }

  for (int m = 0; m < kNumMels; ++m) {
    const float left = hz_points[static_cast<size_t>(m)];
    const float center = hz_points[static_cast<size_t>(m + 1)];
    const float right = hz_points[static_cast<size_t>(m + 2)];
    if (center <= left || right <= center) {
      continue;
    }
    const float enorm = 2.0f / (right - left);
    for (int k = 0; k < freq_bins; ++k) {
      const float fft_hz = static_cast<float>(kSampleRate) * k / kFftSize;
      const float lower = (fft_hz - left) / (center - left);
      const float upper = (right - fft_hz) / (right - center);
      filters[static_cast<size_t>(m * freq_bins + k)] = std::max(0.0f, std::min(lower, upper)) * enorm;
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

  std::vector<double> diff(static_cast<size_t>(max_lag + 1), 0.0);
  for (int lag = 1; lag <= max_lag; ++lag) {
    double sum = 0.0;
    for (int i = 0; i < kWinSize - lag; ++i) {
      const float a = audio[static_cast<size_t>(ReflectIndex(start + i, static_cast<int>(audio.size())))];
      const float b = audio[static_cast<size_t>(ReflectIndex(start + i + lag, static_cast<int>(audio.size())))];
      const double d = static_cast<double>(a) - b;
      sum += d * d;
    }
    diff[static_cast<size_t>(lag)] = sum;
  }

  std::vector<double> cmndf(static_cast<size_t>(max_lag + 1), 1.0);
  double running_sum = 0.0;
  for (int lag = 1; lag <= max_lag; ++lag) {
    running_sum += diff[static_cast<size_t>(lag)];
    cmndf[static_cast<size_t>(lag)] = diff[static_cast<size_t>(lag)] * lag / (running_sum + 1e-12);
  }

  constexpr double threshold = 0.12;
  int best_lag = 0;
  for (int lag = min_lag; lag <= max_lag; ++lag) {
    if (cmndf[static_cast<size_t>(lag)] < threshold) {
      while (lag + 1 <= max_lag &&
             cmndf[static_cast<size_t>(lag + 1)] < cmndf[static_cast<size_t>(lag)]) {
        ++lag;
      }
      best_lag = lag;
      break;
    }
  }

  if (best_lag == 0) {
    double best_value = std::numeric_limits<double>::infinity();
    for (int lag = min_lag; lag <= max_lag; ++lag) {
      const double value = cmndf[static_cast<size_t>(lag)];
      if (value < best_value) {
        best_value = value;
        best_lag = lag;
      }
    }
    if (best_value > 0.35) {
      return 0.0f;
    }
  }

  double refined_lag = best_lag;
  if (best_lag > min_lag && best_lag < max_lag) {
    const double left = cmndf[static_cast<size_t>(best_lag - 1)];
    const double mid = cmndf[static_cast<size_t>(best_lag)];
    const double right = cmndf[static_cast<size_t>(best_lag + 1)];
    const double denom = left - 2.0 * mid + right;
    if (std::abs(denom) > 1e-12) {
      refined_lag += 0.5 * (left - right) / denom;
    }
  }
  if (refined_lag <= 0.0) {
    return 0.0f;
  }
  return static_cast<float>(kSampleRate / refined_lag);
}

void InterpolateUnvoicedF0(std::vector<float>& f0) {
  std::vector<int> voiced;
  voiced.reserve(f0.size());
  for (int i = 0; i < static_cast<int>(f0.size()); ++i) {
    if (f0[static_cast<size_t>(i)] > 0.0f) {
      voiced.push_back(i);
    }
  }
  if (voiced.empty()) {
    return;
  }
  for (int i = 0; i < voiced.front(); ++i) {
    f0[static_cast<size_t>(i)] = f0[static_cast<size_t>(voiced.front())];
  }
  for (size_t k = 0; k + 1 < voiced.size(); ++k) {
    const int left = voiced[k];
    const int right = voiced[k + 1];
    const float left_f0 = f0[static_cast<size_t>(left)];
    const float right_f0 = f0[static_cast<size_t>(right)];
    for (int i = left + 1; i < right; ++i) {
      const float alpha = static_cast<float>(i - left) / static_cast<float>(right - left);
      const float log_f0 = std::log2(left_f0) * (1.0f - alpha) + std::log2(right_f0) * alpha;
      f0[static_cast<size_t>(i)] = std::exp2(log_f0);
    }
  }
  for (int i = voiced.back() + 1; i < static_cast<int>(f0.size()); ++i) {
    f0[static_cast<size_t>(i)] = f0[static_cast<size_t>(voiced.back())];
  }
}

}  // namespace

VocoderFeatures ExtractVocoderFeatures(const std::vector<float>& audio,
                                       int sample_rate,
                                       int start_frame,
                                       int max_frames) {
  if (sample_rate != kSampleRate) {
    throw std::runtime_error("ExtractVocoderFeatures expects 44100 Hz audio");
  }
  if (audio.empty()) {
    throw std::runtime_error("empty audio input");
  }
  if (start_frame < 0 || max_frames < 0) {
    throw std::runtime_error("start_frame and max_frames must be non-negative");
  }

  const int left_pad = (kWinSize - kHopSize) / 2;
  const int right_pad = (kWinSize - kHopSize + 1) / 2;
  const std::vector<float> padded = ReflectPad(audio, left_pad, right_pad);
  if (static_cast<int>(padded.size()) < kFftSize) {
    throw std::runtime_error("audio is too short for feature extraction");
  }

  const int total_frames = 1 + (static_cast<int>(padded.size()) - kFftSize) / kHopSize;
  if (start_frame >= total_frames) {
    throw std::runtime_error("start frame is beyond extracted feature length");
  }
  const int available_frames = total_frames - start_frame;
  const int frames = max_frames > 0 ? std::min(max_frames, available_frames) : available_frames;
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
    const int absolute_frame = start_frame + t;
    const int offset = absolute_frame * kHopSize;
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

    features.f0[static_cast<size_t>(t)] =
        EstimateF0ForFrame(audio, absolute_frame * kHopSize + kHopSize / 2);
  }

  InterpolateUnvoicedF0(features.f0);
  return features;
}
