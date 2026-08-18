#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct LauncherLanguage
{
  const char* code;
  const char* name;
};

// Launcher-owned catalogue. Emulator output, paths, game names, network errors
// and keyboard input are deliberately never passed through it.
class LauncherLocalization
{
public:
  bool SetLanguage(std::string_view preference);
  std::string_view Translate(std::string_view source) const;
  std::string_view Preference() const { return m_preference; }
  std::string_view ResolvedCode() const { return m_resolved; }
  const char* DisplayName() const;

  static const std::vector<LauncherLanguage>& Languages();
  static int FindLanguage(std::string_view code);

private:
  std::string m_preference{"system"};
  std::string m_resolved{"en"};
  std::unordered_map<std::string, std::string> m_catalog;
};

