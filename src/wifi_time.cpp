#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <time.h>
#include "wifi_time.h"
#include "schedule.h"
#include "config.h"

void wifiSetSsid(const char* ssid) {
    Preferences prefs;
    prefs.begin("wifi", false);
    prefs.putString("ssid", ssid);
    prefs.end();
}

void wifiSetPass(const char* pass) {
    Preferences prefs;
    prefs.begin("wifi", false);
    prefs.putString("pass", pass);
    prefs.end();
}

String wifiGetSsid() {
    Preferences prefs;
    if (!prefs.begin("wifi", true)) return "";
    String s = prefs.getString("ssid", "");
    prefs.end();
    return s;
}

bool wifiSyncTime() {
    Preferences prefs;
    if (!prefs.begin("wifi", true)) {
        ESP_LOGW("wifi", "no credentials — use WIFI_SSID / WIFI_PASS");
        return false;
    }
    String ssid = prefs.getString("ssid", "");
    String pass = prefs.getString("pass", "");
    prefs.end();

    if (ssid.isEmpty()) {
        ESP_LOGW("wifi", "SSID not set — use WIFI_SSID <ssid>");
        return false;
    }

    ESP_LOGI("wifi", "connecting to \"%s\" ...", ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());

    uint32_t deadline = millis() + WIFI_CONNECT_MS;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) delay(100);

    if (WiFi.status() != WL_CONNECTED) {
        ESP_LOGW("wifi", "connection timeout — falling back to SET_TIME");
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        return false;
    }
    ESP_LOGI("wifi", "connected — requesting NTP from %s", NTP_SERVER1);

    configTime(0, 0, NTP_SERVER1, NTP_SERVER2);

    deadline = millis() + WIFI_NTP_MS;
    while (time(nullptr) < 1000000000UL && millis() < deadline) delay(100);

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    time_t now = time(nullptr);
    if (now < 1000000000UL) {
        ESP_LOGW("wifi", "NTP timeout — falling back to SET_TIME");
        return false;
    }

    scheduleSetTime((uint32_t)now);
    ESP_LOGI("wifi", "time synced: epoch=%lu", (unsigned long)now);
    return true;
}
