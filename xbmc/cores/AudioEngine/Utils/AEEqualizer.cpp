/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AEEqualizer.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace
{
constexpr double PI = 3.14159265358979323846;
constexpr double BAND_Q = 1.41421356237; // one octave
constexpr double TINY = 1e-18; // flush denormals in the filter state
} // namespace

const std::array<float, CAEEqualizer::BANDS> CAEEqualizer::FREQUENCIES = {
    31.25f, 62.5f, 125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f};

void CAEEqualizer::SetParams(const Params& params)
{
  Params p = params;
  for (float& g : p.gainsDb)
    g = std::clamp(g, -MAX_GAIN_DB, MAX_GAIN_DB);
  p.preampDb = std::clamp(p.preampDb, -MAX_GAIN_DB, MAX_GAIN_DB);
  {
    std::lock_guard<std::mutex> lock(m_lock);
    m_params = p;
  }
  m_version++;
}

CAEEqualizer::Params CAEEqualizer::GetParams() const
{
  std::lock_guard<std::mutex> lock(m_lock);
  return m_params;
}

CAEEqualizer::Biquad CAEEqualizer::Peaking(double freq, double gainDb, double q, double sampleRate)
{
  // RBJ audio EQ cookbook peaking filter
  const double A = std::pow(10.0, gainDb / 40.0);
  const double w0 = 2.0 * PI * freq / sampleRate;
  const double alpha = std::sin(w0) / (2.0 * q);
  const double cosw = std::cos(w0);
  const double a0 = 1.0 + alpha / A;
  Biquad bq;
  bq.b0 = (1.0 + alpha * A) / a0;
  bq.b1 = (-2.0 * cosw) / a0;
  bq.b2 = (1.0 - alpha * A) / a0;
  bq.a1 = (-2.0 * cosw) / a0;
  bq.a2 = (1.0 - alpha / A) / a0;
  return bq;
}

float CAEEqualizer::PeakResponseDb(const std::array<float, BANDS>& gainsDb, int sampleRate)
{
  std::vector<Biquad> filters;
  for (int i = 0; i < BANDS; ++i)
    if (gainsDb[i] != 0.0f && FREQUENCIES[i] < 0.45f * sampleRate)
      filters.push_back(Peaking(FREQUENCIES[i], gainsDb[i], BAND_Q, sampleRate));
  if (filters.empty())
    return 0.0f;

  double peak = 0.0;
  // overlapping bands can add up past the largest single gain: measure the real response
  for (int k = 0; k <= 240; ++k)
  {
    const double f = 20.0 * std::pow(1000.0, k / 240.0); // 20 Hz .. 20 kHz
    if (f >= 0.5 * sampleRate)
      break;
    const std::complex<double> z = std::polar(1.0, -2.0 * PI * f / sampleRate);
    double mag = 1.0;
    for (const Biquad& bq : filters)
    {
      const std::complex<double> num = bq.b0 + bq.b1 * z + bq.b2 * z * z;
      const std::complex<double> den = 1.0 + bq.a1 * z + bq.a2 * z * z;
      mag *= std::abs(num / den);
    }
    peak = std::max(peak, mag);
  }
  return static_cast<float>(20.0 * std::log10(peak));
}

CAEEqualizer::Chain CAEEqualizer::Design(const Params& params, int sampleRate)
{
  Chain chain;
  if (!params.enabled || sampleRate <= 0)
    return chain;
  for (int i = 0; i < BANDS; ++i)
  {
    // a band at or above Nyquist can't be filtered (16 kHz at 32 kHz output)
    if (params.gainsDb[i] != 0.0f && FREQUENCIES[i] < 0.45f * sampleRate)
      chain.filters.push_back(Peaking(FREQUENCIES[i], params.gainsDb[i], BAND_Q, sampleRate));
  }
  double levelDb = params.preampDb;
  if (params.preventClipping)
    levelDb -= std::max(0.0f, PeakResponseDb(params.gainsDb, sampleRate));
  if (std::fabs(levelDb) > 0.001)
    chain.gain = std::pow(10.0, levelDb / 20.0);
  return chain;
}

void CAEEqualizer::Reset()
{
  std::fill(m_state.begin(), m_state.end(), 0.0);
}

void CAEEqualizer::RunChain(const Chain& chain,
                            std::vector<double>& state,
                            float* samples,
                            int stride,
                            int frames,
                            int channel,
                            int channels) const
{
  const size_t nf = chain.filters.size();
  const float gain = static_cast<float>(chain.gain);
  for (size_t f = 0; f < nf; ++f)
  {
    const Biquad& bq = chain.filters[f];
    double& z1 = state[(f * channels + channel) * 2];
    double& z2 = state[(f * channels + channel) * 2 + 1];
    float* s = samples;
    for (int i = 0; i < frames; ++i, s += stride)
    {
      // transposed direct form II
      const double x = *s;
      const double y = bq.b0 * x + z1;
      z1 = bq.b1 * x - bq.a1 * y + z2;
      z2 = bq.b2 * x - bq.a2 * y;
      *s = static_cast<float>(y);
    }
    if (std::fabs(z1) < TINY)
      z1 = 0.0;
    if (std::fabs(z2) < TINY)
      z2 = 0.0;
  }
  if (gain != 1.0f)
  {
    float* s = samples;
    for (int i = 0; i < frames; ++i, s += stride)
      *s *= gain;
  }
}

void CAEEqualizer::Process(float** data, int planes, int channels, int frames, int sampleRate)
{
  if (!data || channels <= 0 || frames <= 0 || sampleRate <= 0)
    return;

  const unsigned int version = m_version.load();
  const bool formatChanged = sampleRate != m_sampleRate || channels != m_channels;
  if (version == m_appliedVersion && !formatChanged)
  {
    if (!m_active)
      return;
  }
  else
  {
    const Chain newChain = Design(GetParams(), sampleRate);
    const bool newActive = !newChain.IsFlat();
    std::vector<double> newState(newChain.filters.size() * channels * 2, 0.0);

    // Crossfade over this buffer when the sound changes during playback (not on format changes,
    // where the old state means nothing, nor when nothing was or will be running).
    const bool crossfade = !formatChanged && (m_active || newActive);
    if (crossfade)
    {
      const int stride = planes == 1 ? channels : 1;
      for (int ch = 0; ch < channels; ++ch)
      {
        float* samples = planes == 1 ? data[0] + ch : data[ch];
        // old sound into scratch
        m_scratch.resize(frames);
        for (int i = 0; i < frames; ++i)
          m_scratch[i] = samples[i * stride];
        if (m_active)
          RunChain(m_chain, m_state, m_scratch.data(), 1, frames, ch, channels);
        // new sound in place
        if (newActive)
          RunChain(newChain, newState, samples, stride, frames, ch, channels);
        for (int i = 0; i < frames; ++i)
        {
          const float t = (i + 1) / static_cast<float>(frames);
          float& s = samples[i * stride];
          s = m_scratch[i] * (1.0f - t) + s * t;
        }
      }
    }

    m_chain = newChain;
    m_state.swap(newState);
    m_active = newActive;
    m_appliedVersion = version;
    m_sampleRate = sampleRate;
    m_channels = channels;
    if (crossfade || !m_active)
      return;
  }

  const int stride = planes == 1 ? channels : 1;
  for (int ch = 0; ch < channels; ++ch)
  {
    float* samples = planes == 1 ? data[0] + ch : data[ch];
    RunChain(m_chain, m_state, samples, stride, frames, ch, channels);
  }
}
