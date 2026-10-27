// SPDX-License-Identifier: GPL-3.0-only

#include "display_ui.h"

#include <math.h>
#include <stdlib.h>

#include <Adafruit_GC9A01A.h>
#include <SPI.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <esp_system.h>

#include "arial_fonts.h"
#include "screen2_icons.h"

namespace {

constexpr int8_t kLcdCs = PIN_LCD_CS;
constexpr int8_t kLcdDc = PIN_LCD_DC;
constexpr int8_t kLcdSck = PIN_LCD_SCLK;
constexpr int8_t kLcdMosi = PIN_LCD_MOSI;
constexpr int8_t kLcdBacklight = PIN_LCD_BL;

constexpr int16_t kScreenSize = 240;
constexpr int16_t kCenter = 120;
constexpr uint16_t kBackground = 0x2945;  // #2B2A29
constexpr uint16_t kBorder = kBackground;
constexpr uint16_t kWhite = 0xFFFF;
constexpr uint16_t kMuted = 0xB596;       // #B2B3B3
constexpr uint16_t kGray = 0x7BEF;
constexpr uint16_t kTrack = 0xDEDB;       // #D9DADA
constexpr uint16_t kLine = 0xC638;        // #C5C6C6
constexpr uint16_t kIpYellow = 0xFF60;    // #FFED00
constexpr uint16_t kCyan = 0x07FF;
constexpr uint16_t kLightBlue = 0x5E7F;
constexpr uint16_t kBlue = 0x24DE;
constexpr uint16_t kBeige = 0xDE35;
constexpr uint16_t kYellow = 0xFFE0;
constexpr uint16_t kGreen = 0x04C8;       // #009846
constexpr uint16_t kOrange = 0xFD20;
constexpr uint16_t kRed = 0xF800;
constexpr uint16_t kBlack = 0x0000;
constexpr uint16_t kLime = 0xB767;
constexpr uint16_t kBrandBlue = 0x051C;   // #00A0E3
constexpr uint16_t kSetupOrange = 0xEBE3; // #EF7F1A
constexpr uint8_t kEmojiCount = 10;
constexpr uint8_t kEmojiPixel = 6;
constexpr uint32_t kEmojiDurationMs = 5000UL;
constexpr uint32_t kEmojiMinIntervalMs = 180000UL;
constexpr uint32_t kEmojiMaxIntervalMs = 900000UL;
constexpr int16_t kRingInnerRadius = 104;
constexpr int16_t kRingOuterRadius = 116;

Adafruit_GC9A01A lcd(kLcdCs, kLcdDc, -1);
U8G2_FOR_ADAFRUIT_GFX fonts;

void printText(int16_t x, int16_t y, const uint8_t* font, uint16_t color,
               const String& text) {
  fonts.setFont(font);
  fonts.setForegroundColor(color);
  fonts.setCursor(x, y);
  fonts.print(text);
}

String fitText(const String& input, int16_t maximumWidth,
               const uint8_t* font = u8g2_font_6x13_t_cyrillic) {
  String out;
  out.reserve(input.length() + 4);
  fonts.setFont(font);
  for (size_t i = 0; i < input.length();) {
    const uint8_t lead = static_cast<uint8_t>(input[i]);
    size_t charLength = 1;
    if ((lead & 0xE0) == 0xC0) charLength = 2;
    else if ((lead & 0xF0) == 0xE0) charLength = 3;
    else if ((lead & 0xF8) == 0xF0) charLength = 4;
    if (i + charLength > input.length()) break;
    const String candidate = out + input.substring(i, i + charLength);
    if (fonts.getUTF8Width(candidate.c_str()) > maximumWidth) return out + "...";
    out = candidate;
    i += charLength;
  }
  return out;
}

const ArialRaster::Glyph* findArialGlyph(const ArialRaster::Font& font,
                                         uint16_t codepoint) {
  uint16_t low = 0;
  uint16_t high = font.glyphCount;
  while (low < high) {
    const uint16_t middle = static_cast<uint16_t>(low + (high - low) / 2);
    const uint16_t candidate = font.glyphs[middle].codepoint;
    if (candidate < codepoint) low = static_cast<uint16_t>(middle + 1);
    else high = middle;
  }
  if (low < font.glyphCount && font.glyphs[low].codepoint == codepoint) {
    return &font.glyphs[low];
  }
  return low < font.glyphCount ? findArialGlyph(font, '?') : nullptr;
}

uint16_t nextCodepoint(const char*& cursor) {
  const uint8_t first = static_cast<uint8_t>(*cursor++);
  if (first < 0x80) return first;
  if ((first & 0xE0) == 0xC0 && cursor[0] != '\0') {
    const uint8_t second = static_cast<uint8_t>(*cursor++);
    return static_cast<uint16_t>(((first & 0x1F) << 6) | (second & 0x3F));
  }
  if ((first & 0xF0) == 0xE0 && cursor[0] != '\0' && cursor[1] != '\0') {
    const uint8_t second = static_cast<uint8_t>(*cursor++);
    const uint8_t third = static_cast<uint8_t>(*cursor++);
    return static_cast<uint16_t>(((first & 0x0F) << 12) |
                                 ((second & 0x3F) << 6) | (third & 0x3F));
  }
  if ((first & 0xF8) == 0xF0 && cursor[0] != '\0' && cursor[1] != '\0' &&
      cursor[2] != '\0') {
    cursor += 3;
    return '?';
  }
  return '?';
}

uint16_t arialTextWidth(const ArialRaster::Font& font, const String& text) {
  uint32_t width = 0;
  const char* cursor = text.c_str();
  while (*cursor) {
    const uint16_t codepoint = nextCodepoint(cursor);
    const ArialRaster::Glyph* glyph = findArialGlyph(font, codepoint);
    if (glyph) width += glyph->xAdvance;
  }
  return static_cast<uint16_t>(width > 0xFFFF ? 0xFFFF : width);
}

String fitArialText(const String& text, uint16_t maximumWidth,
                    const ArialRaster::Font& font) {
  String out;
  out.reserve(text.length() + 4);
  uint16_t width = 0;
  size_t offset = 0;
  while (offset < text.length()) {
    const char* start = text.c_str() + offset;
    const char* cursor = start;
    const uint16_t codepoint = nextCodepoint(cursor);
    const uint16_t bytes = static_cast<uint16_t>(cursor - start);
    const ArialRaster::Glyph* glyph = findArialGlyph(font, codepoint);
    const uint16_t advance = glyph ? glyph->xAdvance : 0;
    if (width + advance > maximumWidth) {
      const uint16_t ellipsisWidth = arialTextWidth(font, "...");
      if (width + ellipsisWidth <= maximumWidth) out += "...";
      return out;
    }
    out += text.substring(offset, offset + bytes);
    offset += bytes;
    width += advance;
  }
  return out;
}

uint16_t blend565(uint16_t color, uint8_t alphaLevel) {
  const uint16_t backgroundRed = (kBackground >> 11) & 0x1F;
  const uint16_t backgroundGreen = (kBackground >> 5) & 0x3F;
  const uint16_t backgroundBlue = kBackground & 0x1F;
  const uint16_t foregroundRed = (color >> 11) & 0x1F;
  const uint16_t foregroundGreen = (color >> 5) & 0x3F;
  const uint16_t foregroundBlue = color & 0x1F;
  const uint16_t red = (foregroundRed * alphaLevel +
                        backgroundRed * (3 - alphaLevel) + 1) / 3;
  const uint16_t green = (foregroundGreen * alphaLevel +
                          backgroundGreen * (3 - alphaLevel) + 1) / 3;
  const uint16_t blue = (foregroundBlue * alphaLevel +
                         backgroundBlue * (3 - alphaLevel) + 1) / 3;
  return static_cast<uint16_t>((red << 11) | (green << 5) | blue);
}

void drawArialText(int16_t x, int16_t baseline,
                   const ArialRaster::Font& font, uint16_t color,
                   const String& text) {
  const char* cursor = text.c_str();
  while (*cursor) {
    const uint16_t codepoint = nextCodepoint(cursor);
    const ArialRaster::Glyph* glyph = findArialGlyph(font, codepoint);
    if (!glyph) continue;
    for (uint8_t row = 0; row < glyph->height; ++row) {
      for (uint8_t col = 0; col < glyph->width; ++col) {
        const uint16_t pixelIndex = static_cast<uint16_t>(row * glyph->width + col);
        const uint8_t packed = font.bitmap[glyph->bitmapOffset + pixelIndex / 4];
        const uint8_t shift = static_cast<uint8_t>(6 - 2 * (pixelIndex % 4));
        const uint8_t alpha = static_cast<uint8_t>((packed >> shift) & 0x03);
        if (!alpha) continue;
        const int16_t pixelX = static_cast<int16_t>(x + glyph->xOffset + col);
        const int16_t pixelY = static_cast<int16_t>(baseline + glyph->yOffset + row);
        if (pixelX >= 0 && pixelX < kScreenSize && pixelY >= 0 && pixelY < kScreenSize) {
          lcd.drawPixel(pixelX, pixelY, alpha == 3 ? color : blend565(color, alpha));
        }
      }
    }
    x = static_cast<int16_t>(x + glyph->xAdvance);
  }
}

void drawArialCentered(int16_t centerX, int16_t baseline,
                       const ArialRaster::Font& font, uint16_t color,
                       const String& text) {
  const int16_t x = static_cast<int16_t>(centerX - arialTextWidth(font, text) / 2);
  drawArialText(x, baseline, font, color, text);
}

const ArialRaster::Font& statusFont(const String& text) {
  if (arialTextWidth(ArialRaster::kArialBold25, text) <= 202) {
    return ArialRaster::kArialBold25;
  }
  if (arialTextWidth(ArialRaster::kArialBold20, text) <= 202) {
    return ArialRaster::kArialBold20;
  }
  if (arialTextWidth(ArialRaster::kArialBold16, text) <= 202) {
    return ArialRaster::kArialBold16;
  }
  return ArialRaster::kArialBold14;
}

int16_t centerTextX(const String& text, const uint8_t* font) {
  fonts.setFont(font);
  return static_cast<int16_t>((kScreenSize - fonts.getUTF8Width(text.c_str())) / 2);
}

String temperatureValueText(float value) {
  if (!isfinite(value) || value < 0.0f) return "--°";
  return String(static_cast<int>(lroundf(value))) + "°";
}

String remainingText(const PrinterSnapshot& snapshot) {
  if (!snapshot.connected || snapshot.remainingSeconds < 0) return "";
  const uint32_t totalMinutes = static_cast<uint32_t>(snapshot.remainingSeconds) / 60U;
  char buffer[20];
  snprintf(buffer, sizeof(buffer), "%02lu:%02lu",
           static_cast<unsigned long>(totalMinutes / 60U),
           static_cast<unsigned long>(totalMinutes % 60U));
  return String(buffer);
}

struct StatusStyle {
  const char* text;
  uint16_t color;
  bool printing;
};

StatusStyle getStatusStyle(const PrinterSnapshot& snapshot, bool wifiConnected) {
  if (!wifiConnected) return {"НЕТ WI-FI", kBlue, false};
  if (!snapshot.connected || !snapshot.valid) return {"НЕТ СВЯЗИ", kLightBlue, false};
  const uint32_t state = snapshot.state;
  const uint32_t status = snapshot.status;
  if (state == 4 || status == 5) return {"СЕРВИС", kYellow, false};
  if (status == 7) return {"ПЕЧАТЬ ЗАВЕРШЕНА", kGreen, false};
  if (status == 1 || status == 2 || state == 9) return {"ОШИБКА ПЕЧАТИ", kRed, false};
  if (state == 2 || state == 6 || state == 7 || status == 8) {
    return {"ПЕЧАТЬ ПРИОСТАНОВЛЕНА", kYellow, false};
  }
  if (state == 1 || status == 6) return {"ПЕЧАТЬ", kBeige, true};
  if (state == 5 || state == 8) return {"ПОДГОТОВКА", kBeige, false};
  if (snapshot.ready || status == 0x40000000U || status == 0x40000004U) {
    return {"ГОТОВ К ПЕЧАТИ", kWhite, false};
  }
  return {"ОЖИДАНИЕ ЗАДАНИЯ", kMuted, false};
}

void drawNozzleIcon(int16_t x, int16_t y, uint8_t number) {
  if (number == 1) {
    lcd.drawRGBBitmap(x, y, Screen2Icons::kNozzle1Pixels,
                      Screen2Icons::kNozzle1Mask,
                      Screen2Icons::kNozzle1Width,
                      Screen2Icons::kNozzle1Height);
  } else {
    lcd.drawRGBBitmap(x, y, Screen2Icons::kNozzle2Pixels,
                      Screen2Icons::kNozzle2Mask,
                      Screen2Icons::kNozzle2Width,
                      Screen2Icons::kNozzle2Height);
  }
}

void drawBedIcon(int16_t x, int16_t y) {
  lcd.drawRGBBitmap(x, y, Screen2Icons::kBedPixels,
                    Screen2Icons::kBedMask,
                    Screen2Icons::kBedWidth,
                    Screen2Icons::kBedHeight);
}

void drawChamberIcon(int16_t x, int16_t y) {
  lcd.drawRGBBitmap(x, y, Screen2Icons::kChamberPixels,
                    Screen2Icons::kChamberMask,
                    Screen2Icons::kChamberWidth,
                    Screen2Icons::kChamberHeight);
}

void drawWeatherCloud(int16_t x, int16_t y, uint16_t color) {
  const int16_t points[][2] = {
      {5, 40},  {53, 40}, {61, 40}, {67, 35}, {68, 28}, {68, 21},
      {62, 16}, {54, 16}, {51, 5},  {41, 0},  {31, 2},  {23, 9},
      {21, 17}, {12, 17}, {6, 22},  {1, 29},  {1, 35},  {5, 40}};
  for (uint8_t i = 1; i < sizeof(points) / sizeof(points[0]); ++i) {
    lcd.drawLine(x + points[i - 1][0], y + points[i - 1][1],
                 x + points[i][0], y + points[i][1], color);
    if (i > 1 && i < 17) {
      lcd.drawLine(x + points[i - 1][0], y + points[i - 1][1] + 1,
                   x + points[i][0], y + points[i][1] + 1, color);
    }
  }
}

uint16_t weatherTemperatureColor(int16_t temperatureC) {
  if (temperatureC > 15) return kIpYellow;
  if (temperatureC >= 5) return kLine;
  return kBrandBlue;
}

void drawWeatherSun(int16_t centerX, int16_t centerY, uint16_t color) {
  constexpr int16_t kInnerRadius = 12;
  constexpr int16_t kOuterRadius = 19;
  for (int16_t angle = 0; angle < 360; angle += 45) {
    const float radians = angle * DEG_TO_RAD;
    const int16_t x1 = centerX + static_cast<int16_t>(lroundf(cosf(radians) * kInnerRadius));
    const int16_t y1 = centerY + static_cast<int16_t>(lroundf(sinf(radians) * kInnerRadius));
    const int16_t x2 = centerX + static_cast<int16_t>(lroundf(cosf(radians) * kOuterRadius));
    const int16_t y2 = centerY + static_cast<int16_t>(lroundf(sinf(radians) * kOuterRadius));
    lcd.drawLine(x1, y1, x2, y2, color);
    lcd.drawLine(x1 + 1, y1, x2 + 1, y2, color);
  }
  lcd.fillCircle(centerX, centerY, 9, color);
}

void drawWeatherRain(int16_t x, int16_t y, uint16_t color) {
  drawWeatherCloud(x, y, color);
  lcd.drawLine(x + 18, y + 39, x + 14, y + 47, color);
  lcd.drawLine(x + 37, y + 39, x + 33, y + 47, color);
  lcd.drawLine(x + 56, y + 39, x + 52, y + 47, color);
}

void drawWeatherSnowflake(int16_t centerX, int16_t centerY, uint16_t color) {
  constexpr float kDirections[6][2] = {
      {1.0f, 0.0f}, {-1.0f, 0.0f}, {0.5f, 0.866f},
      {-0.5f, -0.866f}, {0.5f, -0.866f}, {-0.5f, 0.866f}};
  for (uint8_t i = 0; i < 6; ++i) {
    const float dx = kDirections[i][0];
    const float dy = kDirections[i][1];
    const int16_t tipX = centerX + static_cast<int16_t>(lroundf(dx * 19.0f));
    const int16_t tipY = centerY + static_cast<int16_t>(lroundf(dy * 19.0f));
    lcd.drawLine(centerX, centerY, tipX, tipY, color);

    const int16_t branchX = centerX + static_cast<int16_t>(lroundf(dx * 11.0f));
    const int16_t branchY = centerY + static_cast<int16_t>(lroundf(dy * 11.0f));
    for (int8_t side = -1; side <= 1; side += 2) {
      const int16_t endX = branchX + static_cast<int16_t>(lroundf(-dx * 4.0f - side * dy * 4.0f));
      const int16_t endY = branchY + static_cast<int16_t>(lroundf(-dy * 4.0f + side * dx * 4.0f));
      lcd.drawLine(branchX, branchY, endX, endY, color);
    }
  }
}

void drawWeatherLightning(int16_t centerX, int16_t centerY, uint16_t color) {
  const int16_t points[][2] = {
      {static_cast<int16_t>(centerX + 5), static_cast<int16_t>(centerY - 20)},
      {static_cast<int16_t>(centerX - 8), static_cast<int16_t>(centerY + 1)},
      {static_cast<int16_t>(centerX + 1), static_cast<int16_t>(centerY + 1)},
      {static_cast<int16_t>(centerX - 5), static_cast<int16_t>(centerY + 20)},
      {static_cast<int16_t>(centerX + 10), static_cast<int16_t>(centerY - 4)},
      {static_cast<int16_t>(centerX + 2), static_cast<int16_t>(centerY - 4)},
      {static_cast<int16_t>(centerX + 5), static_cast<int16_t>(centerY - 20)}};
  for (uint8_t i = 1; i < sizeof(points) / sizeof(points[0]); ++i) {
    lcd.drawLine(points[i - 1][0], points[i - 1][1], points[i][0], points[i][1], color);
    lcd.drawLine(points[i - 1][0] + 1, points[i - 1][1],
                 points[i][0] + 1, points[i][1], color);
  }
}

void drawWeatherIcon(int16_t x, int16_t y, WeatherIcon icon) {
  const int16_t centerX = x + 34;
  const int16_t centerY = y + 21;
  switch (icon) {
    case WeatherIcon::Sun:
      drawWeatherSun(centerX, centerY, kIpYellow);
      break;
    case WeatherIcon::PartlyCloudy:
      drawWeatherSun(x + 25, y + 13, kIpYellow);
      drawWeatherCloud(x, y + 5, kLine);
      break;
    case WeatherIcon::Rain:
      drawWeatherRain(x, y, kBrandBlue);
      break;
    case WeatherIcon::Snowflake:
      drawWeatherSnowflake(centerX, centerY, kBrandBlue);
      break;
    case WeatherIcon::Lightning:
      drawWeatherCloud(x, y, kLine);
      drawWeatherLightning(centerX, centerY + 4, kIpYellow);
      break;
    case WeatherIcon::Cloud:
    default:
      drawWeatherCloud(x, y, kLine);
      break;
  }
}

void drawProgressRing(int progress, bool printing) {
  const bool showProgress = printing && progress > 0;
  const float sweepDegrees =
      360.0f * constrain(progress, 0, 100) / 100.0f;
  const int32_t innerRadiusSquared = kRingInnerRadius * kRingInnerRadius;
  const int32_t outerRadiusSquared = kRingOuterRadius * kRingOuterRadius;
  const int16_t top = kCenter - kRingOuterRadius;
  const int16_t bottom = kCenter + kRingOuterRadius;

  // Paint each ring pixel exactly once as either track or progress. Drawing
  // concentric circle outlines and then a separately rasterized arc leaves
  // gray pixels underneath the green edge, which makes the fill look washed
  // out on this low-resolution display.
  for (int16_t y = top; y <= bottom; ++y) {
    const int16_t dy = y - kCenter;
    const int16_t outerHalfWidth = static_cast<int16_t>(
        sqrtf(static_cast<float>(outerRadiusSquared - dy * dy)));
    const int16_t left = kCenter - outerHalfWidth;
    const int16_t right = kCenter + outerHalfWidth;
    bool runActive = false;
    int16_t runStart = left;
    int16_t previousX = left;
    uint16_t runColor = kTrack;

    for (int16_t x = left; x <= right + 1; ++x) {
      bool inRing = false;
      uint16_t pixelColor = kTrack;
      if (x <= right) {
        const int16_t dx = x - kCenter;
        const int32_t radiusSquared = dx * dx + dy * dy;
        inRing = radiusSquared >= innerRadiusSquared &&
                 radiusSquared <= outerRadiusSquared;
        if (inRing && showProgress) {
          float angle = atan2f(static_cast<float>(dy),
                               static_cast<float>(dx)) * RAD_TO_DEG + 90.0f;
          if (angle < 0.0f) angle += 360.0f;
          if (angle <= sweepDegrees) pixelColor = kGreen;
        }
      }

      if (!inRing || !runActive || pixelColor != runColor ||
          x != previousX + 1) {
        if (runActive) {
          lcd.drawFastHLine(runStart, y, previousX - runStart + 1, runColor);
          runActive = false;
        }
        if (inRing) {
          runActive = true;
          runStart = x;
          runColor = pixelColor;
        }
      }
      previousX = x;
    }
  }
}

void drawStaticRing(uint16_t color) {
  for (int16_t radius = kRingInnerRadius; radius <= kRingOuterRadius; ++radius) {
    lcd.drawCircle(kCenter, kCenter, radius, color);
  }
}

void drawWifiWave(int16_t centerX, int16_t topY, int16_t halfWidth,
                  int16_t sagitta, int16_t thickness, uint16_t color) {
  const int32_t widthSquared = halfWidth * halfWidth;
  for (int16_t offset = -halfWidth; offset <= halfWidth; ++offset) {
    const int16_t y = static_cast<int16_t>(
        topY + (static_cast<int32_t>(offset) * offset * sagitta) / widthSquared);
    lcd.fillRect(centerX + offset, y, 1, thickness, color);
  }
}

void drawWifiSymbol(uint8_t animationStep) {
  // The SVG uses three separate signal bands and a dot. The bands build up,
  // then fade back in sequence so the connection animation uses the icon itself.
  lcd.fillRect(76, 166, 88, 43, kBackground);
  if (animationStep == 1 || animationStep == 2 || animationStep == 3 ||
      animationStep == 4 || animationStep == 5) {
    drawWifiWave(kCenter, 195, 8, 3, 4, kGreen);
  }
  if (animationStep == 2 || animationStep == 3 || animationStep == 4) {
    drawWifiWave(kCenter, 183, 19, 6, 5, kGreen);
  }
  if (animationStep == 3) {
    drawWifiWave(kCenter, 170, 32, 10, 5, kGreen);
  }
  lcd.fillCircle(kCenter, 201, 4, kGreen);
}

uint16_t randomEmojiColor() {
  const uint16_t colors[] = {kWhite, kGray, kLightBlue, kLime};
  return colors[random(0L, 4L)];
}

int16_t emojiX(int16_t x) { return static_cast<int16_t>(x * 3 / 4); }
int16_t emojiY(int16_t y) { return static_cast<int16_t>(55 + y * 3 / 4); }

void drawPixelPattern(int16_t x, int16_t y, const uint16_t* rows,
                      uint8_t height, uint8_t width, uint8_t pixelSize,
                      uint16_t color) {
  for (uint8_t row = 0; row < height; ++row) {
    for (uint8_t col = 0; col < width; ++col) {
      const uint16_t mask = static_cast<uint16_t>(1U << (width - col - 1));
      if (rows[row] & mask) {
        lcd.fillRect(x + col * pixelSize, y + row * pixelSize, pixelSize,
                     pixelSize, color);
      }
    }
  }
}

void drawPixelLine(int16_t x, int16_t y, int16_t x0, int16_t y0, int16_t x1,
                   int16_t y1, uint8_t pixelSize, uint16_t color) {
  const int16_t dx = abs(x1 - x0);
  const int16_t sx = x0 < x1 ? 1 : -1;
  const int16_t dy = -abs(y1 - y0);
  const int16_t sy = y0 < y1 ? 1 : -1;
  int16_t error = dx + dy;
  while (true) {
    lcd.fillRect(x + x0 * pixelSize, y + y0 * pixelSize, pixelSize, pixelSize, color);
    if (x0 == x1 && y0 == y1) break;
    const int16_t twiceError = 2 * error;
    if (twiceError >= dy) { error += dy; x0 += sx; }
    if (twiceError <= dx) { error += dx; y0 += sy; }
  }
}

void drawPixelPath(int16_t x, int16_t y, const int8_t (*points)[2],
                   uint8_t pointCount, uint8_t pixelSize, uint16_t color) {
  for (uint8_t i = 0; i < pointCount; ++i) {
    lcd.fillRect(x + points[i][0] * pixelSize,
                 y + points[i][1] * pixelSize, pixelSize, pixelSize, color);
  }
}

void drawChevronEye(int16_t x, int16_t y, uint16_t color) {
  static const uint16_t rows[] = {0x020, 0x070, 0x0D8, 0x18C, 0x306, 0x603, 0x401};
  drawPixelPattern(x, y, rows, 7, 11, kEmojiPixel, color);
}

void drawRoundEye(int16_t x, int16_t y, uint16_t color) {
  static const uint16_t rows[] = {0x078, 0x084, 0x102, 0x201, 0x201,
                                  0x201, 0x102, 0x084, 0x078};
  drawPixelPattern(x, y, rows, 9, 10, kEmojiPixel, color);
}

void drawClosedEye(int16_t x, int16_t y, uint16_t color) {
  static const uint16_t rows[] = {0x201, 0x102, 0x084, 0x078};
  drawPixelPattern(x, y, rows, 4, 10, kEmojiPixel, color);
}

void drawEmojiSpool(uint16_t bodyColor, uint16_t numberColor) {
  static const uint16_t rows[] = {0x038, 0x038, 0x1FF, 0x101, 0x101,
                                  0x101, 0x1FF, 0x0FE, 0x07C, 0x038};
  static const uint16_t digitOne[] = {0x2, 0x6, 0x2, 0x2, 0x7};
  drawPixelPattern(emojiX(32), emojiY(38), rows, 10, 9, kEmojiPixel, bodyColor);
  drawPixelPattern(emojiX(60), emojiY(74), digitOne, 5, 3, 3, numberColor);
}

void drawEmojiScreen(uint8_t index) {
  lcd.fillScreen(kBlack);
  const uint16_t colorA = randomEmojiColor();
  const uint16_t colorB = randomEmojiColor();
  const uint16_t colorC = randomEmojiColor();
  const uint16_t colorD = randomEmojiColor();
  static const uint16_t squareRows[] = {0x3FF, 0x201, 0x201, 0x201, 0x201,
                                        0x201, 0x201, 0x201, 0x201, 0x3FF};
  static const uint16_t smallSquareRows[] = {0xF, 0x9, 0x9, 0xF};
  static const uint16_t centerPixel[] = {0x1};
  static const uint16_t arrowRows[] = {0x08, 0x1C, 0x2A, 0x08, 0x08, 0x08,
                                       0x08, 0x08, 0x08, 0x2A, 0x1C, 0x08};
  static const uint16_t extruderRows[] = {0x3C, 0x3C, 0xFF, 0x81, 0x81,
                                          0xFF, 0x3C, 0x18, 0x18};
  static const uint16_t xRows[] = {0x101, 0x082, 0x044, 0x028, 0x010,
                                   0x028, 0x044, 0x082, 0x101};
  static const int8_t pulsePath[][2] = {{0, 2}, {1, 2}, {2, 2}, {3, 1},
                                        {4, 0}, {5, 2}, {6, 2}, {7, 2}};
  switch (index) {
    case 0:
      drawChevronEye(emojiX(16), emojiY(58), colorA);
      drawChevronEye(emojiX(216), emojiY(58), colorB);
      break;
    case 1:
      drawRoundEye(emojiX(48), emojiY(50), colorA);
      drawRoundEye(emojiX(192), emojiY(50), colorB);
      break;
    case 2:
      drawClosedEye(emojiX(48), emojiY(68), colorA);
      drawClosedEye(emojiX(192), emojiY(68), colorB);
      break;
    case 3:
      drawRoundEye(emojiX(32), emojiY(38), colorA);
      drawPixelPath(emojiX(40), emojiY(54), pulsePath, 8, kEmojiPixel, colorB);
      drawPixelLine(emojiX(32), emojiY(38), 9, 8, 13, 12, kEmojiPixel, colorA);
      drawClosedEye(emojiX(208), emojiY(68), colorC);
      break;
    case 4:
      drawRoundEye(emojiX(32), emojiY(50), colorA);
      drawPixelLine(emojiX(52), emojiY(66), 0, 3, 2, 5, kEmojiPixel, colorB);
      drawPixelLine(emojiX(52), emojiY(66), 2, 5, 8, 0, kEmojiPixel, colorB);
      drawChevronEye(emojiX(200), emojiY(58), colorC);
      break;
    case 5:
      drawPixelLine(emojiX(30), emojiY(35), 0, 9, 8, 1, kEmojiPixel, colorA);
      drawPixelLine(emojiX(30), emojiY(35), 0, 1, 8, 9, kEmojiPixel, colorB);
      lcd.fillRect(emojiX(24), emojiY(26), 14, 7, colorA);
      lcd.fillRect(emojiX(20), emojiY(30), 7, 14, colorA);
      lcd.fillRect(emojiX(93), emojiY(101), 14, 7, colorB);
      lcd.fillRect(emojiX(102), emojiY(92), 7, 14, colorB);
      drawRoundEye(emojiX(208), emojiY(50), colorC);
      break;
    case 6:
      drawPixelPattern(emojiX(18), emojiY(30), arrowRows, 12, 7, kEmojiPixel, colorA);
      drawPixelPattern(emojiX(78), emojiY(42), extruderRows, 9, 8, 6, colorB);
      lcd.fillRect(emojiX(68), emojiY(122), 75, 6, colorC);
      for (int16_t hatch = 70; hatch < 164; hatch += 16) {
        lcd.fillRect(emojiX(hatch), emojiY(130), 6, 6, colorC);
      }
      drawRoundEye(emojiX(208), emojiY(50), colorD);
      break;
    case 7:
      drawPixelPattern(emojiX(32), emojiY(46), squareRows, 10, 10, 8, colorA);
      drawPixelPattern(emojiX(56), emojiY(70), smallSquareRows, 4, 4, 4, colorB);
      drawPixelPattern(emojiX(68), emojiY(82), centerPixel, 1, 1, 6, colorC);
      drawRoundEye(emojiX(208), emojiY(50), colorD);
      break;
    case 8:
      drawEmojiSpool(colorA, colorB);
      drawChevronEye(emojiX(208), emojiY(58), colorC);
      break;
    default:
      drawPixelPattern(emojiX(40), emojiY(44), xRows, 9, 9, 8, colorA);
      drawPixelPattern(emojiX(199), emojiY(44), xRows, 9, 9, 8, colorB);
      break;
  }
}

uint32_t randomEmojiInterval() {
  return static_cast<uint32_t>(random(static_cast<long>(kEmojiMinIntervalMs),
                                      static_cast<long>(kEmojiMaxIntervalMs + 1UL)));
}

void drawDashboardBase() {
  lcd.fillScreen(kBackground);
  lcd.drawCircle(kCenter, kCenter, 119, kBorder);
}

void clearDashboardRect(int16_t x, int16_t y, int16_t width, int16_t height) {
  constexpr int16_t kInnerRadius = kRingInnerRadius - 1;
  const int32_t innerRadiusSquared = kInnerRadius * kInnerRadius;
  for (int16_t row = y; row < y + height; ++row) {
    const int16_t dy = row - kCenter;
    const int32_t remaining = innerRadiusSquared - dy * dy;
    if (remaining <= 0) continue;
    const int16_t halfWidth = static_cast<int16_t>(sqrtf(static_cast<float>(remaining)));
    const int16_t circleLeft = kCenter - halfWidth;
    const int16_t circleRight = kCenter + halfWidth + 1;
    const int16_t left = x > circleLeft ? x : circleLeft;
    const int16_t requestedRight = x + width;
    const int16_t right = requestedRight < circleRight ? requestedRight : circleRight;
    if (right > left) lcd.fillRect(left, row, right - left, 1, kBackground);
  }
}

}  // namespace

void DisplayUI::begin(uint16_t rotationDegrees) {
  randomSeed(esp_random());
  pinMode(kLcdBacklight, OUTPUT);
  digitalWrite(kLcdBacklight, HIGH);
  SPI.begin(kLcdSck, -1, kLcdMosi, kLcdCs);
  lcd.begin(40000000);
  lcd.setRotation(static_cast<uint8_t>((rotationDegrees / 90U) % 4U));
  lcd.invertDisplay(true);
  fonts.begin(lcd);
  fonts.setFontMode(1);
  drawDashboardBase();
}

void DisplayUI::showWifiConnecting(const String& ssid, uint8_t animationStep) {
  if (screenMode_ != ScreenMode::WifiConnecting) {
    screenMode_ = ScreenMode::WifiConnecting;
    lcd.fillScreen(kBackground);
    drawStaticRing(kBrandBlue);
    drawArialCentered(kCenter, 46, ArialRaster::kArialBold20, kBrandBlue,
                      "PICASO");
    drawArialCentered(kCenter, 69, ArialRaster::kArialBold20, kBrandBlue,
                      "MONITOR");
    lcd.drawFastHLine(43, 81, 154, kWhite);
    drawArialCentered(kCenter, 100, ArialRaster::kArialRegular14, kLine,
                      "Подключаюсь к WiFi");
    lastSsid_ = "\x01";
    lastWifiAnimationStep_ = 255;
  }
  if (ssid != lastSsid_) {
    lcd.fillRect(20, 116, 200, 24, kBackground);
    const String fitted = fitArialText(ssid, 196, ArialRaster::kArialRegular14);
    drawArialCentered(kCenter, 132, ArialRaster::kArialRegular14, kBrandBlue,
                      fitted);
    lastSsid_ = ssid;
  }
  animationStep %= 6;
  if (animationStep != lastWifiAnimationStep_) {
    drawWifiSymbol(animationStep);
    lastWifiAnimationStep_ = animationStep;
  }
}

void DisplayUI::showSetupPortal(const String& apName, const String& apPassword) {
  if (screenMode_ != ScreenMode::SetupPortal) {
    screenMode_ = ScreenMode::SetupPortal;
    lcd.fillScreen(kBackground);
    drawStaticRing(kSetupOrange);
    drawArialCentered(kCenter, 59, ArialRaster::kArialBold20, kBrandBlue,
                      "НАСТРОЙКА");
    lcd.drawFastHLine(43, 66, 154, kWhite);
    drawArialCentered(kCenter, 86, ArialRaster::kArialRegular14, kLine,
                      "Подключитесь к WiFi");
    drawArialCentered(kCenter, 155, ArialRaster::kArialRegular14, kLine,
                      "Откройте в браузере");
    drawArialCentered(kCenter, 179, ArialRaster::kArialRegular14, kLine,
                      "192.168.4.1");
    lastApName_ = "\x01";
    lastApPassword_ = "\x01";
  }
  if (apName != lastApName_) {
    lcd.fillRect(20, 96, 200, 24, kBackground);
    const String fitted = fitArialText(apName, 196, ArialRaster::kArialRegular14);
    drawArialCentered(kCenter, 111, ArialRaster::kArialRegular14, kBrandBlue,
                      fitted);
    lastApName_ = apName;
  }
  if (apPassword != lastApPassword_) {
    lcd.fillRect(20, 118, 200, 24, kBackground);
    const String password = "Пароль: " + apPassword;
    const String fitted = fitArialText(password, 196, ArialRaster::kArialRegular14);
    drawArialCentered(kCenter, 134, ArialRaster::kArialRegular14, kGreen,
                      fitted);
    lastApPassword_ = apPassword;
  }
}

void DisplayUI::showDashboard(const PicasoDiscovery& discovery,
                              const PrinterSnapshot& snapshot,
                              const String& printerIp,
                              const String& deviceIp,
                              bool wifiConnected,
                              const String& clockTime,
                              const String& clockDate,
                              const WeatherSnapshot& weather,
                              const String& weatherCity) {
  const bool printerAvailable = wifiConnected && snapshot.connected && snapshot.valid;
  const uint32_t now = millis();
  const bool enteringDashboard = screenMode_ != ScreenMode::Dashboard;
  if (enteringDashboard) {
    screenMode_ = ScreenMode::Dashboard;
    dashboardPage_ = 255;
    dashboardStartedAt_ = now;
    dashboardPrinterAvailable_ = printerAvailable;
    emojiActive_ = false;
    emojiCursor_ = kEmojiCount;
    nextEmojiAt_ = dashboardStartedAt_ + randomEmojiInterval();
    lastHeader_ = "";
  } else if (printerAvailable != dashboardPrinterAvailable_) {
    // Start the appropriate sequence immediately when printer connectivity
    // changes instead of waiting for the previous sequence to roll over.
    dashboardPrinterAvailable_ = printerAvailable;
    dashboardStartedAt_ = now;
    dashboardPage_ = 255;
    emojiActive_ = false;
    if (printerAvailable) nextEmojiAt_ = now + randomEmojiInterval();
  }

  if (printerAvailable && emojiActive_) {
    const uint32_t shownFor = now - emojiStartedAt_;
    if (shownFor < kEmojiDurationMs) return;
    emojiActive_ = false;
    dashboardStartedAt_ += shownFor;
    dashboardPage_ = 255;
    drawDashboardBase();
    lastHeader_ = "";
  }

  if (printerAvailable && static_cast<int32_t>(now - nextEmojiAt_) >= 0) {
    if (emojiCursor_ >= kEmojiCount) {
      for (uint8_t i = 0; i < kEmojiCount; ++i) emojiOrder_[i] = i;
      for (int8_t i = kEmojiCount - 1; i > 0; --i) {
        const uint8_t swapIndex = static_cast<uint8_t>(random(i + 1L));
        const uint8_t value = emojiOrder_[i];
        emojiOrder_[i] = emojiOrder_[swapIndex];
        emojiOrder_[swapIndex] = value;
      }
      emojiCursor_ = 0;
    }
    drawEmojiScreen(emojiOrder_[emojiCursor_++]);
    emojiActive_ = true;
    emojiStartedAt_ = now;
    nextEmojiAt_ = now + randomEmojiInterval();
    return;
  }

  static const uint8_t screenSequence[] = {0, 1, 3, 2, 0, 3, 1, 2, 3, 5};
  const uint8_t sequenceLength = weatherCity.length() ? 10 : 9;
  uint8_t page;
  if (!printerAvailable) {
    const bool showWeather = weatherCity.length() != 0;
    const uint32_t noPrinterCycle =
        (now - dashboardStartedAt_) % (showWeather ? 25000UL : 20000UL);
    if (noPrinterCycle < 3000UL) {
      page = 4;
    } else if (noPrinterCycle < 20000UL) {
      page = 3;
    } else {
      page = 5;
    }
  } else {
    const uint8_t screenSlot = static_cast<uint8_t>(
        ((now - dashboardStartedAt_) / 5000UL) % sequenceLength);
    page = screenSequence[screenSlot];
  }
  const bool pageChanged = dashboardPage_ != page;
  if (pageChanged) {
    dashboardPage_ = page;
    lastStatus_ = "";
    lastTempsValues_ = "";
    lastNozzle1_ = "";
    lastNozzle2_ = "";
    lastBed_ = "";
    lastChamber_ = "";
    lastBigProgress_ = "";
    lastMaterial1_ = "";
    lastMaterial2_ = "";
    lastClockTime_ = "";
    lastClockDate_ = "";
    lastWeatherValues_ = "";
    lastSetupAddress_ = "";
    lastStatusColor_ = 0;
    lastProgressColor_ = 0;
    lastProgressPercent_ = -1;
    lastHeader_ = "";
    drawDashboardBase();
    if (page == 1) {
      drawNozzleIcon(67, 66, 1);
      drawNozzleIcon(146, 66, 2);
      drawBedIcon(58, 139);
      drawChamberIcon(139, 132);
    } else if (page == 2) {
      lcd.drawFastHLine(46, 124, 148, kLine);
      drawNozzleIcon(51, 77, 1);
      drawNozzleIcon(51, 142, 2);
    } else if (page == 3) {
      lcd.drawFastHLine(46, 121, 148, kLine);
    } else if (page == 5) {
      lcd.drawFastHLine(46, 120, 148, kLine);
    } else if (page == 4) {
      drawStaticRing(kBrandBlue);
      drawArialCentered(kCenter, 46, ArialRaster::kArialBold20, kBrandBlue,
                        "PICASO");
      drawArialCentered(kCenter, 69, ArialRaster::kArialBold20, kBrandBlue,
                        "MONITOR");
      lcd.drawFastHLine(43, 81, 154, kWhite);
      drawArialCentered(kCenter, 108, ArialRaster::kArialBold20, kIpYellow,
                        "НЕТ СВЯЗИ");
      drawArialCentered(kCenter, 135, ArialRaster::kArialBold20, kIpYellow,
                        "С ПРИНТЕРОМ");
      drawArialCentered(kCenter, 175, ArialRaster::kArialBold12, kSetupOrange,
                        "НАСТРОЙКА:");
    }
  }

  const String deviceAddress = wifiConnected && deviceIp != "0.0.0.0"
                                   ? deviceIp : String("--");
  const String header = page == 0
                           ? fitArialText(printerIp.length() ? printerIp : String("--"),
                                          180, ArialRaster::kArialBold12)
                           : fitArialText(deviceAddress, 180,
                                          ArialRaster::kArialBold12);
  const uint16_t headerColor = page == 0 ? kMuted : kIpYellow;
  if (page < 2 && header != lastHeader_) {
    clearDashboardRect(28, 27, 184, 30);
    drawArialCentered(kCenter, 44, ArialRaster::kArialBold12, headerColor, header);
    lcd.drawFastHLine(46, 52, 148, kLine);
    lastHeader_ = header;
  }

  const StatusStyle style = getStatusStyle(snapshot, wifiConnected);
  const bool hasPrinterData = snapshot.connected && snapshot.valid;
  const String nozzle1 = hasPrinterData ? temperatureValueText(snapshot.nozzle1) : String("--°");
  const String nozzle2 = hasPrinterData ? temperatureValueText(snapshot.nozzle2) : String("--°");
  const String bed = hasPrinterData ? temperatureValueText(snapshot.bed) : String("--°");
  const String chamber = hasPrinterData ? temperatureValueText(snapshot.chamber) : String("--°");

  if (page == 0) {
    const String status(style.text);
    if (status != lastStatus_ || style.color != lastStatusColor_) {
      clearDashboardRect(16, 59, 208, 29);
      const ArialRaster::Font& font = statusFont(status);
      drawArialCentered(kCenter, 82, font, style.color, status);
      lastStatus_ = status;
      lastStatusColor_ = style.color;
    }

    String percent;
    if (style.printing && snapshot.progress >= 0) {
      percent = String(constrain(snapshot.progress, 0, 100)) + "%";
    }
    const String eta = style.printing ? remainingText(snapshot) : String("");
    const String bigProgress = percent + "|" + eta;
    if (bigProgress != lastBigProgress_) {
      clearDashboardRect(32, 94, 176, 31);
      if (percent.length()) {
        drawArialCentered(80, 118, ArialRaster::kArialBold25, kWhite, percent);
      }
      if (eta.length()) {
        drawArialCentered(160, 118, ArialRaster::kArialBold25, kWhite, eta);
      }
      lastBigProgress_ = bigProgress;
    }

    const String tempValues = nozzle1 + "|" + nozzle2 + "|" + bed + "|" + chamber;
    if (tempValues != lastTempsValues_) {
      clearDashboardRect(42, 132, 156, 64);
      const String values[] = {nozzle1, nozzle2, bed, chamber};
      const int16_t centers[] = {78, 162, 78, 162};
      const int16_t baselines[] = {151, 151, 190, 190};
      for (uint8_t i = 0; i < 4; ++i) {
        drawArialCentered(centers[i], baselines[i], ArialRaster::kArialBold16,
                          kLine, values[i]);
      }
      lastTempsValues_ = tempValues;
    }
  } else if (page == 1) {
    const String values[] = {nozzle1, nozzle2, bed, chamber};
    const int16_t centers[] = {80, 160, 80, 160};
    const int16_t baselines[] = {119, 119, 185, 185};
    String* caches[] = {&lastNozzle1_, &lastNozzle2_, &lastBed_, &lastChamber_};
    const int16_t clearY[] = {99, 99, 166, 166};
    for (uint8_t i = 0; i < 4; ++i) {
      if (values[i] != *caches[i]) {
        clearDashboardRect(centers[i] - 42, clearY[i], 84, 25);
        drawArialCentered(centers[i], baselines[i], ArialRaster::kArialBold20,
                          kLine, values[i]);
        *caches[i] = values[i];
      }
    }
  } else if (page == 2) {
    const String material1 = discovery.valid && discovery.material1.length()
                                 ? discovery.material1 : String("Профиль не задан");
    const String material2 = (discovery.nozzle2Size != 0 || discovery.material2.length() != 0)
                                 ? (discovery.material2.length() ? discovery.material2
                                                                  : String("Профиль не задан"))
                                 : String("Нет второго сопла");
    const String material1Fit = fitArialText(material1, 127, ArialRaster::kArialRegular16);
    const String material2Fit = fitArialText(material2, 127, ArialRaster::kArialRegular16);
    if (material1Fit != lastMaterial1_) {
      clearDashboardRect(84, 78, 132, 38);
      drawArialText(89, 97, ArialRaster::kArialRegular16, kWhite, material1Fit);
      lastMaterial1_ = material1Fit;
    }
    if (material2Fit != lastMaterial2_) {
      clearDashboardRect(84, 143, 132, 38);
      drawArialText(89, 163, ArialRaster::kArialRegular16, kWhite, material2Fit);
      lastMaterial2_ = material2Fit;
    }
  } else if (page == 3) {
    if (clockTime != lastClockTime_) {
      clearDashboardRect(38, 57, 164, 53);
      drawArialCentered(kCenter, 99, ArialRaster::kArialBold40, kLine, clockTime);
      lastClockTime_ = clockTime;
    }
    if (clockDate != lastClockDate_) {
      clearDashboardRect(36, 145, 168, 39);
      drawArialCentered(kCenter, 172, ArialRaster::kArialBold25, kLine, clockDate);
      lastClockDate_ = clockDate;
    }
  } else if (page == 5) {
    const String weatherValues = weather.valid
        ? String(weather.currentTemperatureC) + "|" +
              String(weather.tomorrowMaximumC) + "|" + weather.condition + "|" +
              String(static_cast<uint8_t>(weather.icon))
        : String("unavailable");
    if (weatherValues != lastWeatherValues_) {
      clearDashboardRect(36, 58, 168, 49);
      drawWeatherIcon(42, 60, weather.valid ? weather.icon : WeatherIcon::Cloud);
      if (weather.valid) {
        String current = weather.currentTemperatureC > 0 ? "+" : "";
        current += String(weather.currentTemperatureC) + "°";
        drawArialCentered(162, 88, ArialRaster::kArialBold40,
                          weatherTemperatureColor(weather.currentTemperatureC),
                          current);
      } else {
        drawArialCentered(160, 88, ArialRaster::kArialBold12, kMuted,
                          "НЕТ ДАННЫХ");
      }
      clearDashboardRect(38, 128, 164, 62);
      if (weather.valid) {
        String tomorrow = weather.tomorrowMaximumC > 0 ? "+" : "";
        tomorrow += String(weather.tomorrowMaximumC) + "°";
        drawArialText(52, 143, ArialRaster::kArialRegular14, kLine, "Завтра");
        drawArialCentered(78, 181, ArialRaster::kArialBold25,
                          weatherTemperatureColor(weather.tomorrowMaximumC),
                          tomorrow);
        if (weather.condition == "Облачно с прояснениями") {
          drawArialText(112, 143, ArialRaster::kArialRegular14, kLine,
                        "Облачно с");
          drawArialText(112, 160, ArialRaster::kArialRegular14, kLine,
                        "прояснениями");
        } else {
          drawArialText(112, 143, ArialRaster::kArialRegular14, kLine,
                        fitArialText(weather.condition, 90,
                                     ArialRaster::kArialRegular14));
        }
      }
      lastWeatherValues_ = weatherValues;
    }
    lcd.drawFastHLine(46, 120, 148, kLine);
  } else {
    const String deviceAddress = wifiConnected && deviceIp != "0.0.0.0"
                                     ? deviceIp : String("--");
    const String setupAddress = fitArialText(deviceAddress, 180,
                                             ArialRaster::kArialBold12);
    if (setupAddress != lastSetupAddress_) {
      clearDashboardRect(30, 177, 180, 20);
      drawArialCentered(kCenter, 190, ArialRaster::kArialBold12,
                        kSetupOrange, setupAddress);
      lastSetupAddress_ = setupAddress;
    }
  }

  int progress = 0;
  if (style.printing && snapshot.progress >= 0) {
    progress = constrain(snapshot.progress, 0, 100);
  } else if (hasPrinterData && snapshot.status == 7) {
    progress = 100;
  }
  const uint16_t progressColor = style.printing && progress > 0 ? kGreen : kTrack;
  if (page != 4 && (pageChanged || progress != lastProgressPercent_ ||
                    progressColor != lastProgressColor_)) {
    drawProgressRing(progress, style.printing);
    lastProgressPercent_ = progress;
    lastProgressColor_ = progressColor;
  }
}
