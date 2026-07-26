#include "retroachievements.h"

#include <curl/curl.h>

#include <cctype>
#include <cstring>
#include <cstdlib>
#include <limits>

#ifndef NETHERSX2_VERSION
#define NETHERSX2_VERSION "unknown"
#endif

namespace {
constexpr std::size_t MaxLoginResponse = 1024u * 1024u;

struct ResponseBuffer {
  std::string data;
  bool overflow = false;
};

std::size_t writeResponse(void *contents, std::size_t size, std::size_t count,
                          void *userdata) {
  auto *buffer = static_cast<ResponseBuffer *>(userdata);
  if (!buffer || (count && size > std::numeric_limits<std::size_t>::max() / count))
    return 0;
  const std::size_t incoming = size * count;
  if (incoming > MaxLoginResponse - buffer->data.size()) {
    buffer->overflow = true;
    return 0;
  }
  buffer->data.append(static_cast<const char *>(contents), incoming);
  return incoming;
}

int hexDigit(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

void appendUtf8(std::string &output, unsigned codepoint) {
  if (codepoint <= 0x7f) output.push_back(static_cast<char>(codepoint));
  else if (codepoint <= 0x7ff) {
    output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else if (codepoint <= 0xffff) {
    output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  }
}

bool parseJsonString(const std::string &json, std::size_t position,
                     std::string &output) {
  if (position >= json.size() || json[position] != '"') return false;
  std::string parsed;
  for (std::size_t index = position + 1; index < json.size(); ++index) {
    const char value = json[index];
    if (value == '"') {
      output.swap(parsed);
      return true;
    }
    if (value != '\\') {
      parsed.push_back(value);
      continue;
    }
    if (++index >= json.size()) return false;
    const char escaped = json[index];
    if (escaped == '"' || escaped == '\\' || escaped == '/')
      parsed.push_back(escaped);
    else if (escaped == 'b') parsed.push_back('\b');
    else if (escaped == 'f') parsed.push_back('\f');
    else if (escaped == 'n') parsed.push_back('\n');
    else if (escaped == 'r') parsed.push_back('\r');
    else if (escaped == 't') parsed.push_back('\t');
    else if (escaped == 'u') {
      if (index + 4 >= json.size()) return false;
      unsigned codepoint = 0;
      for (int digit = 0; digit < 4; ++digit) {
        const int part = hexDigit(json[++index]);
        if (part < 0) return false;
        codepoint = (codepoint << 4) | static_cast<unsigned>(part);
      }
      appendUtf8(parsed, codepoint);
    } else {
      return false;
    }
  }
  return false;
}

std::size_t fieldValue(const std::string &json, const char *field) {
  const std::string key = std::string("\"") + field + "\"";
  std::size_t position = json.find(key);
  if (position == std::string::npos) return position;
  position = json.find(':', position + key.size());
  if (position == std::string::npos) return position;
  do {
    ++position;
  } while (position < json.size() &&
           std::isspace(static_cast<unsigned char>(json[position])));
  return position;
}

bool stringField(const std::string &json, const char *field,
                 std::string &output) {
  const std::size_t position = fieldValue(json, field);
  return position != std::string::npos &&
         parseJsonString(json, position, output);
}

bool boolField(const std::string &json, const char *field, bool &output) {
  const std::size_t position = fieldValue(json, field);
  if (position == std::string::npos) return false;
  if (json.compare(position, 4, "true") == 0) {
    output = true;
    return true;
  }
  if (json.compare(position, 5, "false") == 0) {
    output = false;
    return true;
  }
  return false;
}
} // namespace

void retroAchievementsClearSecret(void *data, std::size_t size) {
  volatile unsigned char *bytes = static_cast<volatile unsigned char *>(data);
  while (bytes && size--) *bytes++ = 0;
}

RetroAchievementsLoginResult retroAchievementsLoginWithPassword(
    const std::string &username, const char *password,
    RetroAchievementsLoginResponse &response) {
  response = {};
  if (username.empty() || !password || !password[0]) {
    response.error = "Enter both a username and password.";
    return RetroAchievementsLoginResult::InvalidCredentials;
  }

  CURL *curl = curl_easy_init();
  if (!curl) {
    response.error = "The network service is unavailable.";
    return RetroAchievementsLoginResult::NetworkUnavailable;
  }

  char *encoded_username = curl_easy_escape(curl, username.c_str(),
                                             static_cast<int>(username.size()));
  char *encoded_password = curl_easy_escape(curl, password, 0);
  if (!encoded_username || !encoded_password) {
    if (encoded_username) curl_free(encoded_username);
    if (encoded_password) curl_free(encoded_password);
    curl_easy_cleanup(curl);
    response.error = "Could not prepare the login request.";
    return RetroAchievementsLoginResult::InvalidResponse;
  }

  std::string post = "r=login2&u=";
  post += encoded_username;
  post += "&p=";
  post += encoded_password;
  retroAchievementsClearSecret(encoded_password, std::strlen(encoded_password));
  curl_free(encoded_username);
  curl_free(encoded_password);

  ResponseBuffer body;
  struct curl_slist *headers = nullptr;
  headers = curl_slist_append(headers,
                              "Content-Type: application/x-www-form-urlencoded");
  curl_easy_setopt(curl, CURLOPT_URL,
                   "https://retroachievements.org/dorequest.php");
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post.data());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                   static_cast<curl_off_t>(post.size()));
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeResponse);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_USERAGENT,
                   "NetherSX2-nx/" NETHERSX2_VERSION
                   " (Nintendo Switch; NetherSX2 2.2n)");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
#if LIBCURL_VERSION_NUM >= 0x075500
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif

  const CURLcode curl_result = curl_easy_perform(curl);
  long status_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
  if (headers) curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  retroAchievementsClearSecret(post.data(), post.size());

  if (curl_result != CURLE_OK || body.overflow) {
    response.error = "Could not contact RetroAchievements. Check your internet connection.";
    return RetroAchievementsLoginResult::NetworkUnavailable;
  }
  if (status_code < 200 || status_code >= 300) {
    response.error = "RetroAchievements returned HTTP " +
                     std::to_string(status_code) + ".";
    return RetroAchievementsLoginResult::HttpError;
  }

  bool success = false;
  if (!boolField(body.data, "Success", success)) {
    response.error = "RetroAchievements returned an invalid login response.";
    return RetroAchievementsLoginResult::InvalidResponse;
  }
  if (!success) {
    if (!stringField(body.data, "Error", response.error) || response.error.empty())
      response.error = "The username or password was rejected.";
    return RetroAchievementsLoginResult::InvalidCredentials;
  }
  if (!stringField(body.data, "User", response.username) ||
      !stringField(body.data, "Token", response.token) ||
      response.username.empty() || response.token.empty()) {
    response.error = "The login succeeded, but no account token was returned.";
    return RetroAchievementsLoginResult::InvalidResponse;
  }
  return RetroAchievementsLoginResult::Success;
}
