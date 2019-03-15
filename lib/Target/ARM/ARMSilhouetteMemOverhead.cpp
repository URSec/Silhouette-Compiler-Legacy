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
// or a store; however, these two functions are not 100% accurate as it'd report 
// some non-load/store instructions.
//
//===----------------------------------------------------------------------===//
//

#include "ARMSilhouetteMemOverhead.h"
#include "ARMTargetMachine.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/Support/raw_ostream.h"

#define BYTE_TO_MB 1024
#define DEBUG_TYPE "arm-silhouette-mem-overhead"

using namespace llvm;
using namespace std;

// memory overhead stats
STATISTIC(MEM_OVERHEAD, "Memory overhead in Bytes");
STATISTIC(MEM_OP_SIZE, "Memory operation instruction size");
STATISTIC(CODE_SIZE, "Original code size in Bytes");

char ARMSilhouetteMemOverhead::ID = 0;

ARMSilhouetteMemOverhead::ARMSilhouetteMemOverhead() : MachineFunctionPass(ID) {
  return;
}

StringRef ARMSilhouetteMemOverhead::getPassName() const {
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
  // This variable is used to call method getInstSizeInBytes().
  const ARMBaseInstrInfo *ABII = MF.getSubtarget<ARMSubtarget>().getInstrInfo();

  unsigned long totalInstrSize = 0;
  unsigned long memOpSize = 0;  // all load and store instruction size
  unsigned int memIncreased = 0;

  for (MachineBasicBlock &MBB : MF) {
    for (MachineInstr &MI : MBB) {
      unsigned opcode = MI.getOpcode();
      int64_t imm = 0;

      int instrSize = ABII->getInstSizeInBytes(MI);
      totalInstrSize += instrSize;
      CODE_SIZE += instrSize;

      if (MI.mayStore() || MI.mayLoad()) {
        MEM_OP_SIZE += instrSize;
        switch(opcode) {
          // stores immediate; A7.7.158 STR(immediate)
          case ARM::tSTRi:    // Encoding T1: STR<c> <Rt>, [<Rn>{,#<imm5>}]
          case ARM::tSTRspi:  // Encoding T2: STR<c> <Rt>, [SP, #<imm8>]
          case ARM::t2STRi12: // Encoding T3: STR<c>.W <Rt>,[<Rn>,#<imm12>]
            memOpSize += 2;
            imm = MI.getOperand(2).getImm();
            if (imm < 0) {
              // Add an extra add and a sub
              memIncreased += 6;
            } else {
              memIncreased += 2;
            }
            break;
          
          // indexed stores
          case ARM::t2STR_PRE: // pre-index store
          case ARM::t2STR_POST: // post-index store
            // We need an extra add or sub to update the base register.
            memIncreased += 2;
            break;

          // STR(register); A7.7.159
          case ARM::tSTRr: // Encoding T1: STR<c> <Rt>,[<Rn>,<Rm>]
            // Update the base register first, and then restore.
            memIncreased += 4;
            break;
        }
      }
    }
  }

  MEM_OVERHEAD += memIncreased;

  errs() << "Function " << MF.getName() << ":\n";
  errs() << "Total code size  = " << totalInstrSize << " bytes.\n";
  errs() << "Increase code size = " << memIncreased << " bytes (" << 
    format("%.2f", (float(memIncreased)) / totalInstrSize * 100) << "%).\n";

  // This pass doesn't change the code.
  return false;
}


//
// Create a new pass.
namespace llvm {
  FunctionPass *createARMSilhouetteMemOverhead(void) {
    return new ARMSilhouetteMemOverhead();
  }
}
