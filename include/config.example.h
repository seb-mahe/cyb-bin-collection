// Copy this file to include/config.h and fill in your own values.
// config.h is ignored by git so your WiFi password stays private.
#pragma once

// --- WiFi ---
#define WIFI_SSID       "your-wifi-name"
#define WIFI_PASSWORD   "your-wifi-password"

// --- aklapi server (https://github.com/rusq/aklapi) ---
// IP of the machine running aklapi on your LAN, default port is 8080.
// Use plain http:// on the local network.
#define AKLAPI_BASE_URL "http://192.168.1.50:8080"

// Your street address, as you would type it on the Auckland Council website.
#define COLLECTION_ADDRESS "500 Queen Street"

// --- Time ---
// POSIX timezone string for Auckland (handles NZST / NZDT automatically).
#define TZ_INFO "NZST-12NZDT,M9.5.0,M4.1.0/3"

// Weekly refresh: every Monday at this local hour (0-23).
#define WEEKLY_REFRESH_HOUR 6
