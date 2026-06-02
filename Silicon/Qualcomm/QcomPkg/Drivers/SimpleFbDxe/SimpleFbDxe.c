/* SimpleFbDxe: Simple FrameBuffer */
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FrameBufferBltLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <PiDxe.h>
#include <Protocol/GraphicsOutput.h>
#include <Uefi.h>

/// Defines
/*
 * Convert enum video_log2_bpp to bytes and bits. Note we omit the outer
 * brackets to allow multiplication by fractional pixels.
 */
#define VNBYTES(bpix) (1 << (bpix)) / 8
#define VNBITS(bpix) (1 << (bpix))

#define FB_BITS_PER_PIXEL (32)
#define FB_BYTES_PER_PIXEL (FB_BITS_PER_PIXEL / 8)

/*
 * Bits per pixel selector. Each value n is such that the bits-per-pixel is
 * 2 ^ n
 */
enum video_log2_bpp {
  VIDEO_BPP1 = 0,
  VIDEO_BPP2,
  VIDEO_BPP4,
  VIDEO_BPP8,
  VIDEO_BPP16,
  VIDEO_BPP32,
};

typedef struct {
  VENDOR_DEVICE_PATH DisplayDevicePath;
  EFI_DEVICE_PATH    EndDevicePath;
} DISPLAY_DEVICE_PATH;

DISPLAY_DEVICE_PATH mDisplayDevicePath = {
    {{HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      {
          (UINT8)(sizeof(VENDOR_DEVICE_PATH)),
          (UINT8)((sizeof(VENDOR_DEVICE_PATH)) >> 8),
      }},
     EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID},
    {END_DEVICE_PATH_TYPE,
     END_ENTIRE_DEVICE_PATH_SUBTYPE,
     {sizeof(EFI_DEVICE_PATH_PROTOCOL), 0}}};

/// Declares

STATIC FRAME_BUFFER_CONFIGURE *mFrameBufferBltLibConfigure;
STATIC UINTN                   mFrameBufferBltLibConfigureSize;

#ifdef FORCE_SETUP_ON_BOOT
STATIC BOOLEAN mDpuLinearFrameBufferEnabled;
STATIC UINT32  mDpuActivePipeMask;
STATIC UINT32  mDpuPendingFlushMask;
#endif

STATIC
BOOLEAN
RangesOverlap(
    IN UINT64 BaseA, IN UINT64 SizeA, IN UINT64 BaseB, IN UINT64 SizeB)
{
  UINT64 EndA;
  UINT64 EndB;

  if (SizeA == 0 || SizeB == 0)
    return FALSE;

  if (BaseA > ((UINT64)-1) - SizeA || BaseB > ((UINT64)-1) - SizeB)
    return TRUE;

  EndA = BaseA + SizeA;
  EndB = BaseB + SizeB;

  return (BaseA < EndB) && (BaseB < EndA);
}

#ifdef FORCE_SETUP_ON_BOOT
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

#define ACE5_MDP_BASE         0x0AE00000u
#define ACE5_SPLASH_BASE      0xD5100000u
#define ACE5_SPLASH_SIZE      0x02B00000u
#define DPU_FETCH_CONFIG_RESET_VALUE 0x00000087u

#define SSPP_SRC0_ADDR             0x014
#define SSPP_SRC1_ADDR             0x018
#define SSPP_SRC2_ADDR             0x01C
#define SSPP_SRC3_ADDR             0x020
#define SSPP_SRC_YSTRIDE0          0x024
#define SSPP_SRC_YSTRIDE1          0x028
#define SSPP_SRC_FORMAT            0x030
#define SSPP_SRC_UNPACK_PATTERN    0x034
#define SSPP_SRC_OP_MODE           0x038
#define SSPP_UBWC_STATIC_CTRL      0x044
#define SSPP_FETCH_CONFIG          0x048
#define SSPP_UBWC_ERROR_STATUS     0x138
#define SSPP_SRC_FORMAT_REC1       0x174
#define SSPP_SRC_UNPACK_PATTERN_REC1 0x178
#define SSPP_SRC_OP_MODE_REC1      0x17C
#define SSPP_UBWC_STATIC_CTRL_REC1 0x1C0
#define SSPP_UBWC_ERROR_STATUS_REC1 0x1C8

#define CTL_FLUSH             0x018
#define CTL_START             0x01C
#define CTL_PREPARE           0x0D0
#define CTL_FETCH_PIPE_ACTIVE 0x0FC
#define CTL_INTF_ACTIVE       0x0F4
#define CTL_INTF_FLUSH        0x110
#define CTL_PIPE_ACTIVE       0x12C
#define CTL_FLUSH_MASK_CTL    (1u << 17)
#define CTL_INTF_IDX          31

#define DPU_ABGR8888_LINEAR_SRC_FORMAT 0x000237FFu
#define DPU_ABGR8888_UNPACK_PATTERN    0x03010002u
#define DPU_OP_BWC_BITS                0x00000007u
#define DPU_OP_PE_OVERRIDE             (1u << 31)

STATIC CONST UINT32 mDpuSsppOffsets[] = {
    0x05000, 0x07000, 0x09000, 0x0B000,
    0x25000, 0x27000, 0x29000, 0x2B000, 0x2D000, 0x2F000,
};

STATIC CONST UINT32 mDpuSsppActiveBits[] = {
    16, 17, 18, 19,
    0, 1, 2, 3, 4, 5,
};

STATIC CONST UINT32 mDpuSsppFlushBits[] = {
    0, 1, 2, 18,
    11, 12, 24, 25, 13, 14,
};

STATIC CONST UINT32 mDpuCtlOffsets[] = {
    0x16000, 0x17000, 0x18000, 0x19000, 0x1A000, 0x1B000,
};

STATIC
BOOLEAN
DpuAddressInSplash(IN UINT32 Address)
{
  return Address >= ACE5_SPLASH_BASE &&
         Address < (ACE5_SPLASH_BASE + ACE5_SPLASH_SIZE);
}

STATIC
BOOLEAN
DpuSsppUsesSplash(IN UINTN PipeBase)
{
  return DpuAddressInSplash(MmioRead32(PipeBase + SSPP_SRC0_ADDR)) ||
         DpuAddressInSplash(MmioRead32(PipeBase + SSPP_SRC1_ADDR)) ||
         DpuAddressInSplash(MmioRead32(PipeBase + SSPP_SRC2_ADDR)) ||
         DpuAddressInSplash(MmioRead32(PipeBase + SSPP_SRC3_ADDR));
}

STATIC
UINT32
DpuReadActivePipeMask(VOID)
{
  UINTN  Index;
  UINTN  CtlBase;
  UINT32 ActiveMask;

  ActiveMask = 0;

  for (Index = 0; Index < sizeof(mDpuCtlOffsets) / sizeof(mDpuCtlOffsets[0]); Index++) {
    CtlBase = ACE5_MDP_BASE + mDpuCtlOffsets[Index];
    ActiveMask |= MmioRead32(CtlBase + CTL_FETCH_PIPE_ACTIVE);
  }

  return ActiveMask;
}

STATIC
VOID
DpuProgramSsppLinear(
    IN UINTN PipeBase, IN UINT32 FrameBufferAddress, IN UINT32 Stride)
{
  UINT32 OpMode;

  MmioWrite32(PipeBase + SSPP_SRC0_ADDR, FrameBufferAddress);
  MmioWrite32(PipeBase + SSPP_SRC1_ADDR, 0);
  MmioWrite32(PipeBase + SSPP_SRC2_ADDR, 0);
  MmioWrite32(PipeBase + SSPP_SRC3_ADDR, 0);
  MmioWrite32(PipeBase + SSPP_SRC_YSTRIDE0, Stride);
  MmioWrite32(PipeBase + SSPP_SRC_YSTRIDE1, 0);
  MmioWrite32(PipeBase + SSPP_FETCH_CONFIG, DPU_FETCH_CONFIG_RESET_VALUE);
  MmioWrite32(PipeBase + SSPP_UBWC_STATIC_CTRL, 0);
  MmioWrite32(PipeBase + SSPP_SRC_FORMAT, DPU_ABGR8888_LINEAR_SRC_FORMAT);
  MmioWrite32(PipeBase + SSPP_SRC_UNPACK_PATTERN, DPU_ABGR8888_UNPACK_PATTERN);

  OpMode = MmioRead32(PipeBase + SSPP_SRC_OP_MODE);
  OpMode &= ~DPU_OP_BWC_BITS;
  OpMode |= DPU_OP_PE_OVERRIDE;
  MmioWrite32(PipeBase + SSPP_SRC_OP_MODE, OpMode);
  MmioWrite32(PipeBase + SSPP_UBWC_ERROR_STATUS, (1u << 31));

  MmioWrite32(PipeBase + SSPP_UBWC_STATIC_CTRL_REC1, 0);
  MmioWrite32(PipeBase + SSPP_SRC_FORMAT_REC1, DPU_ABGR8888_LINEAR_SRC_FORMAT);
  MmioWrite32(
      PipeBase + SSPP_SRC_UNPACK_PATTERN_REC1,
      DPU_ABGR8888_UNPACK_PATTERN);
  OpMode = MmioRead32(PipeBase + SSPP_SRC_OP_MODE_REC1);
  OpMode &= ~DPU_OP_BWC_BITS;
  OpMode |= DPU_OP_PE_OVERRIDE;
  MmioWrite32(PipeBase + SSPP_SRC_OP_MODE_REC1, OpMode);
  MmioWrite32(PipeBase + SSPP_UBWC_ERROR_STATUS_REC1, (1u << 31));
}

STATIC
VOID
DpuMarkSsppForFlush(IN UINTN Index)
{
  mDpuActivePipeMask |= (1u << mDpuSsppActiveBits[Index]);
  mDpuPendingFlushMask |= (1u << mDpuSsppFlushBits[Index]);
}

STATIC
VOID
DpuFlushActiveSplashPipes(VOID)
{
  UINTN  Index;
  UINTN  CtlBase;
  UINT32 FetchActive;
  UINT32 IntfActive;
  UINT32 FlushMask;

  if (mDpuActivePipeMask == 0 || mDpuPendingFlushMask == 0) {
    return;
  }

  for (Index = 0; Index < sizeof(mDpuCtlOffsets) / sizeof(mDpuCtlOffsets[0]); Index++) {
    CtlBase     = ACE5_MDP_BASE + mDpuCtlOffsets[Index];
    FetchActive = MmioRead32(CtlBase + CTL_FETCH_PIPE_ACTIVE);

    if ((FetchActive & mDpuActivePipeMask) == 0 && FetchActive != 0) {
      continue;
    }

    if (FetchActive == 0) {
      MmioWrite32(CtlBase + CTL_FETCH_PIPE_ACTIVE, mDpuActivePipeMask);
      MmioWrite32(CtlBase + CTL_PIPE_ACTIVE, mDpuActivePipeMask);
    }

    FlushMask  = mDpuPendingFlushMask | CTL_FLUSH_MASK_CTL;
    IntfActive = MmioRead32(CtlBase + CTL_INTF_ACTIVE);
    if (IntfActive != 0) {
      MmioWrite32(CtlBase + CTL_INTF_FLUSH, IntfActive);
      FlushMask |= (1u << CTL_INTF_IDX);
    }

    MmioWrite32(CtlBase + CTL_FLUSH, FlushMask);
    MmioWrite32(CtlBase + CTL_PREPARE, 1);
    MmioWrite32(CtlBase + CTL_START, 1);
  }
}

STATIC
BOOLEAN
DpuSwitchSplashPipesToLinear(
    IN UINT32 FrameBufferAddress, IN UINT32 Stride)
{
  UINTN  Index;
  UINTN  PipeBase;
  UINT32 ActivePipeMask;
  UINT32 PipeActiveMask;

  mDpuActivePipeMask   = 0;
  mDpuPendingFlushMask = 0;

  for (Index = 0; Index < sizeof(mDpuSsppOffsets) / sizeof(mDpuSsppOffsets[0]); Index++) {
    PipeBase = ACE5_MDP_BASE + mDpuSsppOffsets[Index];
    if (!DpuSsppUsesSplash(PipeBase)) {
      continue;
    }

    DpuProgramSsppLinear(PipeBase, FrameBufferAddress, Stride);
    DpuMarkSsppForFlush(Index);
  }

  if (mDpuActivePipeMask == 0) {
    ActivePipeMask = DpuReadActivePipeMask();

    for (Index = 0; Index < sizeof(mDpuSsppOffsets) / sizeof(mDpuSsppOffsets[0]); Index++) {
      PipeActiveMask = (1u << mDpuSsppActiveBits[Index]);
      if ((ActivePipeMask & PipeActiveMask) == 0) {
        continue;
      }

      PipeBase = ACE5_MDP_BASE + mDpuSsppOffsets[Index];
      DpuProgramSsppLinear(PipeBase, FrameBufferAddress, Stride);
      DpuMarkSsppForFlush(Index);
    }
  }

  if (mDpuActivePipeMask == 0 || mDpuPendingFlushMask == 0) {
    return FALSE;
  }

  DpuFlushActiveSplashPipes();
  return TRUE;
}

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
DrawTianoCoreSplash(
    IN EFI_PHYSICAL_ADDRESS FrameBufferAddress, IN UINTN Stride,
    IN UINTN BufferHeight, IN UINTN VisibleWidth, IN UINTN VisibleHeight)
{
  CONST CHAR8     *Text;
  volatile UINT32 *FrameBuffer;
  UINTN            TextLength;
  UINTN            Scale;
  UINTN            TextWidth;
  UINTN            TextHeight;
  UINTN            TextX;
  UINTN            TextY;
  UINTN            BarWidth;
  UINTN            BarHeight;
  UINT32           Background;
  UINT32           Foreground;
  UINT32           Accent;

  if (FrameBufferAddress == 0 || Stride == 0 || BufferHeight == 0) {
    return;
  }

  if (VisibleWidth == 0 || VisibleHeight == 0 || VisibleWidth > Stride ||
      VisibleHeight > BufferHeight) {
    VisibleWidth  = Stride;
    VisibleHeight = BufferHeight;
  }

  FrameBuffer = (volatile UINT32 *)(UINTN)FrameBufferAddress;
  Background  = SplashColor(0x07, 0x16, 0x22);
  Foreground  = SplashColor(0xF7, 0xFC, 0xFF);
  Accent      = SplashColor(0x00, 0xA6, 0xC8);

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
      (VOID *)(UINTN)FrameBufferAddress, Stride * BufferHeight * sizeof(UINT32));
}
#endif

STATIC
EFI_STATUS
EFIAPI
DisplayQueryMode(
    IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This, IN UINT32 ModeNumber,
    OUT UINTN *SizeOfInfo, OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);

STATIC
EFI_STATUS
EFIAPI
DisplaySetMode(IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This, IN UINT32 ModeNumber);

STATIC
EFI_STATUS
EFIAPI
DisplayBlt(
    IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL *            BltBuffer,
    OPTIONAL IN EFI_GRAPHICS_OUTPUT_BLT_OPERATION BltOperation,
    IN UINTN SourceX, IN UINTN SourceY, IN UINTN DestinationX,
    IN UINTN DestinationY, IN UINTN Width, IN UINTN Height,
    IN UINTN Delta OPTIONAL);

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL mDisplay = {
    DisplayQueryMode, DisplaySetMode, DisplayBlt, NULL};

STATIC
EFI_STATUS
EFIAPI
DisplayQueryMode(
    IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This, IN UINT32 ModeNumber,
    OUT UINTN *SizeOfInfo, OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info)
{
  EFI_STATUS Status;
  Status = gBS->AllocatePool(
      EfiBootServicesData, sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION),
      (VOID **)Info);

  ASSERT_EFI_ERROR(Status);

  *SizeOfInfo                   = sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION);
  (*Info)->Version              = This->Mode->Info->Version;
  (*Info)->HorizontalResolution = This->Mode->Info->HorizontalResolution;
  (*Info)->VerticalResolution   = This->Mode->Info->VerticalResolution;
  (*Info)->PixelFormat          = This->Mode->Info->PixelFormat;
  (*Info)->PixelsPerScanLine    = This->Mode->Info->PixelsPerScanLine;

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
DisplaySetMode(IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This, IN UINT32 ModeNumber)
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
DisplayBlt(
    IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL *            BltBuffer,
    OPTIONAL IN EFI_GRAPHICS_OUTPUT_BLT_OPERATION BltOperation,
    IN UINTN SourceX, IN UINTN SourceY, IN UINTN DestinationX,
    IN UINTN DestinationY, IN UINTN Width, IN UINTN Height,
    IN UINTN Delta OPTIONAL)
{

  RETURN_STATUS Status;
  EFI_TPL       Tpl;
  //
  // We have to raise to TPL_NOTIFY, so we make an atomic write to the frame
  // buffer. We would not want a timer based event (Cursor, ...) to come in
  // while we are doing this operation.
  //
  Tpl    = gBS->RaiseTPL(TPL_NOTIFY);
  Status = FrameBufferBlt(
      mFrameBufferBltLibConfigure, BltBuffer, BltOperation, SourceX, SourceY,
      DestinationX, DestinationY, Width, Height, Delta);
  gBS->RestoreTPL(Tpl);

  WriteBackInvalidateDataCacheRange(
      (void *)mDisplay.Mode->FrameBufferBase, mDisplay.Mode->FrameBufferSize);

#ifdef FORCE_SETUP_ON_BOOT
  if (mDpuLinearFrameBufferEnabled) {
    DpuFlushActiveSplashPipes();
  }
#endif

  return RETURN_ERROR(Status) ? EFI_INVALID_PARAMETER : EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
SimpleFbDxeInitialize(
    IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{

  EFI_STATUS Status             = EFI_SUCCESS;
  EFI_HANDLE hUEFIDisplayHandle = NULL;

  /* Retrieve simple frame buffer from pre-SEC bootloader */
  DEBUG(
      (EFI_D_ERROR,
       "SimpleFbDxe: Retrieve MIPI FrameBuffer parameters from PCD\n"));
  UINT32 MipiFrameBufferAddr   = FixedPcdGet32(PcdMipiFrameBufferAddress);
  UINT32 MipiFrameBufferWidth  = FixedPcdGet32(PcdMipiFrameBufferWidth);
  UINT32 MipiFrameBufferHeight = FixedPcdGet32(PcdMipiFrameBufferHeight);
  UINT32 VisibleWidth          = FixedPcdGet32(PcdMipiFrameBufferVisibleWidth);
  UINT32 VisibleHeight         = FixedPcdGet32(PcdMipiFrameBufferVisibleHeight);
  UINT32 DisplayWidth          = MipiFrameBufferWidth;
  UINT32 DisplayHeight         = MipiFrameBufferHeight;

  /* Sanity check */
  if (MipiFrameBufferAddr == 0 || MipiFrameBufferWidth == 0 ||
      MipiFrameBufferHeight == 0) {
    DEBUG((EFI_D_ERROR, "SimpleFbDxe: Invalid FrameBuffer parameters\n"));
    return EFI_DEVICE_ERROR;
  }

  if (VisibleWidth != 0 && VisibleHeight != 0 &&
      VisibleWidth <= MipiFrameBufferWidth &&
      VisibleHeight <= MipiFrameBufferHeight &&
      MipiFrameBufferWidth - VisibleWidth <= 64 &&
      MipiFrameBufferHeight - VisibleHeight <= 64) {
    DisplayWidth  = VisibleWidth;
    DisplayHeight = VisibleHeight;
  }

  /* Prepare struct */
  if (mDisplay.Mode == NULL) {
    Status = gBS->AllocatePool(
        EfiBootServicesData, sizeof(EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE),
        (VOID **)&mDisplay.Mode);

    ASSERT_EFI_ERROR(Status);
    if (EFI_ERROR(Status))
      return Status;

    ZeroMem(mDisplay.Mode, sizeof(EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE));
  }

  if (mDisplay.Mode->Info == NULL) {
    Status = gBS->AllocatePool(
        EfiBootServicesData, sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION),
        (VOID **)&mDisplay.Mode->Info);

    ASSERT_EFI_ERROR(Status);
    if (EFI_ERROR(Status))
      return Status;

    ZeroMem(mDisplay.Mode->Info, sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION));
  }

  /* Set information */
  mDisplay.Mode->MaxMode       = 1;
  mDisplay.Mode->Mode          = 0;
  mDisplay.Mode->Info->Version = 0;

  mDisplay.Mode->Info->HorizontalResolution = DisplayWidth;
  mDisplay.Mode->Info->VerticalResolution   = DisplayHeight;

  /* SimpleFB runs on a8r8g8b8 (VIDEO_BPP32) for DB410c */
  UINT32               LineLength = MipiFrameBufferWidth * VNBYTES(VIDEO_BPP32);
  UINT64               FrameBufferSize64;
  UINTN                FrameBufferSize;
  EFI_PHYSICAL_ADDRESS FrameBufferAddress        = MipiFrameBufferAddr;

  FrameBufferSize64 = (UINT64)LineLength * MipiFrameBufferHeight;

  if (FrameBufferSize64 == 0 || FrameBufferSize64 > (UINT64)((UINTN)-1)) {
    DEBUG((EFI_D_ERROR, "SimpleFbDxe: Invalid FrameBuffer size\n"));
    return EFI_DEVICE_ERROR;
  }

  FrameBufferSize = (UINTN)FrameBufferSize64;

  if (RangesOverlap(
          FrameBufferAddress, FrameBufferSize,
          FixedPcdGet64(PcdFdBaseAddress), FixedPcdGet32(PcdFdSize)) ||
      RangesOverlap(
          FrameBufferAddress, FrameBufferSize,
          FixedPcdGet64(PcdUefiMemPoolBase),
          FixedPcdGet32(PcdUefiMemPoolSize))) {
    DEBUG(
        (EFI_D_ERROR,
         "SimpleFbDxe: Refusing unsafe FrameBuffer range 0x%lx-0x%lx\n",
         FrameBufferAddress, FrameBufferAddress + FrameBufferSize - 1));
    return EFI_DEVICE_ERROR;
  }

#ifdef FORCE_SETUP_ON_BOOT
  mDpuLinearFrameBufferEnabled = DpuSwitchSplashPipesToLinear(
      (UINT32)FrameBufferAddress, LineLength);
  if (!mDpuLinearFrameBufferEnabled) {
    DEBUG(
        (EFI_D_ERROR,
         "SimpleFbDxe: No active DPU splash pipe found; refusing framebuffer writes\n"));
    return EFI_DEVICE_ERROR;
  }
#endif

  mDisplay.Mode->Info->PixelsPerScanLine = MipiFrameBufferWidth;
  mDisplay.Mode->Info->PixelFormat = PixelRedGreenBlueReserved8BitPerColor;
  mDisplay.Mode->SizeOfInfo      = sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION);
  mDisplay.Mode->FrameBufferBase = FrameBufferAddress;
  mDisplay.Mode->FrameBufferSize = FrameBufferSize;

  //
  // Create the FrameBufferBltLib configuration.
  //
  Status = FrameBufferBltConfigure(
      (VOID *)(UINTN)mDisplay.Mode->FrameBufferBase, mDisplay.Mode->Info,
      mFrameBufferBltLibConfigure, &mFrameBufferBltLibConfigureSize);
  if (Status == RETURN_BUFFER_TOO_SMALL) {
    mFrameBufferBltLibConfigure = AllocatePool(mFrameBufferBltLibConfigureSize);
    if (mFrameBufferBltLibConfigure != NULL) {
      Status = FrameBufferBltConfigure(
          (VOID *)(UINTN)mDisplay.Mode->FrameBufferBase, mDisplay.Mode->Info,
          mFrameBufferBltLibConfigure, &mFrameBufferBltLibConfigureSize);
    }
  }
  ASSERT_EFI_ERROR(Status);

#ifdef FORCE_SETUP_ON_BOOT
  DrawTianoCoreSplash(
      FrameBufferAddress, MipiFrameBufferWidth, MipiFrameBufferHeight,
      DisplayWidth, DisplayHeight);
  WriteBackInvalidateDataCacheRange(
      (VOID *)(UINTN)FrameBufferAddress, FrameBufferSize);
  DpuFlushActiveSplashPipes();
#else
  ZeroMem((void *)FrameBufferAddress, FrameBufferSize);
  WriteBackInvalidateDataCacheRange(
      (void *)FrameBufferAddress, FrameBufferSize);
#endif

  /* Register handle */
  Status = gBS->InstallMultipleProtocolInterfaces(
      &hUEFIDisplayHandle, &gEfiDevicePathProtocolGuid, &mDisplayDevicePath,
      &gEfiGraphicsOutputProtocolGuid, &mDisplay, NULL);

  ASSERT_EFI_ERROR(Status);

  return Status;
}
