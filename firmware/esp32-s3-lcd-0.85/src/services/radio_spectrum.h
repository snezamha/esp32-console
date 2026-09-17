#pragma once

#include <cmath>
#include <cstdint>

namespace radio_spectrum {
constexpr int kBands = 8;
constexpr int kWindow = 512;

// Measure eight frequencies in the decoded PCM that is about to reach the speaker.
// One MP3 frame is enough; no FFT buffer or allocation is needed on the audio task.
inline void Analyze(const int16_t* pcm, int frames, int channels, int sample_rate,
                    int volume, uint8_t (&levels)[kBands]) {
  for (auto& level : levels) level = 0;
  if (!pcm || frames < kWindow || (channels != 1 && channels != 2) || sample_rate < 8000 || volume <= 0) return;

  float window[kWindow];
  const int start = (frames - kWindow) / 2;
  int channel = 0;
  if (channels == 2) {
    uint32_t left = 0, right = 0;
    for (int i = 0; i < kWindow; ++i) {
      const int l = pcm[(start + i) * 2], r = pcm[(start + i) * 2 + 1];
      left += l < 0 ? -l : l;
      right += r < 0 ? -r : r;
    }
    channel = right > left ? 1 : 0;
  }
  float mean = 0;
  for (int i = 0; i < kWindow; ++i) {
    const float sample = pcm[(start + i) * channels + channel] * (1.0f / 32768.0f);
    window[i] = sample;
    mean += sample;
  }
  mean /= kWindow;
  for (int i = 0; i < kWindow; ++i) {
    // Triangle taper reduces leakage from frequencies between Goertzel bins.
    const float taper = 1.0f - std::fabs((2.0f * i - (kWindow - 1)) / (kWindow - 1));
    window[i] = (window[i] - mean) * taper;
  }

  constexpr int centers[kBands] = {100, 200, 400, 800, 1600, 3200, 6400, 10000};
  for (int band = 0; band < kBands; ++band) {
    if (centers[band] >= sample_rate / 2) continue;
    int bin = static_cast<int>(centers[band] * kWindow / static_cast<float>(sample_rate) + 0.5f);
    if (bin < 1) bin = 1;
    const float coefficient = 2.0f * std::cos(6.28318530718f * bin / kWindow);
    float previous = 0, before_previous = 0;
    for (float sample : window) {
      const float current = sample + coefficient * previous - before_previous;
      before_previous = previous;
      previous = current;
    }
    const float power = previous * previous + before_previous * before_previous - coefficient * previous * before_previous;
    const float amplitude = std::sqrt(power > 0 ? power : 0) * (4.0f / kWindow) * (volume / 100.0f);
    const float db = 20.0f * std::log10(amplitude + 0.00001f);
    const int level = static_cast<int>((db + 55.0f) * (100.0f / 55.0f));
    levels[band] = static_cast<uint8_t>(level < 0 ? 0 : level > 100 ? 100 : level);
  }
}
}  // namespace radio_spectrum
