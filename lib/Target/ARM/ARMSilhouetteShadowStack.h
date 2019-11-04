//===- ARMSilhouetteShadowStack - Modify Prologue/Epilogue for Shadow Stack ==//
//
//         Protecting Control Flow of Real-time OS applications
//
// This file was written by at the University of Rochester.
// All Rights Reserved.
//
//===----------------------------------------------------------------------===//
//
// This pass instruments the function prologue/epilogue to save/load the return
// address from a parallel shadow stack.
//
//===----------------------------------------------------------------------===//
//

#include "ARMSilhouetteInstrumentor.h"
#include "llvm/CodeGen/MachineFunctionPass.h"

namespace llvm {

  struct ARMSilhouetteShadowStack
      : public MachineFunctionPass, ARMSilhouetteInstrumentor {
    // pass identifier variable
    static char ID;

    ARMSilhouetteShadowStack();

    virtual StringRef getPassName() const override;

    virtual bool runOnMachineFunction(MachineFunction & MF) override;

  private:
    void setupShadowStack(MachineInstr & MI);
    void popFromShadowStack(MachineInstr & MI, MachineOperand & PCLR);
  };

  FunctionPass * createARMSilhouetteShadowStack(void);
}
