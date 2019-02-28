//===-- ARMSilhouetteSTR2STRT - Store to Unprivileged Store convertion-----===//
//
//         Protecting Control Flow of Real-time OS applications
//
// This file was written by at the University of Rochester.
// All Rights Reserved.
//
//===----------------------------------------------------------------------===//
//
// This pass converts all normal STR instructions to the STRT family 
// instructions.
//
//===----------------------------------------------------------------------===//
//

#ifndef ARM_SILHOUETTE_STR2STRT
#define ARM_SILHOUETTE_STR2STRT

#include "llvm/CodeGen/MachineFunctionPass.h"

namespace llvm {
  struct ARMSilhouetteSTR2STRT : public MachineFunctionPass {
    // pass identifier variable
    static char ID;

    ARMSilhouetteSTR2STRT();

    virtual StringRef getPassName() const override;

    virtual bool runOnMachineFunction(MachineFunction &MF) override;
  };

  FunctionPass *createARMSilhouetteSTR2STRT(void);
}

#endif
