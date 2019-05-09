//===-- ARMSilhouetteLabelCFI - Label-Based Forward Control-Flow Integrity ===//
//
//         Protecting Control Flow of Real-time OS applications
//
// This file was written by at the University of Rochester.
// All Rights Reserved.
//
//===----------------------------------------------------------------------===//
//
// This pass implements the label-based single-label control-flow integrity for
// forward indirect control-flow transfer instructions on ARM.
//
//===----------------------------------------------------------------------===//
//

#ifndef ARM_SILHOUETTE_LABEL_CFI
#define ARM_SILHOUETTE_LABEL_CFI

#include "llvm/CodeGen/MachineFunctionPass.h"

namespace llvm {
  struct ARMSilhouetteLabelCFI : public MachineFunctionPass {
    // pass identifier variable
    static char ID;

    // The constant CFI label (encoding of "mov r0, r0")
    static const uint16_t CFI_LABEL = 0x4600;

    ARMSilhouetteLabelCFI();

    virtual StringRef getPassName() const override;

    virtual bool runOnMachineFunction(MachineFunction & MF) override;

  private:
    void insertCFILabel(MachineFunction & MF);
    void insertCFILabel(MachineBasicBlock & MBB);
    void insertCFICheck(MachineInstr & MI, unsigned Reg);
  };

  FunctionPass * createARMSilhouetteLabelCFI(void);
}

#endif
