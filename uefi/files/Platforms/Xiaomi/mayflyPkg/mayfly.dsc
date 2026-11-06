##
#  Copyright (c) 2011 - 2022, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2020, Intel Corporation. All rights reserved.
#  Copyright (c) 2018, Bingxing Wang. All rights reserved.
#  Copyright (c) Microsoft Corporation.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################
[Defines]
  PLATFORM_NAME                  = mayfly
  PLATFORM_GUID                  = EAC8AC9F-7453-4671-82B4-377ADB9CB837
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/mayflyPkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = mayflyPkg/mayfly.fdf
  USE_CUSTOM_DISPLAY_DRIVER      = 0
  SOC_TYPE                       = 1

!include PalimaPkg/PalimaPkg.dsc.inc

[PcdsFixedAtBuild]
  #
  # DDR Memory
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x80000000

  #
  # UEFI Stack
  #
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0xA760D000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"Xiaomi"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"Xiaomi 12S"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"mayfly"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"Xiaomi_12S_mayfly"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"Xiaomi 12S"

  #
  # Simple Frame Buffer
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|1080
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|2400
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferDelay|0

  #
  # Platform PEI
  #
  gQcomPkgTokenSpaceGuid.PcdPlatformType|"LA"

  #
  # Bring-up: an ASSERT prints and carries on
  #
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x03

[LibraryClasses]
  #
  # Memory Libraries
  #
  MemoryMapLib|mayflyPkg/Library/MemoryMapLib/MemoryMapLib.inf

  #
  # QCOM Libraries
  #
  ConfigurationMapLib|mayflyPkg/Library/ConfigurationMapLib/ConfigurationMapLib.inf

[Components]
  #
  # ACPI Tables
  #
  ingres/AcpiTables.inf

[BuildOptions]
  # Same as mkxnuimg.py (payload_base) and the loader's LOADER_CFLAGS
  *_*_*_CC_FLAGS = -DXNU_PAYLOAD_BASE=0xB0000000ULL
