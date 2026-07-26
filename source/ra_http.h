/* Native HTTPS transport for the Android URLDownloader contract used by the
 * RetroAchievements implementation embedded in libemucore.so. */

#ifndef __RA_HTTP_H__
#define __RA_HTTP_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  int status_code;
  char *content_type;
  uint8_t *data;
  size_t data_size;
} RaHttpResponse;

bool ra_http_init(void);
void ra_http_shutdown(void);

bool ra_http_request(const char *url, const char *user_agent,
                     const void *post_data, size_t post_size,
                     bool is_post, RaHttpResponse *response);
void ra_http_response_clear(RaHttpResponse *response);

#endif
