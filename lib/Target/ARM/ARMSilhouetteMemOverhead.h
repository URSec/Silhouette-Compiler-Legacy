//===-- ARMSilhouetteMemOverhead - Estimate Memory Overhead of Silhouette -===//
//
//         Protecting Control Flow of Real-time OS applications
//
// This file was written by at the University of Rochester.
// All Rights Reserved.
//
//===----------------------------------------------------------------------===//
//
// This pass estimates the memory overhead of converting all normal loads and 
// store instructions to unprivileged loads and stores. It is an estimation 
// because we use mayLoad() and mayStore() to check if an instruction is a load 
// or a store; however, these two functions are not 100% accurate.
//
//===----------------------------------------------------------------------===//
//

#ifndef ARM_SILHOUETTE_MEM_OVERHEAD
#define ARM_SILHOUETTE_MEM_OVERHEAD

#include "llvm/CodeGen/MachineFunctionPass.h"

namespace llvm {
  struct ARMSilhouetteMemOverhead : public MachineFunctionPass {
    // pass identifier variable
    static char ID;

    ARMSilhouetteMemOverhead();

    virtual StringRef getPassName() const override;

    virtual bool runOnMachineFunction(MachineFunction &MF) override;
  };

  FunctionPass *createARMSilhouetteMemOverhead(void);
}

#endif
