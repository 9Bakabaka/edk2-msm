#include <Library/ArmGicLib.h>
#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/PlatformPrePiLib.h>

#include "PlatformUtils.h"

#ifdef FORCE_SETUP_ON_BOOT
#ifdef ENABLE_EARLY_SPLASH
#define SIMPLE_SPLASH_GLYPH_WIDTH  5
#define SIMPLE_SPLASH_GLYPH_HEIGHT 7

typedef struct {
  CHAR8 Character;
  UINT8 Rows[SIMPLE_SPLASH_GLYPH_HEIGHT];
} SIMPLE_SPLASH_GLYPH;

STATIC CONST SIMPLE_SPLASH_GLYPH mSimpleSplashGlyphs[] = {
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'C', {0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'I', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F}},
    {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
};

STATIC
UINT32
SplashColor(IN UINT8 Red, IN UINT8 Green, IN UINT8 Blue)
{
  return 0xFF000000u | ((UINT32)Blue << 16) | ((UINT32)Green << 8) |
         (UINT32)Red;
}
#endif

STATIC
UINTN
SplashAlignValue(IN UINTN Value, IN UINTN Alignment)
{
  return (Value + Alignment - 1) & ~(Alignment - 1);
}

STATIC
UINTN
SplashDivRoundUp(IN UINTN Dividend, IN UINTN Divisor)
{
  return (Dividend + Divisor - 1) / Divisor;
}

STATIC
UINTN
GetRgbUbwcMetaSize(IN UINTN Width, IN UINTN Height)
{
  UINTN MacroTileWidth;
  UINTN MacroTileHeight;

  MacroTileWidth  = SplashAlignValue(SplashDivRoundUp(Width, 16), 64);
  MacroTileHeight = SplashAlignValue(SplashDivRoundUp(Height, 4), 16);

  return SplashAlignValue(MacroTileWidth * MacroTileHeight, 4096);
}

#ifdef ENABLE_EARLY_SPLASH
STATIC
VOID
SplashFillRect(
    IN volatile UINT32 *FrameBuffer, IN UINTN Stride, IN UINTN Width,
    IN UINTN Height, IN UINTN X, IN UINTN Y, IN UINTN RectWidth,
    IN UINTN RectHeight, IN UINT32 Color)
{
  UINTN Row;
  UINTN Column;

  if (X >= Width || Y >= Height) {
    return;
  }

  if (RectWidth > Width - X) {
    RectWidth = Width - X;
  }

  if (RectHeight > Height - Y) {
    RectHeight = Height - Y;
  }

  for (Row = 0; Row < RectHeight; Row++) {
    for (Column = 0; Column < RectWidth; Column++) {
      FrameBuffer[(Y + Row) * Stride + X + Column] = Color;
    }
  }
}

STATIC
CONST SIMPLE_SPLASH_GLYPH *
SplashFindGlyph(IN CHAR8 Character)
{
  UINTN Index;

  for (Index = 0; Index < sizeof(mSimpleSplashGlyphs) / sizeof(mSimpleSplashGlyphs[0]); Index++) {
    if (mSimpleSplashGlyphs[Index].Character == Character) {
      return &mSimpleSplashGlyphs[Index];
    }
  }

  return NULL;
}

STATIC
VOID
SplashDrawGlyph(
    IN volatile UINT32 *FrameBuffer, IN UINTN Stride, IN UINTN Width,
    IN UINTN Height, IN UINTN X, IN UINTN Y, IN UINTN Scale,
    IN CHAR8 Character, IN UINT32 Color)
{
  CONST SIMPLE_SPLASH_GLYPH *Glyph;
  UINTN                     Row;
  UINTN                     Column;

  Glyph = SplashFindGlyph(Character);
  if (Glyph == NULL) {
    return;
  }

  for (Row = 0; Row < SIMPLE_SPLASH_GLYPH_HEIGHT; Row++) {
    for (Column = 0; Column < SIMPLE_SPLASH_GLYPH_WIDTH; Column++) {
      if ((Glyph->Rows[Row] & (1u << (SIMPLE_SPLASH_GLYPH_WIDTH - 1 - Column))) != 0) {
        SplashFillRect(
            FrameBuffer, Stride, Width, Height, X + Column * Scale,
            Y + Row * Scale, Scale, Scale, Color);
      }
    }
  }
}

STATIC
VOID
SplashDrawText(
    IN volatile UINT32 *FrameBuffer, IN UINTN Stride, IN UINTN Width,
    IN UINTN Height, IN UINTN X, IN UINTN Y, IN UINTN Scale,
    IN CONST CHAR8 *Text, IN UINT32 Color)
{
  UINTN Index;
  UINTN Advance;

  Advance = (SIMPLE_SPLASH_GLYPH_WIDTH + 1) * Scale;
  for (Index = 0; Text[Index] != '\0'; Index++) {
    SplashDrawGlyph(
        FrameBuffer, Stride, Width, Height, X + Index * Advance, Y, Scale,
        Text[Index], Color);
  }
}

STATIC
VOID
DrawEarlyTianoCoreSplash(VOID)
{
  CONST CHAR8    *Text;
  volatile UINT32 *FrameBuffer;
  UINTN           FrameBufferAddress;
  UINTN           FrameBufferPixelAddress;
  UINTN           UbwcMetaSize;
  UINTN           Stride;
  UINTN           BufferHeight;
  UINTN           VisibleWidth;
  UINTN           VisibleHeight;
  UINTN           TextLength;
  UINTN           Scale;
  UINTN           TextWidth;
  UINTN           TextHeight;
  UINTN           TextX;
  UINTN           TextY;
  UINTN           BarWidth;
  UINTN           BarHeight;
  UINT32          Background;
  UINT32          Foreground;
  UINT32          Accent;

  FrameBufferAddress = FixedPcdGet32(PcdMipiFrameBufferAddress);
  Stride             = FixedPcdGet32(PcdMipiFrameBufferWidth);
  BufferHeight       = FixedPcdGet32(PcdMipiFrameBufferHeight);
  VisibleWidth       = FixedPcdGet32(PcdMipiFrameBufferVisibleWidth);
  VisibleHeight      = FixedPcdGet32(PcdMipiFrameBufferVisibleHeight);

  if (FrameBufferAddress == 0 || Stride == 0 || BufferHeight == 0) {
    return;
  }

  if (VisibleWidth == 0 || VisibleHeight == 0 || VisibleWidth > Stride ||
      VisibleHeight > BufferHeight) {
    VisibleWidth  = Stride;
    VisibleHeight = BufferHeight;
  }

  UbwcMetaSize            = GetRgbUbwcMetaSize(Stride, BufferHeight);
  FrameBufferPixelAddress = FrameBufferAddress + UbwcMetaSize;
  FrameBuffer             = (volatile UINT32 *)FrameBufferPixelAddress;
  Background  = SplashColor(0x07, 0x16, 0x22);
  Foreground  = SplashColor(0xF7, 0xFC, 0xFF);
  Accent      = SplashColor(0x00, 0xA6, 0xC8);

  ZeroMem((VOID *)FrameBufferAddress, UbwcMetaSize);
  SplashFillRect(
      FrameBuffer, Stride, Stride, BufferHeight, 0, 0, Stride, BufferHeight,
      Background);

  Text       = "TIANOCORE";
  TextLength = 9;
  Scale      = VisibleWidth / 60;
  if (Scale < 8) {
    Scale = 8;
  }

  while (Scale > 4) {
    TextWidth = ((TextLength * (SIMPLE_SPLASH_GLYPH_WIDTH + 1)) - 1) * Scale;
    if (TextWidth + 64 <= VisibleWidth) {
      break;
    }
    Scale--;
  }

  TextWidth  = ((TextLength * (SIMPLE_SPLASH_GLYPH_WIDTH + 1)) - 1) * Scale;
  TextHeight = SIMPLE_SPLASH_GLYPH_HEIGHT * Scale;
  TextX      = (VisibleWidth > TextWidth) ? ((VisibleWidth - TextWidth) / 2) : 0;
  TextY      = (VisibleHeight > TextHeight) ? ((VisibleHeight - TextHeight) / 2) : 0;

  SplashDrawText(
      FrameBuffer, Stride, VisibleWidth, VisibleHeight, TextX, TextY, Scale,
      Text, Foreground);

  BarWidth  = (TextWidth > (Scale * 4)) ? (TextWidth - Scale * 4) : TextWidth;
  BarHeight = (Scale > 10) ? (Scale / 3) : 3;
  SplashFillRect(
      FrameBuffer, Stride, VisibleWidth, VisibleHeight,
      (VisibleWidth - BarWidth) / 2, TextY + TextHeight + (Scale * 2),
      BarWidth, BarHeight, Accent);

  WriteBackInvalidateDataCacheRange(
      (VOID *)FrameBufferAddress,
      UbwcMetaSize + Stride * BufferHeight * sizeof(UINT32));
}
#endif
#endif

VOID InitializeSharedUartBuffers(VOID)
{
  UINTN FrameBufferAddress;
  UINTN FrameBufferSize;
  INTN *pFbConPosition =
      NULL;

  FrameBufferAddress = FixedPcdGet32(PcdMipiFrameBufferAddress);
  FrameBufferSize    = FixedPcdGet32(PcdMipiFrameBufferWidth) *
                    FixedPcdGet32(PcdMipiFrameBufferHeight) *
                    FixedPcdGet32(PcdMipiFrameBufferPixelBpp) / 8;

#ifdef FORCE_SETUP_ON_BOOT
  FrameBufferAddress += GetRgbUbwcMetaSize(
      FixedPcdGet32(PcdMipiFrameBufferWidth),
      FixedPcdGet32(PcdMipiFrameBufferHeight));
#endif

  pFbConPosition = (INTN *)(FrameBufferAddress + FrameBufferSize);

  *(pFbConPosition + 0) = 0;
  *(pFbConPosition + 1) = 0;
}

VOID UartInit(VOID)
{
  SerialPortInitialize();
  InitializeSharedUartBuffers();

#ifdef ENABLE_EARLY_SPLASH
  DrawEarlyTianoCoreSplash();
#endif

  DEBUG((EFI_D_INFO, "\nRenegade Project edk2-msm (AArch64)\n"));
  DEBUG(
      (EFI_D_INFO, "Firmware version %s built %a %a\n\n",
       (CHAR16 *)PcdGetPtr(PcdFirmwareVersionString), __TIME__, __DATE__));
}

VOID PlatformInitialize(VOID)
{
  // Initialize UART Serial
  UartInit();

  // Initialize GIC
  MmioWrite32(
      GICR_WAKER_CURRENT_CPU,
      (MmioRead32(GICR_WAKER_CURRENT_CPU) & ~GIC_WAKER_PROCESSORSLEEP));
}
