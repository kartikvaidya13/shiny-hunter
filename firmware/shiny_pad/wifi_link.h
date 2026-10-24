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

// Reports state changes on Serial and accepts the TCP client. Returns the client, or nullptr.
// Sets *fresh when a new client has just connected.
Stream *wifiPoll(bool *fresh);

#endif  // SHINY_PAD_WIFI_LINK_H_
