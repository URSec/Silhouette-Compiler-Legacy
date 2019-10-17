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

#include <deque>

namespace llvm {
  struct ARMSilhouetteInstrumentor {
    unsigned long getFunctionCodeSize(const MachineFunction & MF);

    unsigned getITBlockSize(const MachineInstr & IT);

    MachineInstr * findIT(MachineInstr & MI, unsigned & distance);

    const MachineInstr * findIT(const MachineInstr & MI, unsigned & distance);

    void insertInstBefore(MachineInstr & MI, MachineInstr * Inst);

    void insertInstAfter(MachineInstr & MI, MachineInstr * Inst);

    void insertInstsBefore(MachineInstr & MI,
                           std::deque<MachineInstr *> & Insts);

    void insertInstsAfter(MachineInstr & MI,
                          std::deque<MachineInstr *> & Insts);

    void removeInst(MachineInstr & MI);

  private:
    std::deque<bool> decodeITMask(unsigned Mask);
    unsigned encodeITMask(std::deque<bool> DQMask);
  };
}

#endif
