// Not secret, but deployment-specific — update if the hub is replaced or
// the home router's channel changes (see architecture.md's open question
// on ESP-NOW/WiFi-STA channel coupling).
#pragma once

#include <cstdint>

// Hub's WiFi MAC — printed on the hub's serial log at boot as
// "ESP-NOW ready. Hub MAC: ...", also shown on the hub's web page's Debug
// ESP-NOW line. This is tied to the physical board's chip (the one with
// the external U.FL antenna), not to "whichever board is plugged into
// /dev/ttyUSB0" — Linux doesn't guarantee stable ttyUSB numbering across
// reboots/replugs. Identify the hub board physically (antenna = hub,
// relay/pump wiring = satellite) before flashing, not by port number.
// Confirmed 2026-08-06 by unplug test: this is the antenna board's MAC.
static const uint8_t HUB_MAC[6] = {0x68, 0x09, 0x47, 0x9E, 0x8A, 0x60};

// Must match the WiFi channel the home router put the hub's STA connection
// on (the satellite never joins the router's WiFi itself, so it can't
// learn this automatically yet — hardcoded until a discovery mechanism
// exists). Currently channel 1 (network "INALNET-FIBRA_0DF7").
static const uint8_t WIFI_CHANNEL = 1;
