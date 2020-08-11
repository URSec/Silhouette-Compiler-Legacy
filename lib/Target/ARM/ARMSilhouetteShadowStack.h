//===- ARMSilhouetteShadowStack - Modify Prologue/Epilogue for Shadow Stack ==//
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
