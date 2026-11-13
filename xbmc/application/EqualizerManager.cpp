/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "EqualizerManager.h"

#include "FileItem.h"
#include "ServiceBroker.h"
#include "URL.h"
#include "cores/AudioEngine/Utils/AEEqualizer.h"
#include "dialogs/GUIDialogKaiToast.h"
#include "guilib/LocalizeStrings.h"
#include "media/MediaType.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "settings/lib/Setting.h"
#include "settings/lib/SettingsManager.h"
#include "utils/FileUtils.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/XBMCTinyXML.h"
#include "utils/log.h"
#include "video/VideoInfoTag.h"

#include <mutex>
#include <set>

namespace
{
constexpr const char* RULES_FILE = "special://profile/equalizer.xml";
constexpr const char* KEY_SEASON = "season|";
constexpr const char* KEY_FOLDER = "folder|";

std::shared_ptr<CSettings> GetSettings()
{
  return CServiceBroker::GetSettingsComponent()->GetSettings();
}
} // namespace

CEqualizerManager& CEqualizerManager::GetInstance()
{
  static CEqualizerManager instance;
  return instance;
}

void CEqualizerManager::EnsureInitialized()
{
  // NOT under m_critical (see DecideLocked)
  std::call_once(m_initOnce, [this]() {
    {
      std::unique_lock<CCriticalSection> lock(m_critical);
      Load();
    }
    // Re-decide when the defaults change while something plays. The manager lives until the
    // process ends, so it's never unregistered.
    std::set<std::string> settingSet{CSettings::SETTING_AUDIOOUTPUT_EQ_ENABLED,
                                     CSettings::SETTING_AUDIOOUTPUT_EQ_MUSIC};
    GetSettings()->GetSettingsManager()->RegisterCallback(this, settingSet);
  });
}

void CEqualizerManager::Load()
{
  m_rules.clear();
  CXBMCTinyXML doc;
  if (!CFileUtils::Exists(RULES_FILE) || !doc.LoadFile(RULES_FILE))
    return;
  const TiXmlElement* root = doc.RootElement();
  if (!root)
    return;
  for (const TiXmlElement* e = root->FirstChildElement("scope"); e;
       e = e->NextSiblingElement("scope"))
  {
    const char* key = e->Attribute("key");
    const char* active = e->Attribute("active");
    if (!key || !active)
      continue;
    Rule rule;
    rule.active = StringUtils::EqualsNoCase(active, "true");
    const char* label = e->Attribute("label");
    rule.label = label ? label : "";
    m_rules[key] = rule;
  }
  CLog::Log(LOGDEBUG, "CEqualizerManager: {} remembered equalizer choices", m_rules.size());
}

void CEqualizerManager::Save() const
{
  // under m_critical
  CXBMCTinyXML doc;
  TiXmlElement root("equalizer");
  for (const auto& [key, rule] : m_rules)
  {
    TiXmlElement scope("scope");
    scope.SetAttribute("key", key.c_str());
    scope.SetAttribute("active", rule.active ? "true" : "false");
    scope.SetAttribute("label", rule.label.c_str());
    root.InsertEndChild(scope);
  }
  doc.InsertEndChild(root);
  if (!doc.SaveFile(RULES_FILE))
    CLog::Log(LOGERROR, "CEqualizerManager: unable to save {}", RULES_FILE);
}

void CEqualizerManager::ScopesFor(const CFileItem& item,
                                  bool& music,
                                  std::vector<std::string>& keys,
                                  std::string& label)
{
  keys.clear();
  label.clear();
  music = !item.IsVideo() && (item.IsAudio() || item.HasMusicInfoTag());
  if (music)
  {
    label = g_localizeStrings.Get(39727); // "music"
    return;
  }

  // library episode: the season
  if (item.HasVideoInfoTag())
  {
    const CVideoInfoTag* tag = item.GetVideoInfoTag();
    if (tag->m_type == MediaTypeEpisode && !tag->m_strShowTitle.empty() && tag->m_iSeason >= 0)
    {
      keys.push_back(KEY_SEASON + StringUtils::ToLower(tag->m_strShowTitle) + "|" +
                     std::to_string(tag->m_iSeason));
      label = StringUtils::Format(g_localizeStrings.Get(39724), tag->m_strShowTitle,
                                  tag->m_iSeason); // "Lower Decks season 2"
    }
  }

  // the folder (so File manager playback of the same files shares the choice)
  std::string path = item.GetDynPath();
  if (path.empty())
    path = item.GetPath();
  if (!path.empty() && !item.IsInternetStream() && !item.IsPlugin() && !item.IsPVR() &&
      !URIUtils::IsStack(path))
  {
    std::string folder = URIUtils::GetDirectory(path);
    if (!folder.empty())
    {
      keys.push_back(KEY_FOLDER + StringUtils::ToLower(CURL(folder).GetWithoutUserDetails()));
      if (label.empty())
      {
        std::string name = folder;
        URIUtils::RemoveSlashAtEnd(name);
        label = URIUtils::GetFileName(name);
      }
    }
  }
  if (label.empty())
    label = g_localizeStrings.Get(39728); // "this video"
}

bool CEqualizerManager::DecideLocked(bool videoDefault, bool musicOn) const
{
  if (m_music)
    return musicOn;
  for (const std::string& key : m_keys)
  {
    const auto it = m_rules.find(key);
    if (it != m_rules.end())
      return it->second.active;
  }
  return videoDefault;
}

void CEqualizerManager::Apply(bool active)
{
  if (CAEEqualizer::IsActive() != active)
    CLog::Log(LOGINFO, "CEqualizerManager: equalizer {} for {}", active ? "on" : "off", m_label);
  CAEEqualizer::SetActive(active);
}

void CEqualizerManager::OnPlaybackStarted(const CFileItem& item)
{
  EnsureInitialized();
  const bool videoDefault = GetSettings()->GetBool(CSettings::SETTING_AUDIOOUTPUT_EQ_ENABLED);
  const bool musicOn = GetSettings()->GetBool(CSettings::SETTING_AUDIOOUTPUT_EQ_MUSIC);
  bool music;
  std::vector<std::string> keys;
  std::string label;
  ScopesFor(item, music, keys, label);

  std::unique_lock<CCriticalSection> lock(m_critical);
  m_music = music;
  m_keys = std::move(keys);
  m_label = std::move(label);
  Apply(DecideLocked(videoDefault, musicOn));
}

bool CEqualizerManager::IsActive() const
{
  return CAEEqualizer::IsActive();
}

bool CEqualizerManager::IsPlayingMusic() const
{
  std::unique_lock<CCriticalSection> lock(m_critical);
  return m_music;
}

void CEqualizerManager::SetActiveForCurrent(bool active)
{
  std::string label;
  bool music;
  EnsureInitialized();
  {
    std::unique_lock<CCriticalSection> lock(m_critical);
    music = m_music;
    label = m_label;
    if (!music && !m_keys.empty())
    {
      for (const std::string& key : m_keys)
        m_rules[key] = Rule{active, m_label};
      Save();
    }
    Apply(active);
  }
  if (music)
    GetSettings()->SetBool(CSettings::SETTING_AUDIOOUTPUT_EQ_MUSIC, active); // remembered there

  // "Equalizer on for Lower Decks season 2"
  CGUIDialogKaiToast::QueueNotification(
      CGUIDialogKaiToast::Info, g_localizeStrings.Get(39700),
      StringUtils::Format(g_localizeStrings.Get(active ? 39725 : 39726), label), 3000, false);
}

void CEqualizerManager::OnSettingChanged(const std::shared_ptr<const CSetting>& setting)
{
  if (!setting)
    return;
  const bool videoDefault = GetSettings()->GetBool(CSettings::SETTING_AUDIOOUTPUT_EQ_ENABLED);
  const bool musicOn = GetSettings()->GetBool(CSettings::SETTING_AUDIOOUTPUT_EQ_MUSIC);
  std::unique_lock<CCriticalSection> lock(m_critical);
  if (m_label.empty())
    return; // nothing has played yet
  Apply(DecideLocked(videoDefault, musicOn));
}
