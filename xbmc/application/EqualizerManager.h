/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "settings/lib/ISettingCallback.h"
#include "threads/CriticalSection.h"

#include <map>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

class CFileItem;

/*!
 * \brief Decides whether the audio equalizer runs for what's playing.
 *
 * - Videos: switching the equalizer on or off in the playback audio settings is remembered for
 *   the TV show season (library episodes) and for the folder the file is in, so every episode of
 *   that season gets the same choice, however it's opened. Anything without a remembered choice
 *   uses Settings > System > Audio > "Use equalizer for videos by default".
 * - Music: one switch for all music ("Use equalizer for music"), changed in the same places.
 *
 * The band levels are shared by everything (Settings > System > Audio > Equalizer).
 * Remembered choices are kept in special://profile/equalizer.xml.
 */
class CEqualizerManager : public ISettingCallback
{
public:
  static CEqualizerManager& GetInstance();

  //! called by the player callback for every item that starts playing
  void OnPlaybackStarted(const CFileItem& item);

  bool IsActive() const;
  bool IsPlayingMusic() const;

  //! switch the equalizer for what's playing and remember the choice (see class description)
  void SetActiveForCurrent(bool active);

  void OnSettingChanged(const std::shared_ptr<const CSetting>& setting) override;

private:
  CEqualizerManager() = default;

  struct Rule
  {
    bool active = false;
    std::string label;
  };

  void EnsureInitialized();
  void Load();
  void Save() const;
  //! under m_critical; the defaults are read from the settings beforehand (never under the lock,
  //! as the settings call OnSettingChanged with their own lock held)
  bool DecideLocked(bool videoDefault, bool musicOn) const;
  void Apply(bool active);

  static void ScopesFor(const CFileItem& item,
                        bool& music,
                        std::vector<std::string>& keys,
                        std::string& label);

  mutable CCriticalSection m_critical;
  std::once_flag m_initOnce;
  std::map<std::string, Rule> m_rules;

  // what's playing
  bool m_music = false;
  std::vector<std::string> m_keys; //!< most specific first: season, then folder
  std::string m_label; //!< e.g. "Star Trek: Lower Decks season 2" or a folder name
};
