/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <vector>

/*!
 * \brief 10-band graphic equalizer for the audio engine's float output.
 *
 * One peaking biquad per band (octave spaced, 31 Hz - 16 kHz, Q = sqrt(2)), run on every channel
 * of the mixed PCM output. Parameters may be changed from any thread with SetParams(); the audio
 * thread picks them up on its next Process() call and crossfades from the old filters to the new
 * ones over that buffer, so moving a slider during playback doesn't click.
 */
class CAEEqualizer
{
public:
  static constexpr int BANDS = 10;
  static constexpr float MAX_GAIN_DB = 12.0f;
  static const std::array<float, BANDS> FREQUENCIES;

  struct Params
  {
    bool enabled = false;
    std::array<float, BANDS> gainsDb{}; //!< per band, -12..+12 dB
    float preampDb = 0.0f; //!< extra gain for the whole signal
    bool preventClipping = true; //!< lower the level by the largest boost
  };

  CAEEqualizer() = default;

  //! thread safe
  void SetParams(const Params& params);
  Params GetParams() const;

  /*!
   * \brief Equalize float samples in place (audio thread only).
   * \param data planar: one pointer per channel; interleaved: data[0] only
   * \param planes number of planes (1 = interleaved)
   * \param channels channels in the stream
   * \param frames samples per channel
   * \param sampleRate in Hz
   */
  void Process(float** data, int planes, int channels, int frames, int sampleRate);

  //! forget filter history (e.g. after a flush), audio thread only
  void Reset();

  //! peak gain in dB of the band filters alone, at the given sample rate (for headroom)
  static float PeakResponseDb(const std::array<float, BANDS>& gainsDb, int sampleRate);

private:
  struct Biquad
  {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  };
  struct Chain
  {
    std::vector<Biquad> filters; //!< only bands with a non-zero gain that fit below Nyquist
    double gain = 1.0; //!< preamp and headroom, linear
    bool IsFlat() const { return filters.empty() && gain == 1.0; }
  };

  static Biquad Peaking(double freq, double gainDb, double q, double sampleRate);
  static Chain Design(const Params& params, int sampleRate);
  void RunChain(const Chain& chain,
                std::vector<double>& state,
                float* samples,
                int stride,
                int frames,
                int channel,
                int channels) const;

  mutable std::mutex m_lock;
  Params m_params; // guarded by m_lock
  std::atomic<unsigned int> m_version{1};

  // audio thread only
  unsigned int m_appliedVersion = 0;
  int m_sampleRate = 0;
  int m_channels = 0;
  bool m_active = false;
  Chain m_chain;
  std::vector<double> m_state; //!< 2 per filter per channel
  std::vector<float> m_scratch;
};
