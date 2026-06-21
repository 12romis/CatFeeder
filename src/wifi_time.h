#pragma once
#include <Arduino.h>

// Save Wi-Fi credentials to NVS (persist across reboots and deep sleep).
void wifiSetSsid(const char* ssid);
void wifiSetPass(const char* pass);

// Stored SSID, or empty string if not configured.
String wifiGetSsid();

// Connect to Wi-Fi, fetch NTP epoch, disconnect immediately.
// Calls scheduleSetTime() on success. Returns true if time was synced.
bool wifiSyncTime();
