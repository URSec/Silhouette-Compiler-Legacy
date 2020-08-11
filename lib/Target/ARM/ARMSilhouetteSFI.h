//===-- ARMSilhouetteSFI - Software Fault Isolation on stores -------------===//
//
//         Protecting Control Flow of Real-time OS applications
//              Copyright (c) 2019-2020, University of Rochester
//
// Part of the Silhouette Project, under the Apache License v2.0 with
// LLVM Exceptions.
// See LICENSE.txt in the top-level directory for license information.
//
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass applies bit-masking on addresses that the generated code stores
// into, either selectively (applying to only heavyweight stores) or entirely.
//
//===----------------------------------------------------------------------===//
//

#ifndef ARM_SILHOUETTE_SFI
#define ARM_SILHOUETTE_SFI

#include "ARMSilhouetteInstrumentor.h"
#include "llvm/CodeGen/MachineFunctionPass.h"

namespace llvm {

  enum SilhouetteSFIOption {
    // No SFI
    NoSFI,
    // Selective SFI
    SelSFI,
    // Full SFI
    FullSFI,
  };

  // The SFI masks for t2BICri
  static const uint32_t SFI_MASK  = 0xc0000000u;
  static const uint32_t SFI_MASK2 = 0x00800000u;

  struct ARMSilhouetteSFI
      : public MachineFunctionPass, ARMSilhouetteInstrumentor {
    // pass identifier variable
    static char ID;

    ARMSilhouetteSFI();

    virtual StringRef getPassName() const override;

    virtual bool runOnMachineFunction(MachineFunction & MF) override;
  };

  FunctionPass *createARMSilhouetteSFI(void);

}

#endif
