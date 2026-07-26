#pragma once

#include <cstddef>
#include <string>

enum class RetroAchievementsLoginResult {
  Success,
  NetworkUnavailable,
  HttpError,
  InvalidCredentials,
  InvalidResponse,
};

struct RetroAchievementsLoginResponse {
  std::string username;
  std::string token;
  std::string error;
};

RetroAchievementsLoginResult retroAchievementsLoginWithPassword(
    const std::string &username, const char *password,
    RetroAchievementsLoginResponse &response);

void retroAchievementsClearSecret(void *data, std::size_t size);
