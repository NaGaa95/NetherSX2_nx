/* libcurl replacement for NetherSX2's Java URLDownloader. Calls are made from
 * the core's HTTP workers, so synchronous transfers do not block emulation. */

#include "ra_http.h"

#include <curl/curl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef NETHERSX2_VERSION
#define NETHERSX2_VERSION "unknown"
#endif

#define RA_HTTP_MAX_BODY (16u * 1024u * 1024u)

static bool s_curl_ready;

typedef struct {
  uint8_t *data;
  size_t size;
  size_t capacity;
  bool failed;
} RaHttpBuffer;

static size_t ra_http_write(void *contents, size_t size, size_t count,
                            void *userdata) {
  RaHttpBuffer *buffer = (RaHttpBuffer *)userdata;
  if (!buffer || (count != 0 && size > SIZE_MAX / count)) return 0;
  const size_t incoming = size * count;
  if (incoming > RA_HTTP_MAX_BODY - buffer->size) {
    buffer->failed = true;
    return 0;
  }
  const size_t required = buffer->size + incoming;
  if (required > buffer->capacity) {
    size_t capacity = buffer->capacity ? buffer->capacity : 4096;
    while (capacity < required) {
      if (capacity >= RA_HTTP_MAX_BODY / 2) {
        capacity = RA_HTTP_MAX_BODY;
        break;
      }
      capacity *= 2;
    }
    uint8_t *data = (uint8_t *)realloc(buffer->data, capacity + 1);
    if (!data) {
      buffer->failed = true;
      return 0;
    }
    buffer->data = data;
    buffer->capacity = capacity;
  }
  if (incoming) memcpy(buffer->data + buffer->size, contents, incoming);
  buffer->size = required;
  if (buffer->data) buffer->data[buffer->size] = 0;
  return incoming;
}

bool ra_http_init(void) {
  if (s_curl_ready) return true;
  s_curl_ready = curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK;
  return s_curl_ready;
}

void ra_http_shutdown(void) {
  if (s_curl_ready) {
    curl_global_cleanup();
    s_curl_ready = false;
  }
}

void ra_http_response_clear(RaHttpResponse *response) {
  if (!response) return;
  free(response->content_type);
  free(response->data);
  memset(response, 0, sizeof(*response));
}

bool ra_http_request(const char *url, const char *user_agent,
                     const void *post_data, size_t post_size,
                     bool is_post, RaHttpResponse *response) {
  if (!response) return false;
  ra_http_response_clear(response);
  if (!s_curl_ready || !url || (is_post && post_size && !post_data))
    return false;

  // Upgrade only the retired RA image host; reject every other plaintext URL.
  static const char old_image_host[] = "http://i.retroachievements.org";
  static const char old_media_host[] = "http://media.retroachievements.org";
  static const char secure_media_host[] = "https://media.retroachievements.org";
  const char *suffix = NULL;
  if (!strncmp(url, old_image_host, sizeof(old_image_host) - 1))
    suffix = url + sizeof(old_image_host) - 1;
  else if (!strncmp(url, old_media_host, sizeof(old_media_host) - 1))
    suffix = url + sizeof(old_media_host) - 1;
  if (suffix && suffix[0] != '/' && suffix[0] != '\0') suffix = NULL;

  char *upgraded_url = NULL;
  const char *request_url = url;
  if (suffix) {
    const size_t size = sizeof(secure_media_host) + strlen(suffix);
    upgraded_url = (char *)malloc(size);
    if (!upgraded_url) return false;
    snprintf(upgraded_url, size, "%s%s", secure_media_host, suffix);
    request_url = upgraded_url;
  }
  if (strncmp(request_url, "https://", 8) != 0) {
    free(upgraded_url);
    return false;
  }

  CURL *curl = curl_easy_init();
  if (!curl) {
    free(upgraded_url);
    return false;
  }

  RaHttpBuffer buffer = {0};
  struct curl_slist *headers = NULL;
  if (is_post)
    headers = curl_slist_append(headers,
                                "Content-Type: application/x-www-form-urlencoded");

  const char *effective_user_agent =
      "NetherSX2-nx/" NETHERSX2_VERSION
      " (Nintendo Switch; NetherSX2 2.2n)";
  (void)user_agent; /* Always identify the actual frontend to RA servers. */

  curl_easy_setopt(curl, CURLOPT_URL, request_url);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, effective_user_agent);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ra_http_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
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
  if (is_post) {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)post_size);
  }

  const CURLcode result = curl_easy_perform(curl);
  long status_code = 0;
  char *content_type = NULL;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
  curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type);

  if (status_code > INT_MAX) status_code = INT_MAX;
  response->status_code = (int)status_code;
  response->content_type = strdup(content_type ? content_type : "");
  response->data = buffer.data;
  response->data_size = buffer.size;
  buffer.data = NULL;

  if (headers) curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  free(upgraded_url);
  free(buffer.data);

  const bool ok = result == CURLE_OK && !buffer.failed &&
                  response->content_type != NULL;
  return ok;
}
