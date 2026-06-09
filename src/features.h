#pragma once

#include <vector>

struct VocoderFeatures {
  int frames = 0;
  std::vector<float> mel;  // [128, frames]
  std::vector<float> f0;   // [frames]
};

VocoderFeatures ExtractVocoderFeatures(const std::vector<float>& audio,
                                       int sample_rate,
                                       int start_frame = 0,
                                       int max_frames = 0);
