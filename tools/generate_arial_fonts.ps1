# SPDX-License-Identifier: GPL-3.0-only

Add-Type -AssemblyName System.Drawing

$ErrorActionPreference = 'Stop'
$outputPath = Join-Path $PSScriptRoot '..\include\arial_fonts.h'
$outputPath = [System.IO.Path]::GetFullPath($outputPath)
$fontFamily = New-Object System.Drawing.FontFamily('Arial')
$format = [System.Drawing.StringFormat]::GenericTypographic
$fontsToBuild = @(
  @{ Name = 'ArialBold8'; Size = 8; Style = [System.Drawing.FontStyle]::Bold },
  @{ Name = 'ArialBold12'; Size = 12; Style = [System.Drawing.FontStyle]::Bold },
  @{ Name = 'ArialBold14'; Size = 14; Style = [System.Drawing.FontStyle]::Bold },
  @{ Name = 'ArialBold16'; Size = 16; Style = [System.Drawing.FontStyle]::Bold },
  @{ Name = 'ArialBold20'; Size = 20; Style = [System.Drawing.FontStyle]::Bold },
  @{ Name = 'ArialBold25'; Size = 25; Style = [System.Drawing.FontStyle]::Bold },
  @{ Name = 'ArialBold40'; Size = 40; Style = [System.Drawing.FontStyle]::Bold },
  @{ Name = 'ArialRegular14'; Size = 14; Style = [System.Drawing.FontStyle]::Regular },
  @{ Name = 'ArialRegular16'; Size = 16; Style = [System.Drawing.FontStyle]::Regular }
)

$codepoints = [System.Collections.Generic.SortedSet[int]]::new()
for ($code = 32; $code -le 126; $code++) { [void]$codepoints.Add($code) }
[void]$codepoints.Add(0x0401)
for ($code = 0x0410; $code -le 0x042F; $code++) { [void]$codepoints.Add($code) }
for ($code = 0x0430; $code -le 0x044F; $code++) { [void]$codepoints.Add($code) }
[void]$codepoints.Add(0x0451)
[void]$codepoints.Add(0x00B0)
[void]$codepoints.Add(0x2013)
[void]$codepoints.Add(0x2014)

$measureBitmap = [System.Drawing.Bitmap]::new(1, 1)
$measureGraphics = [System.Drawing.Graphics]::FromImage($measureBitmap)
$measureGraphics.PageUnit = [System.Drawing.GraphicsUnit]::Pixel
$measureGraphics.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit

$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add('#pragma once')
$lines.Add('')
$lines.Add('// Contains glyph bitmaps generated from Arial; this generated asset is not covered by the project GPL license. See README.md.')
$lines.Add('')
$lines.Add('#include <stdint.h>')
$lines.Add('')
$lines.Add('// Generated from the locally installed Arial font by tools/generate_arial_fonts.ps1.')
$lines.Add('namespace ArialRaster {')
$lines.Add('struct Glyph { uint16_t codepoint; uint8_t width; uint8_t height; int8_t xOffset; int8_t yOffset; uint8_t xAdvance; uint16_t bitmapOffset; };')
$lines.Add('struct Font { const Glyph* glyphs; const uint8_t* bitmap; uint16_t glyphCount; uint8_t pixelSize; };')
$lines.Add('')

foreach ($fontSpec in $fontsToBuild) {
  $font = [System.Drawing.Font]::new('Arial', [single]$fontSpec.Size, [System.Drawing.FontStyle]$fontSpec.Style, [System.Drawing.GraphicsUnit]::Pixel)
  $ascent = [single]($fontFamily.GetCellAscent([System.Drawing.FontStyle]$fontSpec.Style) * $fontSpec.Size / $fontFamily.GetEmHeight([System.Drawing.FontStyle]$fontSpec.Style))
  $glyphRows = [System.Collections.Generic.List[string]]::new()
  $bitmapBytes = [System.Collections.Generic.List[byte]]::new()

  foreach ($codepoint in $codepoints) {
    $character = [string][char]$codepoint
    $path = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $path.AddString($character, $fontFamily, [int]$fontSpec.Style, [single]$fontSpec.Size, [System.Drawing.PointF]::new(0, 0), $format)
    $bounds = $path.GetBounds()
    $measure = $measureGraphics.MeasureString($character, $font, [System.Drawing.PointF]::new(0, 0), $format)
    $advance = if ($codepoint -eq 0x0020) {
      [Math]::Max(1, [int][Math]::Round($fontSpec.Size * 0.278))
    } else {
      [Math]::Max(1, [int][Math]::Round($measure.Width))
    }

    if ($path.PointCount -eq 0 -or $bounds.Width -le 0 -or $bounds.Height -le 0) {
      $glyphRows.Add(('  {{0x{0:X4}, 0, 0, 0, 0, {1}, {2}}},' -f $codepoint, $advance, $bitmapBytes.Count))
      $path.Dispose()
      continue
    }

    $left = [int][Math]::Floor($bounds.Left)
    $top = [int][Math]::Floor($bounds.Top)
    $width = [Math]::Min(255, [int][Math]::Ceiling($bounds.Right) - $left + 1)
    $height = [Math]::Min(255, [int][Math]::Ceiling($bounds.Bottom) - $top + 1)
    $glyphBitmap = [System.Drawing.Bitmap]::new($width, $height, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($glyphBitmap)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
    $graphics.TranslateTransform(-$left, -$top)
    $graphics.FillPath([System.Drawing.Brushes]::White, $path)
    $graphics.Dispose()

    $offset = $bitmapBytes.Count
    $pixelIndex = 0
    $packed = 0
    for ($y = 0; $y -lt $height; $y++) {
      for ($x = 0; $x -lt $width; $x++) {
        $alpha = $glyphBitmap.GetPixel($x, $y).A
        if ($alpha -lt 36) { $level = 0 }
        elseif ($alpha -lt 112) { $level = 1 }
        elseif ($alpha -lt 202) { $level = 2 }
        else { $level = 3 }
        $packed = $packed -bor ($level -shl (6 - 2 * ($pixelIndex % 4)))
        $pixelIndex++
        if (($pixelIndex % 4) -eq 0) {
          $bitmapBytes.Add([byte]$packed)
          $packed = 0
        }
      }
    }
    if (($pixelIndex % 4) -ne 0) { $bitmapBytes.Add([byte]$packed) }

    $glyphRows.Add(('  {{0x{0:X4}, {1}, {2}, {3}, {4}, {5}, {6}}},' -f $codepoint, $width, $height, $left, ($top - [int][Math]::Round($ascent)), $advance, $offset))
    $glyphBitmap.Dispose()
    $path.Dispose()
  }

  $lines.Add(('static const Glyph k{0}Glyphs[] = {{' -f $fontSpec.Name))
  foreach ($row in $glyphRows) { $lines.Add($row) }
  $lines.Add('};')
  $lines.Add(('static const uint8_t k{0}Bitmap[] = {{' -f $fontSpec.Name))
  for ($i = 0; $i -lt $bitmapBytes.Count; $i += 16) {
    $end = [Math]::Min($i + 15, $bitmapBytes.Count - 1)
    $chunk = @()
    for ($j = $i; $j -le $end; $j++) { $chunk += ('0x{0:X2}' -f $bitmapBytes[$j]) }
    $lines.Add(('  ' + ($chunk -join ', ') + ','))
  }
  $lines.Add('};')
  $lines.Add(('static const Font k{0} = {{k{0}Glyphs, k{0}Bitmap, sizeof(k{0}Glyphs) / sizeof(k{0}Glyphs[0]), {1}}};' -f $fontSpec.Name, $fontSpec.Size))
  $lines.Add('')
  $font.Dispose()
  Write-Host ("Generated {0}: {1} glyphs, {2} bitmap bytes" -f $fontSpec.Name, $glyphRows.Count, $bitmapBytes.Count)
}

$lines.Add('}')
$measureGraphics.Dispose()
$measureBitmap.Dispose()
[System.IO.File]::WriteAllLines($outputPath, $lines, [System.Text.Encoding]::ASCII)
Write-Host "Wrote $outputPath"
