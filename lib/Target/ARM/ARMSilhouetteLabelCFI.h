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

#include "ARMSilhouetteInstrumentor.h"
#include "llvm/CodeGen/MachineFunctionPass.h"

namespace llvm {
  struct ARMSilhouetteLabelCFI
      : public MachineFunctionPass, ARMSilhouetteInstrumentor {
    // pass identifier variable
    static char ID;

    // The constant CFI label for indirect calls (encoding of "movs r3, r3")
    static const uint16_t CFI_LABEL_CALL = 0x001b;
    // The constant CFI label for indirect jumps (encoding of "mov r0, r0")
    static const uint16_t CFI_LABEL_JMP = 0x4600;

    ARMSilhouetteLabelCFI();

    virtual StringRef getPassName() const override;

    virtual bool runOnMachineFunction(MachineFunction & MF) override;

  private:
    void insertCFILabelForCall(MachineFunction & MF);
    void insertCFILabelForJump(MachineBasicBlock & MBB);
    void insertCFICheckForCall(MachineInstr & MI, unsigned Reg);
    void insertCFICheckForJump(MachineInstr & MI, unsigned Reg);
    void insertCFICheck(MachineInstr & MI, unsigned Reg, uint16_t Label);
  };

  FunctionPass * createARMSilhouetteLabelCFI(void);
}

#endif
