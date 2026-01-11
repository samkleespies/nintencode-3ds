#include "net.h"

#include "app_config.h"
#include "ui.h"

#include <3ds.h>
#include <curl/curl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOC_BUFFERSIZE 0x100000

static u32 *soc_buffer = NULL;

typedef struct {
  char *data;
  size_t size;
  size_t capacity;
} ResponseBuffer;

static int progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                             curl_off_t ultotal, curl_off_t ulnow) {
  (void)clientp; (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
  ui_render_thinking();
  return 0;
}

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
  size_t realsize = size * nmemb;
  ResponseBuffer *buf = (ResponseBuffer *)userp;

  if (buf->size + realsize + 1 > buf->capacity) {
    size_t new_cap = buf->capacity * 2;
    if (new_cap < buf->size + realsize + 1) {
      new_cap = buf->size + realsize + 1;
    }
    char *new_data = realloc(buf->data, new_cap);
    if (!new_data) {
      return 0;
    }
    buf->data = new_data;
    buf->capacity = new_cap;
  }

  memcpy(buf->data + buf->size, contents, realsize);
  buf->size += realsize;
  buf->data[buf->size] = '\0';

  return realsize;
}

int net_init(char *error, size_t error_size) {
  soc_buffer = (u32 *)memalign(0x1000, SOC_BUFFERSIZE);
  if (!soc_buffer) {
    snprintf(error, error_size, "soc buffer alloc failed");
    return 0;
  }
  
  Result res = socInit(soc_buffer, SOC_BUFFERSIZE);
  if (res != 0) {
    snprintf(error, error_size, "socInit failed: 0x%08lx", res);
    return 0;
  }

  CURLcode curl_res = curl_global_init(CURL_GLOBAL_DEFAULT);
  if (curl_res != CURLE_OK) {
    snprintf(error, error_size, "curl_global_init failed: %s", curl_easy_strerror(curl_res));
    socExit();
    return 0;
  }

  return 1;
}

void net_exit(void) {
  curl_global_cleanup();
  socExit();
  if (soc_buffer) {
    free(soc_buffer);
    soc_buffer = NULL;
  }
}

int net_post_json(const char *url, const char *body, char *response,
                  size_t response_size, char *error, size_t error_size) {
  CURL *curl = curl_easy_init();
  if (!curl) {
    snprintf(error, error_size, "curl_easy_init failed");
    return 0;
  }

  ResponseBuffer resp_buf = {
    .data = malloc(4096),
    .size = 0,
    .capacity = 4096
  };
  if (!resp_buf.data) {
    snprintf(error, error_size, "malloc failed");
    curl_easy_cleanup(curl);
    return 0;
  }
  resp_buf.data[0] = '\0';

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));

  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  
  char api_key_header[256];
  snprintf(api_key_header, sizeof(api_key_header), "x-api-key: %s", NINTENCODE_API_KEY);
  headers = curl_slist_append(headers, api_key_header);
  
  headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");
  headers = curl_slist_append(headers, "User-Agent: nintencode-3ds/1.0");
  
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp_buf);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

  CURLcode res = curl_easy_perform(curl);

  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

  curl_slist_free_all(headers);

  if (res != CURLE_OK) {
    snprintf(error, error_size, "curl error: %s", curl_easy_strerror(res));
    free(resp_buf.data);
    curl_easy_cleanup(curl);
    return 0;
  }

  size_t copy_size = resp_buf.size;
  if (copy_size >= response_size) {
    copy_size = response_size - 1;
  }
  memcpy(response, resp_buf.data, copy_size);
  response[copy_size] = '\0';

  free(resp_buf.data);
  curl_easy_cleanup(curl);

  if (http_code < 200 || http_code >= 300) {
    snprintf(error, error_size, "HTTP %ld: %.100s", http_code, response);
    return 0;
  }

  return 1;
}
