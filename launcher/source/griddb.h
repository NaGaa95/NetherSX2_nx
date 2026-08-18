#pragma once
#include <atomic>
#include <string>
#include <vector>

enum {
  GRIDDB_OK = 0,
  GRIDDB_NO_KEY,
  GRIDDB_NO_NET,
  GRIDDB_NOT_FOUND,
  GRIDDB_ERROR,
  GRIDDB_CANCELLED,
};

struct GridDbGameResult {
  long id = 0;
  std::string name;
};

struct GridDbArtwork {
  std::string url;
  std::string thumbnailUrl;
  int width = 0;
  int height = 0;
};

int griddb_search_games(const std::string &apiKey, const std::string &title,
                        std::vector<GridDbGameResult> &results,
                        const std::atomic_bool *cancel = nullptr);
int griddb_fetch_artworks(const std::string &apiKey, long gameId,
                          std::vector<GridDbArtwork> &artworks,
                          const std::atomic_bool *cancel = nullptr);
int griddb_download_image(const std::string &url, const std::string &outPath,
                          const std::atomic_bool *cancel = nullptr);

int griddb_fetch_cover(const std::string &apiKey, const std::string &title,
                       const std::string &outPath,
                       const std::atomic_bool *cancel = nullptr);

int griddb_fetch_icons(const std::string &key, const std::string &title,
                       const std::string &outDir, int maxCount,
                       const std::atomic_bool *cancel = nullptr);

bool griddb_global_init(void);
void griddb_global_exit(void);
