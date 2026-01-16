/*
 * net.c - Network layer using libcurl with mbedTLS
 *
 * Handles HTTPS communication with the Anthropic API.
 * Uses curl for HTTP and mbedTLS for SSL on the 3DS.
 * Includes error categorization and retry with exponential backoff.
 */

#include "net.h"
#include "app_config.h"
#include "debug_log.h"
#include "ui.h"

#include <3ds.h>
#include <curl/curl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOC_BUFFER_SIZE  0x100000
#define INITIAL_BUF_SIZE 4096

/* Retry configuration */
#define MAX_RETRIES_RATE_LIMIT  5
#define MAX_RETRIES_TRANSIENT   3
#define MAX_RETRIES_NETWORK     3
#define MAX_DELAY_MS            30000
#define BASE_DELAY_MS           1000

/* Error categories for retry logic */
typedef enum {
  ERR_CAT_NONE = 0,
  ERR_CAT_RATE_LIMIT,     /* 429 - retry with exponential backoff */
  ERR_CAT_TRANSIENT,      /* 500, 503 - retry with short delay */
  ERR_CAT_NETWORK,        /* Connection failures - retry */
  ERR_CAT_CONTEXT_LENGTH, /* Context too long - need to prune */
  ERR_CAT_AUTH,           /* 401, 403 - don't retry */
  ERR_CAT_INVALID,        /* 400 - don't retry */
  ERR_CAT_UNKNOWN
} ErrorCategory;

/* Categorize HTTP errors */
static ErrorCategory categorize_http_error(long http_code, const char *body) {
  if (http_code == 429) return ERR_CAT_RATE_LIMIT;
  if (http_code == 500 || http_code == 503) return ERR_CAT_TRANSIENT;
  if (http_code == 401 || http_code == 403) return ERR_CAT_AUTH;
  if (http_code == 400) {
    /* Check for context length error in body */
    if (body && strstr(body, "context_length")) {
      return ERR_CAT_CONTEXT_LENGTH;
    }
    return ERR_CAT_INVALID;
  }
  return ERR_CAT_UNKNOWN;
}

/* Categorize curl errors */
static ErrorCategory categorize_curl_error(CURLcode code) {
  switch (code) {
    case CURLE_COULDNT_CONNECT:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
      return ERR_CAT_NETWORK;
    default:
      return ERR_CAT_UNKNOWN;
  }
}

/* Get max retries for error category */
static int get_max_retries(ErrorCategory cat) {
  switch (cat) {
    case ERR_CAT_RATE_LIMIT:  return MAX_RETRIES_RATE_LIMIT;
    case ERR_CAT_TRANSIENT:   return MAX_RETRIES_TRANSIENT;
    case ERR_CAT_NETWORK:     return MAX_RETRIES_NETWORK;
    default:                  return 0;
  }
}

/* Calculate delay with exponential backoff */
static int calc_delay_ms(ErrorCategory cat, int attempt) {
  int delay;
  switch (cat) {
    case ERR_CAT_RATE_LIMIT:
      /* Exponential: 1s, 2s, 4s, 8s, 16s (capped at 30s) */
      delay = BASE_DELAY_MS * (1 << attempt);
      break;
    case ERR_CAT_TRANSIENT:
      /* Linear with jitter: 1-3s */
      delay = BASE_DELAY_MS + (rand() % 2000);
      break;
    case ERR_CAT_NETWORK:
      /* Gentle exponential: 2s, 3s, 4.5s */
      delay = 2000 * (1 + attempt / 2);
      break;
    default:
      delay = BASE_DELAY_MS;
  }
  return (delay > MAX_DELAY_MS) ? MAX_DELAY_MS : delay;
}

/* Sleep for specified milliseconds (3DS-compatible) */
static void sleep_ms(int ms) {
  svcSleepThread((s64)ms * 1000000LL);
}

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

  /* Grow buffer if needed */
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
  soc_buffer = (u32 *)memalign(0x1000, SOC_BUFFER_SIZE);
  if (!soc_buffer) {
    snprintf(error, error_size, "soc buffer alloc failed");
    return 0;
  }
  
  Result res = socInit(soc_buffer, SOC_BUFFER_SIZE);
  if (res != 0) {
    snprintf(error, error_size, "socInit failed: 0x%08lx", res);
    return 0;
  }

  CURLcode curl_res = curl_global_init(CURL_GLOBAL_DEFAULT);
  if (curl_res != CURLE_OK) {
    snprintf(error, error_size, "curl init failed: %s", curl_easy_strerror(curl_res));
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

/*
 * Internal function to make a single HTTP POST request.
 * Returns: 1 on success, 0 on error
 * Sets: http_code_out to the HTTP status code
 */
static int net_post_json_once(const char *url, const char *body,
                               char *response, size_t response_size,
                               char *error, size_t error_size,
                               long *http_code_out, CURLcode *curl_code_out) {
  CURL *curl = curl_easy_init();
  if (!curl) {
    snprintf(error, error_size, "curl_easy_init failed");
    return 0;
  }

  ResponseBuffer resp_buf = {
    .data = malloc(INITIAL_BUF_SIZE),
    .size = 0,
    .capacity = INITIAL_BUF_SIZE
  };
  if (!resp_buf.data) {
    snprintf(error, error_size, "malloc failed");
    curl_easy_cleanup(curl);
    return 0;
  }
  resp_buf.data[0] = '\0';

  /* Configure request */
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));

  /* Set headers */
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

  /* Execute request */
  CURLcode res = curl_easy_perform(curl);
  *curl_code_out = res;

  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  *http_code_out = http_code;
  curl_slist_free_all(headers);

  if (res != CURLE_OK) {
    snprintf(error, error_size, "curl error: %s", curl_easy_strerror(res));
    free(resp_buf.data);
    curl_easy_cleanup(curl);
    return 0;
  }

  /* Copy response to output buffer */
  size_t copy_size = resp_buf.size;
  if (copy_size >= response_size) {
    copy_size = response_size - 1;
  }
  memcpy(response, resp_buf.data, copy_size);
  response[copy_size] = '\0';

  free(resp_buf.data);
  curl_easy_cleanup(curl);

  return 1;
}

/*
 * Make HTTP POST request with automatic retry on recoverable errors.
 * Uses exponential backoff for rate limits and transient errors.
 */
int net_post_json(const char *url, const char *body,
                  char *response, size_t response_size,
                  char *error, size_t error_size) {
  int attempt = 0;
  ErrorCategory last_error = ERR_CAT_NONE;
  long http_code = 0;
  CURLcode curl_code = CURLE_OK;
  
  while (1) {
    /* Make the request */
    int ok = net_post_json_once(url, body, response, response_size,
                                 error, error_size, &http_code, &curl_code);
    
    /* Check for curl-level errors */
    if (!ok && curl_code != CURLE_OK) {
      last_error = categorize_curl_error(curl_code);
      int max_retries = get_max_retries(last_error);
      
      if (attempt < max_retries) {
        int delay = calc_delay_ms(last_error, attempt);
        debug_log("net: curl error, retrying in %dms (attempt %d/%d)", 
                  delay, attempt + 1, max_retries);
        
        char status_msg[64];
        snprintf(status_msg, sizeof(status_msg), "Network error, retrying (%d/%d)...", 
                 attempt + 1, max_retries);
        ui_set_status(status_msg);
        ui_render();
        
        sleep_ms(delay);
        attempt++;
        continue;
      }
      return 0;
    }
    
    /* Check HTTP status */
    if (http_code >= 200 && http_code < 300) {
      return 1;  /* Success! */
    }
    
    /* HTTP error - categorize and maybe retry */
    last_error = categorize_http_error(http_code, response);
    int max_retries = get_max_retries(last_error);
    
    if (attempt < max_retries) {
      int delay = calc_delay_ms(last_error, attempt);
      debug_log("net: HTTP %ld, retrying in %dms (attempt %d/%d)", 
                http_code, delay, attempt + 1, max_retries);
      
      char status_msg[64];
      if (last_error == ERR_CAT_RATE_LIMIT) {
        snprintf(status_msg, sizeof(status_msg), "Rate limited, waiting %ds...", delay / 1000);
      } else {
        snprintf(status_msg, sizeof(status_msg), "Server error, retrying (%d/%d)...", 
                 attempt + 1, max_retries);
      }
      ui_set_status(status_msg);
      ui_render();
      
      sleep_ms(delay);
      attempt++;
      continue;
    }
    
    /* No more retries - return error */
    if (last_error == ERR_CAT_CONTEXT_LENGTH) {
      snprintf(error, error_size, "Context too long - conversation needs pruning");
    } else if (last_error == ERR_CAT_AUTH) {
      snprintf(error, error_size, "Authentication failed - check API key");
    } else if (last_error == ERR_CAT_RATE_LIMIT) {
      snprintf(error, error_size, "Rate limit exceeded after %d retries", attempt);
    } else {
      snprintf(error, error_size, "HTTP %ld: %.100s", http_code, response);
    }
    return 0;
  }
}
