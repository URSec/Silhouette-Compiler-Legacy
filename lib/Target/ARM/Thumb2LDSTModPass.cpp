//=== Thumb2LDSTModPass.cpp - Modify Load/Store instructions to User Load/Store===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===--------------------------------------------------------------------------===//

#include "ARM.h"
#include "ARMMachineFunctionInfo.h"
#include "Thumb2InstrInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineInstrBundle.h"
using namespace llvm;

namespace {
  class Thumb2LDSTModPass : public MachineFunctionPass {
  public:
    static char ID;
    Thumb2LDSTModPass() : MachineFunctionPass(ID) {}

    bool restrictIT;
    const Thumb2InstrInfo *TII;
    const TargetRegisterInfo *TRI;
    MachineRegisterInfo *MRI;
    MachineFunction *MF;
    ARMFunctionInfo *AFI;

    bool runOnMachineFunction(MachineFunction &Fn) override;

    void getAnalysisUsage(AnalysisUsage &AU) const override{
      AU.addRequired<ARMLoadStoreOpt>();
    };

    const char *getPassName() const override {
      return "Load/Store modification pass";
    }
  }
}

bool Thumb2LDSTModPass::runOnMachineFunction(MachineFunction &Fn) {
  const ARMSubtarget &STI = 
      static_cast<const ARMSubtarget &>(Fn.getSubtarget());
  if (!STI.isThumb2())
    return false;
  AFI = Fn.getInfo<ARMFunctionInfo>();
  MF = &Fn;
  TII = STI->getInstrInfo();
  bool Modified = false;
  for (MachineFunction::iterator MFI = Fn.begin(), E = Fn.end(); MFI != E; 
       ++MFI){
    MachineBasicBlock &MBB = *MFI;
    // TODO: Refer to ARMLoadStoreOptimizer
  }
}
