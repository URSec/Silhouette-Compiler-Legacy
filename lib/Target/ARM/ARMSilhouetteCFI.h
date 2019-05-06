//===-- ARMSilhouetteCFI - Minimal Forward Control-Flow Integrity ---------===//
//
//         Protecting Control Flow of Real-time OS applications
//
// This file was written by at the University of Rochester.
// All Rights Reserved.
//
//===----------------------------------------------------------------------===//
//
// This pass implements the minimal control-flow integrity for forward indirect
// control-flow transfer instructions on ARM.
//
//===----------------------------------------------------------------------===//
//

#ifndef ARM_SILHOUETTE_CFI
#define ARM_SILHOUETTE_CFI

#include "llvm/CodeGen/MachineFunctionPass.h"

namespace llvm {
  struct ARMSilhouetteCFI : public MachineFunctionPass {
    // pass identifier variable
    static char ID;

    ARMSilhouetteCFI();

    virtual StringRef getPassName() const override;

    virtual bool runOnMachineFunction(MachineFunction & MF) override;
  };

  FunctionPass * createARMSilhouetteCFI(void);
}

#endif
