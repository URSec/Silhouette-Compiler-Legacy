//===- ARMSilhouetteInstrumentor - A helper class for instrumentation -----===//
//
//         Protecting Control Flow of Real-time OS applications
//
// This file was written by at the University of Rochester.
// All Rights Reserved.
//
//===----------------------------------------------------------------------===//
//
// This file defines interfaces of the ARMSilhouetteInstrumentor class.
//
//===----------------------------------------------------------------------===//
//

#ifndef ARM_SILHOUETTE_INSTRUMENTOR
#define ARM_SILHOUETTE_INSTRUMENTOR

#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"

#include <deque>

namespace llvm {
  //====================================================================
  // Static inline functions.
  //====================================================================

  //
  // Function: getFunctionCodeSize()
  //
  // Description:
  //   This function computes the code size of a machine function.
  //
  // Input:
  //   MF - A reference to the target machine function.
  //
  // Return value:
  //   The size (in bytes) of the machine function.
  //
  static inline unsigned long getFunctionCodeSize(const MachineFunction & MF) {
    const TargetInstrInfo * TII = MF.getSubtarget().getInstrInfo();

    unsigned long CodeSize = 0ul;
    for (const MachineBasicBlock & MBB : MF) {
      for (const MachineInstr & MI : MBB) {
        CodeSize += TII->getInstSizeInBytes(MI);
      }
    }

    return CodeSize;
  }

  //====================================================================
  // Class ARMSilhouetteInstrumentor.
  //====================================================================

  struct ARMSilhouetteInstrumentor {
    void insertInstBefore(MachineInstr & MI, MachineInstr * Inst);

    void insertInstAfter(MachineInstr & MI, MachineInstr * Inst);

    void insertInstsBefore(MachineInstr & MI,
                           std::deque<MachineInstr *> & Insts);

    void insertInstsAfter(MachineInstr & MI,
                          std::deque<MachineInstr *> & Insts);

    void removeInst(MachineInstr & MI);

  private:
    unsigned getITBlockSize(const MachineInstr & IT);
    MachineInstr * findIT(MachineInstr & MI, unsigned & distance);
    const MachineInstr * findIT(const MachineInstr & MI, unsigned & distance);
    std::deque<bool> decodeITMask(unsigned Mask);
    unsigned encodeITMask(std::deque<bool> DQMask);
  };
}

#endif
