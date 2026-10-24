#include "wifi_link.h"

#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>

static const char kHostname[] = "shiny-pad";
static const char kPrefsNamespace[] = "shiny_pad";
static const uint32_t kPollIntervalMs = 10;

static WiFiServer server(kTcpPort);
static WiFiClient client;
static bool serverStarted = false;
static bool statusKnown = false;
static wl_status_t lastStatus;
static uint32_t lastPollMs = 0;

static const char *statusName(wl_status_t s) {
  switch (s) {
    case WL_CONNECTED: return "UP";
    case WL_NO_SSID_AVAIL: return "NO_SSID";
    case WL_CONNECT_FAILED: return "FAILED";
    case WL_CONNECTION_LOST: return "LOST";
    case WL_DISCONNECTED: return "DOWN";
    case WL_STOPPED:
    case WL_NO_SHIELD: return "OFF";
    default: return "CONNECTING";
  }
}

static void connect(const char *ssid, const char *pass) {
  WiFi.persistent(false);  // credentials live in our own namespace
  WiFi.setHostname(kHostname);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);    // modem sleep adds latency spikes
  WiFi.setAutoReconnect(true);
  WiFi.disconnect();
  WiFi.begin(ssid, pass);
}

void wifiBegin() {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, true);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  prefs.end();
  if (ssid.length() > 0) connect(ssid.c_str(), pass.c_str());
}

void wifiSave(const char *ssid, const char *pass) {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  connect(ssid, pass);
}

void wifiPrintStatus(Print &out) {
  out.printf("WIFI %s %s\n", statusName(WiFi.status()), WiFi.localIP().toString().c_str());
}

Stream *wifiPoll(bool *fresh) {
  *fresh = false;

  if (millis() - lastPollMs >= kPollIntervalMs) {
    lastPollMs = millis();

    wl_status_t s = WiFi.status();
    if (!statusKnown || s != lastStatus) {
      statusKnown = true;
      lastStatus = s;
      wifiPrintStatus(Serial);
      if (s == WL_CONNECTED && !serverStarted) {
        if (!MDNS.begin(kHostname)) Serial.print("ERR MDNS -\n");
        server.begin();
        server.setNoDelay(true);
        serverStarted = true;
      }
    }

    if (serverStarted) {
      WiFiClient incoming = server.accept();
      if (incoming) {
        if (client.connected()) {
          incoming.print("ERR BUSY tcp\n");
          incoming.stop();
        } else {
          client = incoming;
          client.setNoDelay(true);
          *fresh = true;
        }
      }
    }
  }

  return client.connected() ? &client : nullptr;
}
