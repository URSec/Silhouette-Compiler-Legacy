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

#include "ARMSilhouetteMemOverhead.h"
#include "ARMTargetMachine.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

char ARMSilhouetteMemOverhead::ID = 0;

ARMSilhouetteMemOverhead::ARMSilhouetteMemOverhead() : MachineFunctionPass(ID) {
  return;
}

const char *ARMSilhouetteMemOverhead::getPassName() const {
  return "ARM Silhouette Memory Overhead Estimation Pass";
}

//
// Method: runOnMachineFunction()
//
// Description:
//   This method is called when the PassManager wants this pass to transform
//   the specified MachineFunction.
//   This method computes the memory usage of all MachineInstr and estimate the 
//   memory overhead incurred by unprivileged loads and stores.
//
// Inputs:
//   MF - A reference to the MachineFunction to transform.
//
// Outputs:
//   MF - The transformed MachineFunction.
//
// Return value:
//   true - The MachineFunction was transformed.
//   false - The MachineFunction was not transformed.
//
bool ARMSilhouetteMemOverhead::runOnMachineFunction(MachineFunction &MF) {
  
  return false;
}


//
// Create a new pass.
namespace llvm {
  FunctionPass *createARMSilhouetteMemOverhead(void) {
    return new ARMSilhouetteMemOverhead();
  }
}
