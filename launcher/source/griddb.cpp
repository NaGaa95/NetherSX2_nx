#include "griddb.h"

#include <switch.h>
#include <curl/curl.h>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <sys/stat.h>
#include <unordered_set>
#include <unistd.h>
#include <vector>

static constexpr size_t MAX_BODY = 24u * 1024 * 1024;
static bool g_networkReady = false;
struct Buf { std::string data; };
static size_t write_cb(void *p, size_t sz, size_t n, void *u) {
  Buf *b = (Buf *)u;
  if (n != 0 && sz > std::numeric_limits<size_t>::max() / n) return 0;
  size_t add = sz * n;
  if (add > MAX_BODY - b->data.size()) return 0;
  b->data.append((char *)p, add);
  return add;
}

static int transfer_progress(void *userdata, curl_off_t, curl_off_t,
                             curl_off_t, curl_off_t) {
  const auto *cancel = static_cast<const std::atomic_bool *>(userdata);
  return cancel && cancel->load(std::memory_order_acquire) ? 1 : 0;
}

// CURL success does not imply HTTP success.
static bool http_get(const std::string &url, const std::string &bearer,
                     std::string &out, long *code,
                     const std::atomic_bool *cancel) {
  if (code) *code = 0;
  if (cancel && cancel->load(std::memory_order_acquire)) return false;
  if (!g_networkReady) return false;
  CURL *c = curl_easy_init();
  if (!c) return false;
  Buf b;
  struct curl_slist *hdr = nullptr;
  if (!bearer.empty()) {
    std::string h = "Authorization: Bearer " + bearer;
    hdr = curl_slist_append(hdr, h.c_str());
  }
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  if (hdr) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
#if LIBCURL_VERSION_NUM >= 0x075500
  curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
  curl_easy_setopt(c, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
  curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 25L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 8L);
  curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, transfer_progress);
  curl_easy_setopt(c, CURLOPT_XFERINFODATA, cancel);
  curl_easy_setopt(c, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)MAX_BODY);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "NetherSX2-Launcher/1.0");
  CURLcode rc = curl_easy_perform(c);
  long hc = 0;
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &hc);
  if (code) *code = hc;
  if (hdr) curl_slist_free_all(hdr);
  curl_easy_cleanup(c);
  out.swap(b.data);
  return rc == CURLE_OK;
}

static bool http_download(const std::string &url, const std::string &path,
                          const std::atomic_bool *cancel) {
  std::string data;
  long code = 0;
  if (!http_get(url, "", data, &code, cancel) ||
      (cancel && cancel->load(std::memory_order_acquire)) ||
      code < 200 || code >= 300 || data.size() < 64)
    return false;
  const unsigned char *d = (const unsigned char *)data.data();
  bool png = d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G';
  bool jpg = d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF;
  if (!png && !jpg) return false;
  const std::string temporary = path + ".tmp";
  const std::string backup = path + ".old";
  struct stat current{}, previous{};
  bool existed = stat(path.c_str(), &current) == 0;
  if (!existed && errno != ENOENT) return false;
  bool backupExists = stat(backup.c_str(), &previous) == 0;
  if (!backupExists && errno != ENOENT) return false;
  if (!existed && backupExists) {
    if (rename(backup.c_str(), path.c_str()) != 0) return false;
    existed = true;
  } else if (existed && backupExists && remove(backup.c_str()) != 0) {
    return false;
  }
  remove(temporary.c_str());
  FILE *f = fopen(temporary.c_str(), "wb");
  if (!f) return false;
  size_t w = fwrite(data.data(), 1, data.size(), f);
  bool ok = w == data.size() && fflush(f) == 0 && fsync(fileno(f)) == 0;
  if (fclose(f) != 0) ok = false;
  if (!ok) { remove(temporary.c_str()); return false; }

  if (existed) {
    if (rename(path.c_str(), backup.c_str()) != 0) { remove(temporary.c_str()); return false; }
  }
  if (rename(temporary.c_str(), path.c_str()) != 0) {
    if (existed) rename(backup.c_str(), path.c_str());
    remove(temporary.c_str());
    return false;
  }
  fsdevCommitDevice("sdmc");
  if (existed) {
    remove(backup.c_str());
    fsdevCommitDevice("sdmc");
  }
  return true;
}

static std::string url_encode(const std::string &s) {
  static const char *hex = "0123456789ABCDEF";
  std::string o;
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
  }
  return o;
}

static void append_utf8(std::string &output, unsigned codepoint) {
  if (codepoint <= 0x7f) output += (char)codepoint;
  else if (codepoint <= 0x7ff) {
    output += (char)(0xc0 | (codepoint >> 6));
    output += (char)(0x80 | (codepoint & 0x3f));
  } else if (codepoint <= 0xffff) {
    output += (char)(0xe0 | (codepoint >> 12));
    output += (char)(0x80 | ((codepoint >> 6) & 0x3f));
    output += (char)(0x80 | (codepoint & 0x3f));
  } else if (codepoint <= 0x10ffff) {
    output += (char)(0xf0 | (codepoint >> 18));
    output += (char)(0x80 | ((codepoint >> 12) & 0x3f));
    output += (char)(0x80 | ((codepoint >> 6) & 0x3f));
    output += (char)(0x80 | (codepoint & 0x3f));
  }
}

static int hex_digit(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

static bool parse_json_string(const std::string &json, size_t position, std::string &output) {
  if (position >= json.size() || json[position] != '"') return false;
  std::string parsed;
  for (size_t index = position + 1; index < json.size(); ++index) {
    unsigned char value = (unsigned char)json[index];
    if (value == '"') { output.swap(parsed); return true; }
    if (value != '\\') { parsed += (char)value; continue; }
    if (++index >= json.size()) return false;
    char escaped = json[index];
    if (escaped == '"' || escaped == '\\' || escaped == '/') parsed += escaped;
    else if (escaped == 'b') parsed += '\b';
    else if (escaped == 'f') parsed += '\f';
    else if (escaped == 'n') parsed += '\n';
    else if (escaped == 'r') parsed += '\r';
    else if (escaped == 't') parsed += '\t';
    else if (escaped == 'u') {
      if (index + 4 >= json.size()) return false;
      unsigned codepoint = 0;
      for (int digit = 0; digit < 4; ++digit) {
        int part = hex_digit(json[++index]);
        if (part < 0) return false;
        codepoint = (codepoint << 4) | (unsigned)part;
      }
      if (codepoint >= 0xd800 && codepoint <= 0xdbff && index + 6 < json.size() &&
          json[index + 1] == '\\' && json[index + 2] == 'u') {
        unsigned low = 0;
        bool valid = true;
        for (int digit = 0; digit < 4; ++digit) {
          int part = hex_digit(json[index + 3 + digit]);
          if (part < 0) { valid = false; break; }
          low = (low << 4) | (unsigned)part;
        }
        if (valid && low >= 0xdc00 && low <= 0xdfff) {
          codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
          index += 6;
        }
      }
      append_utf8(parsed, codepoint);
    } else return false;
  }
  return false;
}

static std::vector<std::string> json_data_objects(const std::string &json) {
  std::vector<std::string> objects;
  size_t data = json.find("\"data\"");
  if (data == std::string::npos) return objects;
  size_t array = json.find('[', data + 6);
  if (array == std::string::npos) return objects;
  bool quoted = false, escaped = false;
  int depth = 0;
  size_t start = std::string::npos;
  for (size_t index = array + 1; index < json.size(); ++index) {
    char value = json[index];
    if (quoted) {
      if (escaped) escaped = false;
      else if (value == '\\') escaped = true;
      else if (value == '"') quoted = false;
      continue;
    }
    if (value == '"') { quoted = true; continue; }
    if (value == '{') {
      if (depth++ == 0) start = index;
    } else if (value == '}' && depth > 0) {
      if (--depth == 0 && start != std::string::npos) {
        objects.emplace_back(json.substr(start, index - start + 1));
        start = std::string::npos;
      }
    } else if (value == ']' && depth == 0) break;
  }
  return objects;
}

static size_t json_field_value(const std::string &object, const char *field) {
  std::string key = std::string("\"") + field + "\"";
  size_t position = object.find(key);
  if (position == std::string::npos) return position;
  position = object.find(':', position + key.size());
  if (position == std::string::npos) return position;
  do { ++position; } while (position < object.size() && isspace((unsigned char)object[position]));
  return position;
}

static bool json_object_string(const std::string &object, const char *field, std::string &output) {
  size_t position = json_field_value(object, field);
  return position != std::string::npos && parse_json_string(object, position, output);
}

static bool json_object_number(const std::string &object, const char *field, long &output) {
  size_t position = json_field_value(object, field);
  if (position == std::string::npos) return false;
  char *end = nullptr;
  long value = strtol(object.c_str() + position, &end, 10);
  if (end == object.c_str() + position) return false;
  output = value;
  return true;
}

bool griddb_global_init(void) {
  if (g_networkReady) return true;
  if (R_FAILED(socketInitializeDefault())) return false;
  if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
    socketExit();
    return false;
  }
  g_networkReady = true;
  return true;
}
void griddb_global_exit(void) {
  if (!g_networkReady) return;
  curl_global_cleanup();
  socketExit();
  g_networkReady = false;
}

static int status_err(long code) {
  if (code == 401 || code == 403) return GRIDDB_NO_KEY;
  if (code < 200 || code >= 300) return GRIDDB_NOT_FOUND;
  return GRIDDB_OK;
}

int griddb_search_games(const std::string &key, const std::string &title,
                        std::vector<GridDbGameResult> &results,
                        const std::atomic_bool *cancel) {
  results.clear();
  if (key.empty()) return GRIDDB_NO_KEY;
  std::string response;
  long code = 0;
  std::string search = "https://www.steamgriddb.com/api/v2/search/autocomplete/" + url_encode(title);
  if (!http_get(search, key, response, &code, cancel))
    return cancel && cancel->load(std::memory_order_acquire) ? GRIDDB_CANCELLED : GRIDDB_NO_NET;
  int error = status_err(code);
  if (error) return error;
  std::unordered_set<long> seen;
  for (const auto &object : json_data_objects(response)) {
    long id = 0;
    std::string name;
    if (!json_object_number(object, "id", id) || id <= 0 ||
        !json_object_string(object, "name", name) || name.empty() || !seen.insert(id).second)
      continue;
    results.push_back({id, std::move(name)});
    if (results.size() >= 32) break;
  }
  return results.empty() ? GRIDDB_NOT_FOUND : GRIDDB_OK;
}

int griddb_fetch_artworks(const std::string &key, long gameId,
                          std::vector<GridDbArtwork> &artworks,
                          const std::atomic_bool *cancel) {
  artworks.clear();
  if (key.empty()) return GRIDDB_NO_KEY;
  if (gameId <= 0) return GRIDDB_NOT_FOUND;
  char endpoint[320];
  snprintf(endpoint, sizeof(endpoint),
           "https://www.steamgriddb.com/api/v2/grids/game/%ld?dimensions=600x900&types=static&mimes=image/png,image/jpeg",
           gameId);
  std::string response;
  long code = 0;
  if (!http_get(endpoint, key, response, &code, cancel))
    return cancel && cancel->load(std::memory_order_acquire) ? GRIDDB_CANCELLED : GRIDDB_NO_NET;
  int error = status_err(code);
  if (error) return error;
  std::unordered_set<std::string> seen;
  for (const auto &object : json_data_objects(response)) {
    GridDbArtwork artwork;
    long width = 0, height = 0;
    json_object_number(object, "width", width);
    json_object_number(object, "height", height);
    if (!json_object_string(object, "url", artwork.url) || artwork.url.empty() || !seen.insert(artwork.url).second)
      continue;
    if (!json_object_string(object, "thumb", artwork.thumbnailUrl) || artwork.thumbnailUrl.empty())
      artwork.thumbnailUrl = artwork.url;
    artwork.width = (int)width;
    artwork.height = (int)height;
    artworks.emplace_back(std::move(artwork));
    if (artworks.size() >= 100) break;
  }
  return artworks.empty() ? GRIDDB_NOT_FOUND : GRIDDB_OK;
}

int griddb_download_image(const std::string &url, const std::string &outPath,
                          const std::atomic_bool *cancel) {
  if (cancel && cancel->load(std::memory_order_acquire)) return GRIDDB_CANCELLED;
  const bool downloaded=!url.empty() && http_download(url, outPath, cancel);
  if(cancel && cancel->load(std::memory_order_acquire)) return GRIDDB_CANCELLED;
  return downloaded ? GRIDDB_OK : GRIDDB_ERROR;
}

int griddb_fetch_cover(const std::string &key, const std::string &title,
                       const std::string &outPath, const std::atomic_bool *cancel) {
  std::vector<GridDbGameResult> games;
  int result = griddb_search_games(key, title, games, cancel);
  if (result != GRIDDB_OK) return result;
  std::vector<GridDbArtwork> artworks;
  result = griddb_fetch_artworks(key, games.front().id, artworks, cancel);
  if (result != GRIDDB_OK) return result;
  return griddb_download_image(artworks.front().url, outPath, cancel);
}

int griddb_fetch_icons(const std::string &key, const std::string &title,
                       const std::string &outDir, int maxCount,
                       const std::atomic_bool *cancel) {
  if (key.empty()) return 0;
  std::vector<GridDbGameResult> games;
  if (griddb_search_games(key, title, games, cancel) != GRIDDB_OK) return 0;
  long gameId = games.front().id;
  const int cap = maxCount * 3;
  std::vector<std::string> urls;
  auto collect = [&](const char *api) {
    std::string r; long c = 0;
    if (!http_get(api, key, r, &c, cancel) || status_err(c)) return;
    size_t pos = 0; const std::string tag = "\"url\":\"";
    while ((int)urls.size() < cap) {
      size_t at = r.find(tag, pos);
      if (at == std::string::npos) break;
      size_t s = at + tag.size(), e = r.find('"', s);
      if (e == std::string::npos) break;
      std::string u = r.substr(s, e - s);
      pos = e + 1;
      std::string clean; clean.reserve(u.size());
      for (size_t i = 0; i < u.size(); i++) { if (u[i] == '\\' && i + 1 < u.size()) continue; clean += u[i]; }
      urls.push_back(clean);
    }
  };

  char api[256];
  snprintf(api, sizeof(api),
           "https://www.steamgriddb.com/api/v2/grids/game/%ld?dimensions=1024x1024,512x512&types=static&mimes=image/png,image/jpeg",
           gameId);
  collect(api);
  snprintf(api, sizeof(api),
           "https://www.steamgriddb.com/api/v2/icons/game/%ld?mimes=image/png&types=static", gameId);
  collect(api);

  int saved = 0;
  for (const auto &u : urls) {
    if (saved >= maxCount || (cancel && cancel->load(std::memory_order_acquire))) break;
    char out[256]; snprintf(out, sizeof(out), "%s/gicon_%d.png", outDir.c_str(), saved);
    if (http_download(u, out, cancel)) saved++;
  }
  return saved;
}
