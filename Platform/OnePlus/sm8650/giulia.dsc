[Defines]
  VENDOR_NAME                    = OnePlus
  PLATFORM_NAME                  = giulia
  PLATFORM_GUID                  = 28f1a3bf-193a-47e3-a7b9-5a435eaab2ee
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010019
  OUTPUT_DIRECTORY               = Build/$(PLATFORM_NAME)
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = Platform/Qualcomm/sm8650/sm8650.fdf
  DEVICE_DXE_FV_COMPONENTS       = Platform/OnePlus/sm8650/giulia.fdf.inc

!include Platform/Qualcomm/sm8650/sm8650.dsc

[BuildOptions.common]
  GCC:*_*_AARCH64_CC_FLAGS = -DENABLE_SIMPLE_INIT -DFORCE_SETUP_ON_BOOT

[PcdsFixedAtBuild.common]
  gArmTokenSpaceGuid.PcdGicDistributorBase|0x17100000
  gArmTokenSpaceGuid.PcdGicRedistributorsBase|0x17180000
  # OnePlus Ace 5 (giuliac) keeps the pre-boot splash framebuffer in
  # cont_splash_region@0xD5100000. The panel is 1264x2780, while the
  # framebuffer is 1280x2784 aligned.
  gQcomTokenSpaceGuid.PcdMipiFrameBufferAddress|0xD5100000
  gQcomTokenSpaceGuid.PcdMipiFrameBufferWidth|1280
  gQcomTokenSpaceGuid.PcdMipiFrameBufferHeight|2784
  gQcomTokenSpaceGuid.PcdMipiFrameBufferVisibleWidth|1264
  gQcomTokenSpaceGuid.PcdMipiFrameBufferVisibleHeight|2780
  gEfiMdePkgTokenSpaceGuid.PcdPlatformBootTimeOut|3

  # Simple Init
  gSimpleInitTokenSpaceGuid.PcdGuiDefaultDPI|414

  gRenegadePkgTokenSpaceGuid.PcdDeviceVendor|"OnePlus"
  gRenegadePkgTokenSpaceGuid.PcdDeviceProduct|"OnePlus Ace 5"
  gRenegadePkgTokenSpaceGuid.PcdDeviceCodeName|"giuliac"

[PcdsDynamicDefault.common]
  # Let SetupBrowser/GraphicsConsole use the active GOP mode instead of trying
  # to force a small desktop-style setup resolution.
  gEfiMdeModulePkgTokenSpaceGuid.PcdVideoHorizontalResolution|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdVideoVerticalResolution|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupVideoHorizontalResolution|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupVideoVerticalResolution|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupConOutRow|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdSetupConOutColumn|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdConOutRow|0
  gEfiMdeModulePkgTokenSpaceGuid.PcdConOutColumn|0
