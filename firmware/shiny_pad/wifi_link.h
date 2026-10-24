// Wi-Fi station plus one TCP client on the local network. Same line protocol as serial.

#ifndef SHINY_PAD_WIFI_LINK_H_
#define SHINY_PAD_WIFI_LINK_H_

#include <Arduino.h>

constexpr uint16_t kTcpPort = 4747;

// Connects with the saved credentials, if any.
void wifiBegin();

// Saves credentials to flash and reconnects. An empty password means an open network.
void wifiSave(const char *ssid, const char *pass);

// Prints "WIFI <state> <ip>".
void wifiPrintStatus(Print &out);

// Reports state changes on Serial and accepts new TCP clients; a new client replaces the old one,
// so a host that vanished without closing can't lock others out. Sets *fresh on a new client.
// May block briefly, so call only when nothing is due.
void wifiService(bool *fresh);

// The connected TCP client, or nullptr. Cheap enough to call every loop.
Stream *wifiClient();

#endif  // SHINY_PAD_WIFI_LINK_H_
