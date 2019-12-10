//===-- ARMSilhouetteSFI - Software Fault Isolation on stores -------------===//
//
//         Protecting Control Flow of Real-time OS applications
//
// This file was written by at the University of Rochester.
// All Rights Reserved.
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
