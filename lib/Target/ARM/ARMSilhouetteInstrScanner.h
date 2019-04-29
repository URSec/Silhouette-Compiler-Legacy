//===- ARMSilhouetteInstrScanner - Scan Privileged Instructions in the Application code --===//
//
//         Protecting Control Flow of Real-time OS applications
//
// This file was written by at the University of Rochester.
// All Rights Reserved.
//
//===----------------------------------------------------------------------===//
//
// This pass scans a 'blacklist' of privileged instruction that are not supposed
// to be executed in an application's code. If any found, the application is 
// considered unsafe and will be reported by the compiler.
//
//===----------------------------------------------------------------------===//
//


#include "llvm/CodeGen/MachineFunctionPass.h"
namespace llvm {
  struct ARMSilhouetteInstrScanner : public MachineFunctionPass {
    // pass identifier variable
    static char ID;

    ARMSilhouetteInstrScanner();

    virtual StringRef getPassName() const override;

    virtual bool runOnMachineFunction(MachineFunction &MF) override;
  };

  FunctionPass *createARMSilhouetteInstrScanner(void);
}

