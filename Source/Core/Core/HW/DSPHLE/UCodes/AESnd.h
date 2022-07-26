// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <utility>

#include "Common/CommonTypes.h"
#include "Core/HW/DSPHLE/UCodes/UCodes.h"

namespace DSP::HLE
{
class DSPHLE;

class AESndUCode final : public UCodeInterface
{
public:
  AESndUCode(DSPHLE* dsphle, u32 crc);

  void Initialize() override;
  void HandleMail(u32 mail) override;
  void Update() override;
  void DoState(PointerWrap& p) override;

  // June 5, 2010 version (padded to 0x03e0 bytes) - initial release
  // First included with libogc 1.8.4 on October 3, 2010: https://devkitpro.org/viewtopic.php?t=2249
  // https://github.com/devkitPro/libogc/blob/b5fdbdb069c45584aa4dfd950a136a8db9b1144c/libaesnd/dspcode/dspmixer.s
  static constexpr u32 HASH_2010 = 0x008366af;
  // April 11, 2012 version (padded to 0x03e0 bytes) - swapped input channels
  // First included with libogc 1.8.11 on April 22, 2012: https://devkitpro.org/viewtopic.php?t=3094
  // https://github.com/devkitPro/libogc/commit/8f188e12b6a3d8b5a0d49a109fe6a3e4e1702aab
  static constexpr u32 HASH_2012 = 0x078066ab;
  // June 14, 2020 version (0x03e6 bytes) - added unsigned formats
  // First included with libogc 2.1.0 on June 15, 2020: https://devkitpro.org/viewtopic.php?t=9079
  // https://github.com/devkitPro/libogc/commit/eac8fe2c29aa790d552dd6166a1fb195dfdcb825
  static constexpr u32 HASH_2020 = 0x84c680a9;

private:
  void DMAInParameterBlock();
  void DMAOutParameterBlock();
  void SetUpAccelerator(u16 format, u16 gain);
  void DoMixing();

  // Copied from libaesnd/aesndlib.c's aesndpb_t (specifically the first 64 bytes)
#pragma pack(1)
  struct ParameterBlock
  {
    u32 out_buf;

    u32 buf_start;
    u32 buf_end;
    u32 buf_curr;

    u16 yn1;
    u16 yn2;
    u16 pds;

    // Note: only u16-aligned, not u32-aligned.
    // libogc's version has separate u16 freq_l and freq_h fields, but we use #pragma pack(1).
    u32 freq;
    u16 counter;

    s16 left;
    s16 right;
    u16 volume_l;
    u16 volume_r;

    u32 delay;

    u32 flags;
    u8 _pad[20];
  };
#pragma pack()
  static_assert(sizeof(ParameterBlock) == sizeof(u16) * 0x20);

  bool m_next_mail_is_parameter_block_addr = false;
  u32 m_parameter_block_addr = 0;

  ParameterBlock m_parameter_block{};

  // Number of 16-bit stereo samples in the output buffer: 2ms of sample data
  static constexpr u32 NUM_OUTPUT_SAMPLES = 96;

  std::array<s16, NUM_OUTPUT_SAMPLES * 2> m_output_buffer{};
};
}  // namespace DSP::HLE
