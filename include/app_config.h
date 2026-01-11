#pragma once

// Set to 1 to route requests through local debug proxy, 0 for direct API access
#define NINTENCODE_USE_PROXY 0

#if NINTENCODE_USE_PROXY
  #define NINTENCODE_API_URL "http://192.168.0.148:8080/v1/messages"
#else
  #define NINTENCODE_API_URL "https://api.anthropic.com/v1/messages"
#endif

// API key must be set here before building
#define NINTENCODE_API_KEY "YOUR_API_KEY_HERE"

#define NINTENCODE_MODEL_ID "claude-sonnet-4-20250514"
#define NINTENCODE_MAX_TOKENS 8192

// Filesystem paths on SD card
#define NINTENCODE_BASE_PATH "sdmc:/nintencode"
#define NINTENCODE_WORKDIR "sdmc:/nintencode/workdir"
