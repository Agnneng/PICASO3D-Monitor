// SPDX-License-Identifier: GPL-3.0-only

#include "picaso_protocol.h"

#include <math.h>
#include <string.h>

namespace {

constexpr uint16_t kDiscoveryPort = 49149;
constexpr uint16_t kStatePort = 54321;
constexpr uint16_t kDiscoveryFrameType = 12;
constexpr uint16_t kStateCommand = 1;
constexpr size_t kMaxPacketSize = 512;

uint16_t readLe16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) |
         (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t readLe32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

int32_t readLeI32(const uint8_t* p) {
  return static_cast<int32_t>(readLe32(p));
}

float readLeFloat(const uint8_t* p) {
  const uint32_t bits = readLe32(p);
  float value = 0.0f;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

void appendUtf8(String& out, uint16_t cp) {
  if (cp == 0 || cp == 0xFFFF) return;
  if (cp <= 0x7F) {
    out += static_cast<char>(cp);
  } else if (cp <= 0x7FF) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

String decodeCp1251(const uint8_t* bytes, size_t length) {
  static const uint16_t highTable[64] = {
      0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021,
      0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
      0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
      0xFFFF, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
      0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7,
      0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
      0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7,
      0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457};

  String out;
  out.reserve(length * 2);
  for (size_t i = 0; i < length && bytes[i] != 0; ++i) {
    const uint8_t b = bytes[i];
    if (b < 0x20 || b == 0x7F) continue;
    if (b < 0x80) {
      out += static_cast<char>(b);
    } else if (b < 0xC0) {
      appendUtf8(out, highTable[b - 0x80]);
    } else if (b < 0xE0) {
      appendUtf8(out, static_cast<uint16_t>(0x0410 + b - 0xC0));
    } else {
      appendUtf8(out, static_cast<uint16_t>(0x0430 + b - 0xE0));
    }
  }
  out.trim();
  return out;
}

String decodeFixedText(const uint8_t* bytes, size_t length, bool utf8) {
  if (!utf8) return decodeCp1251(bytes, length);

  String out;
  out.reserve(length + 1);
  for (size_t i = 0; i < length && bytes[i] != 0; ++i) {
    const uint8_t b = bytes[i];
    if (b < 0x20 || b == 0x7F) continue;
    out += static_cast<char>(b);
  }
  out.trim();
  return out;
}

bool usesUtf8Names(uint8_t protocolMajor, uint8_t protocolMinor,
                   uint8_t firmwareMajor, uint16_t firmwareMinor) {
  if (protocolMajor == 2) return protocolMinor == 2;
  if (protocolMajor != 1 || protocolMinor != 2) return false;
  return firmwareMajor > 5 ||
         (firmwareMajor == 5 && firmwareMinor >= 9);
}

}  // namespace

bool PicasoClient::begin(uint16_t localPort) {
  localPort_ = localPort;
  return udp_.begin(localPort_);
}

void PicasoClient::setPrinterIP(const IPAddress& ip) { printerIp_ = ip; }

const PicasoDiscovery& PicasoClient::discovery() const { return discovery_; }

bool PicasoClient::readPacket(uint8_t* buffer, size_t capacity,
                              size_t& received, IPAddress& sender) {
  const int packetLength = udp_.parsePacket();
  if (packetLength <= 0) return false;

  sender = udp_.remoteIP();
  const size_t toRead = static_cast<size_t>(packetLength) < capacity
                            ? static_cast<size_t>(packetLength)
                            : capacity;
  const int count = udp_.read(buffer, toRead);
  received = count > 0 ? static_cast<size_t>(count) : 0;
  while (udp_.available() > 0) udp_.read();
  return received > 0;
}

bool PicasoClient::discover(uint32_t timeoutMs) {
  static const uint8_t request[] = {'P', 'I', 'C', 'A', 'S', 'O', '3', 'D'};
  udp_.beginPacket(IPAddress(255, 255, 255, 255), kDiscoveryPort);
  udp_.write(request, sizeof(request));
  if (udp_.endPacket() != 1) return false;

  const uint32_t deadline = millis() + timeoutMs;
  bool found = false;
  uint8_t packet[kMaxPacketSize];
  while (static_cast<int32_t>(deadline - millis()) > 0) {
    size_t length = 0;
    IPAddress sender;
    if (readPacket(packet, sizeof(packet), length, sender)) {
      if (sender == printerIp_ && parseDiscoveryDatagram(packet, length, sender)) {
        found = true;
      }
    } else {
      delay(5);
    }
  }
  return found;
}

bool PicasoClient::poll(PrinterSnapshot& result) {
  const uint8_t preferred = discovery_.valid ? discovery_.protocolMajor : 1;
  const uint8_t fallback = preferred == 1 ? 2 : 1;

  if (pollWithProtocol(preferred, 900, result)) return true;
  if (pollWithProtocol(fallback, 900, result)) return true;
  return false;
}

bool PicasoClient::pollWithProtocol(uint8_t protocolMajor,
                                    uint32_t timeoutMs,
                                    PrinterSnapshot& result) {
  uint8_t request[8] = {protocolMajor, 0, 1, 0, 0, 0, 8, 0};
  if (udp_.beginPacket(printerIp_, kStatePort) != 1) return false;
  udp_.write(request, sizeof(request));
  if (udp_.endPacket() != 1) return false;

  const uint32_t deadline = millis() + timeoutMs;
  uint8_t packet[kMaxPacketSize];
  while (static_cast<int32_t>(deadline - millis()) > 0) {
    size_t length = 0;
    IPAddress sender;
    if (readPacket(packet, sizeof(packet), length, sender)) {
      if (sender != printerIp_) continue;
      if (parseDiscoveryDatagram(packet, length, sender)) continue;
      if (parseStatePacket(packet, length, result)) return true;
    } else {
      delay(5);
    }
  }
  return false;
}

bool PicasoClient::parseDiscoveryDatagram(const uint8_t* data, size_t length,
                                          const IPAddress& sender) {
  bool parsed = false;
  size_t offset = 0;
  while (offset + 8 <= length) {
    const uint16_t frameType = readLe16(data + offset + 2);
    const uint16_t recordLength = readLe16(data + offset + 6);
    if (recordLength < 8 || offset + recordLength > length) break;

    if (frameType == kDiscoveryFrameType &&
        parseDiscoveryRecord(data, length, offset, recordLength, sender)) {
      parsed = true;
    }
    offset += recordLength;
  }
  return parsed;
}

bool PicasoClient::parseDiscoveryRecord(const uint8_t* data, size_t length,
                                         size_t offset, size_t recordLength,
                                         const IPAddress& sender) {
  if (offset + recordLength > length) return false;
  const uint8_t protocolMajor = data[offset];
  const uint8_t protocolMinor = data[offset + 1];
  if (protocolMajor < 1 || protocolMajor > 2 || protocolMinor > 2) return false;

  const uint8_t* body = data + offset + 8;
  const size_t bodyLength = recordLength - 8;
  if (bodyLength < 2) return false;

  size_t versionLength = 0;
  uint8_t firmwareMajor = 0;
  uint16_t firmwareMinor = 0;
  if (protocolMinor == 0) {
    versionLength = 2;
    if (bodyLength < 4) return false;
    firmwareMinor = body[2];
    firmwareMajor = body[3];
  } else if (protocolMinor == 1) {
    versionLength = 3;
    if (bodyLength < 5) return false;
    firmwareMinor = readLe16(body + 2);
    firmwareMajor = body[4];
  } else if (protocolMinor == 2) {
    versionLength = 4;
    if (bodyLength < 6) return false;
    firmwareMinor = body[4];
    firmwareMajor = body[5];
  } else {
    return false;
  }

  constexpr size_t kName = 20;
  constexpr size_t kSerial = 50;
  constexpr size_t kMac = 6;
  constexpr size_t kNozzleName = 10;
  constexpr size_t kProfileName = 40;
  const size_t fixedFields = kName + kSerial + kMac + kNozzleName + 1 +
                             kProfileName + kNozzleName + 1 + kProfileName;
  const size_t textOffset = 2 + versionLength;
  if (bodyLength < textOffset + fixedFields) return false;

  const bool utf8 = usesUtf8Names(protocolMajor, protocolMinor,
                                  firmwareMajor, firmwareMinor);
  size_t cursor = textOffset;
  PicasoDiscovery next;
  next.valid = true;
  next.ip = sender;
  next.protocolMajor = protocolMajor;
  next.protocolMinor = protocolMinor;
  next.firmwareMajor = firmwareMajor;
  next.firmwareMinor = firmwareMinor;
  next.namesUtf8 = utf8;
  next.printerName = decodeFixedText(body + cursor, kName, utf8);
  cursor += kName;
  cursor += kSerial;
  cursor += kMac;
  cursor += kNozzleName;
  next.nozzle1Size = body[cursor++];
  next.material1 = decodeFixedText(body + cursor, kProfileName, utf8);
  cursor += kProfileName;
  cursor += kNozzleName;
  next.nozzle2Size = body[cursor++];
  next.material2 = decodeFixedText(body + cursor, kProfileName, utf8);

  discovery_ = next;
  return true;
}

bool PicasoClient::parseStatePacket(const uint8_t* data, size_t length,
                                    PrinterSnapshot& result) const {
  if (length < 8 || readLe16(data + 2) != kStateCommand ||
      readLe16(data + 6) != length) {
    return false;
  }

  const uint16_t protocolWord = readLe16(data);
  size_t taskOffset = 0;
  size_t progressOffset = 0;
  size_t timeOffset = 0;
  size_t nozzle1Offset = 0;
  size_t nozzle2Offset = 0;
  size_t chamberOffset = 0;
  size_t bedOffset = 0;
  bool ready = false;

  if (protocolWord == 1 && length == 343) {
    taskOffset = 16;
    progressOffset = 275;
    timeOffset = 287;
    nozzle1Offset = 295;
    nozzle2Offset = 299;
    chamberOffset = 303;
    bedOffset = 307;
  } else if (protocolWord == 257 && length == 344) {
    taskOffset = 17;
    progressOffset = 276;
    timeOffset = 288;
    nozzle1Offset = 296;
    nozzle2Offset = 300;
    chamberOffset = 304;
    bedOffset = 308;
    ready = data[16] != 0;
  } else if ((protocolWord == 513 || protocolWord == 514) && length == 387) {
    taskOffset = 20;
    progressOffset = 279;
    timeOffset = 291;
    nozzle1Offset = 299;
    nozzle2Offset = 303;
    chamberOffset = 307;
    bedOffset = 311;
    ready = (readLe32(data + 16) & 1U) != 0;
  } else {
    return false;
  }

  if (bedOffset + sizeof(float) > length || progressOffset + sizeof(float) > length ||
      timeOffset + sizeof(int32_t) > length) {
    return false;
  }

  const bool utf8 = discovery_.valid ? discovery_.namesUtf8 : false;
  result.valid = true;
  result.state = readLe32(data + 8);
  result.status = readLe32(data + 12);
  result.ready = ready;
  result.progress = static_cast<int>(readLeFloat(data + progressOffset));
  result.remainingSeconds = readLeI32(data + timeOffset);
  result.nozzle1 = readLeFloat(data + nozzle1Offset);
  result.nozzle2 = readLeFloat(data + nozzle2Offset);
  result.chamber = readLeFloat(data + chamberOffset);
  result.bed = readLeFloat(data + bedOffset);
  result.jobName = decodeFixedText(data + taskOffset, 255, utf8);
  return true;
}
