// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <Arduino.h>

#include "picaso_protocol.h"

class DisplayUI {
 public:
  void begin(uint16_t rotationDegrees);
  void showWifiConnecting(const String& ssid, uint8_t animationStep);
  void showSetupPortal(const String& apName, const String& apPassword);
  void showDashboard(const PicasoDiscovery& discovery,
                     const PrinterSnapshot& snapshot,
                     const String& printerIp,
                     const String& deviceIp,
                     bool wifiConnected,
                     const String& clockTime,
                     const String& clockDate);

 private:
  enum class ScreenMode : uint8_t { Blank, WifiConnecting, SetupPortal, Dashboard };

  ScreenMode screenMode_ = ScreenMode::Blank;
  uint8_t dashboardPage_ = 255;
  uint32_t dashboardStartedAt_ = 0;
  bool dashboardPrinterAvailable_ = false;
  bool emojiActive_ = false;
  uint32_t emojiStartedAt_ = 0;
  uint32_t nextEmojiAt_ = 0;
  uint8_t emojiOrder_[10] = {};
  uint8_t emojiCursor_ = 10;
  String lastSsid_;
  uint8_t lastWifiAnimationStep_ = 255;
  String lastApName_;
  String lastApPassword_;
  String lastHeader_;
  String lastStatus_;
  String lastTempsValues_;
  String lastNozzle1_;
  String lastNozzle2_;
  String lastBed_;
  String lastChamber_;
  String lastBigProgress_;
  String lastMaterial1_;
  String lastMaterial2_;
  String lastClockTime_;
  String lastClockDate_;
  String lastSetupAddress_;
  uint16_t lastStatusColor_ = 0;
  uint16_t lastProgressColor_ = 0;
  int lastProgressPercent_ = -1;
};
