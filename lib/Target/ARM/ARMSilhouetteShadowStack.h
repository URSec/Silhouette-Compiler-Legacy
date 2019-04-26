//===- ARMSilhouetteShadowStack - Modify Prolog and Epilog for Shadow Stack --===//
//
//         Protecting Control Flow of Real-time OS applications
//
// This file was written by at the University of Rochester.
// All Rights Reserved.
//
//===----------------------------------------------------------------------===//
//
// This pass adds STR imm to store LR to shadow stack and changes 
// popping LR to loading LR from shadow stack 
// instructions.
//
//===----------------------------------------------------------------------===//
//


#include "llvm/CodeGen/MachineFunctionPass.h"
namespace llvm {
  struct ARMSilhouetteShadowStack : public MachineFunctionPass {
    // pass identifier variable
    static char ID;

    ARMSilhouetteShadowStack();

    virtual StringRef getPassName() const override;

    virtual bool runOnMachineFunction(MachineFunction &MF) override;
  };

  FunctionPass *createARMSilhouetteShadowStack(void);
}

