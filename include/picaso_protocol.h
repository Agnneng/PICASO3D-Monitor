// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <Arduino.h>
#include <WiFiUdp.h>

struct PicasoDiscovery {
  bool valid = false;
  IPAddress ip;
  uint8_t protocolMajor = 1;
  uint8_t protocolMinor = 0;
  uint8_t firmwareMajor = 0;
  uint16_t firmwareMinor = 0;
  uint8_t nozzle1Size = 0;
  uint8_t nozzle2Size = 0;
  bool namesUtf8 = false;
  String printerName;
  String material1;
  String material2;
};

struct PrinterSnapshot {
  bool valid = false;
  bool connected = false;
  bool ready = false;
  uint32_t state = 0;
  uint32_t status = 0;
  int progress = -1;
  int32_t remainingSeconds = -1;
  float nozzle1 = NAN;
  float nozzle2 = NAN;
  float chamber = NAN;
  float bed = NAN;
  String jobName;
};

class PicasoClient {
 public:
  bool begin(uint16_t localPort = 54322);
  void setPrinterIP(const IPAddress& ip);
  const PicasoDiscovery& discovery() const;

  // Sends the same broadcast discovery marker used by Polygon X.
  bool discover(uint32_t timeoutMs = 900);

  // Reads printer state using the read-only status command.
  bool poll(PrinterSnapshot& result);

 private:
  WiFiUDP udp_;
  IPAddress printerIp_;
  uint16_t localPort_ = 54322;
  PicasoDiscovery discovery_;

  bool pollWithProtocol(uint8_t protocolMajor, uint32_t timeoutMs,
                        PrinterSnapshot& result);
  bool readPacket(uint8_t* buffer, size_t capacity, size_t& received,
                  IPAddress& sender);
  bool parseDiscoveryDatagram(const uint8_t* data, size_t length,
                              const IPAddress& sender);
  bool parseDiscoveryRecord(const uint8_t* data, size_t length,
                            size_t offset, size_t recordLength,
                            const IPAddress& sender);
  bool parseStatePacket(const uint8_t* data, size_t length,
                        PrinterSnapshot& result) const;
};
