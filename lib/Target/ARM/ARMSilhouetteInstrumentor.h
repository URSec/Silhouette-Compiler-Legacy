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

#include "ARMBaseInstrInfo.h"
#include "llvm/CodeGen/LivePhysRegs.h"
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

  //
  // Function: addImmediateToRegister()
  //
  // Description:
  //   This function builds an ADD/SUB that adds an immediate to a register and
  //   puts the new instruction(s) at the end of a deque.
  //
  // Inputs:
  //   MI    - A reference to the instruction before which to insert new
  //           instructions.
  //   Reg   - The destination register.
  //   Imm   - The immediate to be added.
  //   Insts - A reference to a deque that contains new instructions.
  //
  static inline void
  addImmediateToRegister(MachineInstr & MI, unsigned Reg, int64_t Imm,
                         std::deque<MachineInstr *> & Insts) {
    assert((Imm > -4096 && Imm < 4096) && "Immediate too large!");
    assert((Reg != ARM::SP || Imm % 4 == 0) &&
            "Cannot add unaligned immediate to SP!");

    MachineFunction & MF = *MI.getMF();
    const TargetInstrInfo * TII = MF.getSubtarget().getInstrInfo();

    unsigned PredReg;
    ARMCC::CondCodes Pred = getInstrPredicate(MI, PredReg);

    unsigned addOpc = Imm < 0 ? ARM::t2SUBri12 : ARM::t2ADDri12;
    if (Reg == ARM::SP && Imm > -512 && Imm < 512) {
      addOpc = Imm < 0 ? ARM::tSUBspi : ARM::tADDspi;
      Imm >>= 2;
    }

    Insts.push_back(BuildMI(MF, MI.getDebugLoc(), TII->get(addOpc), Reg)
                    .addReg(Reg)
                    .addImm(Imm < 0 ? -Imm : Imm)
                    .add(predOps(Pred, PredReg)));
  }

  //
  // Function: subtractImmediateFromRegister()
  //
  // Description:
  //   This function builds a SUB/ADD that subtracts an immediate from a register
  //   and puts the new instruction(s) at the end of a deque.
  //
  // Inputs:
  //   MI    - A reference to the instruction before which to insert new
  //           instructions.
  //   Reg   - The destination register.
  //   Imm   - The immediate to be subtracted.
  //   Insts - A reference to a deque that contains new instructions.
  //
  static inline void
  subtractImmediateFromRegister(MachineInstr & MI, unsigned Reg, int64_t Imm,
                                std::deque<MachineInstr *> & Insts) {
    addImmediateToRegister(MI, Reg, -Imm, Insts);
  }

  //
  // Function: findFreeRegisters()
  //
  // Description:
  //   This function computes the liveness of ARM core registers before a given
  //   instruction MI and returns a list of free core registers that can be
  //   used for instrumentation purposes.
  //
  // Inputs:
  //   MI    - A reference to the instruction before which to find free
  //           registers.
  //   Thumb - Whether we are looking for Thumb registers (low registers, i,e,,
  //           R0 -- R7) or ARM registers (both low and high registers, i.e.,
  //           R0 -- R12 and LR).
  //
  // Return value:
  //   A deque of free registers (might be empty, if none is found).
  //
  static inline std::deque<unsigned>
  findFreeRegisters(const MachineInstr & MI, bool Thumb = false) {
    const MachineFunction & MF = *MI.getMF();
    const MachineBasicBlock & MBB = *MI.getParent();
    const MachineRegisterInfo & MRI = MF.getRegInfo();
    const TargetRegisterInfo * TRI = MF.getSubtarget().getRegisterInfo();
    LivePhysRegs UsedRegs(*TRI);

    // First add live-out registers of MBB; these registers are considered live
    // at the end of MBB
    UsedRegs.addLiveOuts(MBB);

    // Then move backward step by step to compute live registers before MI
    MachineBasicBlock::const_iterator MBBI(MI);
    MachineBasicBlock::const_iterator I = MBB.end();
    while (I != MBBI) {
      UsedRegs.stepBackward(*--I);
    }

    // Now add registers that are neither reserved nor live to a free list
    const auto LoGPRs = {
      ARM::R0, ARM::R1, ARM::R2, ARM::R3, ARM::R4, ARM::R5, ARM::R6, ARM::R7,
    };
    const auto HiGPRs = {
      ARM::R8, ARM::R9, ARM::R10, ARM::R11, ARM::R12, ARM::LR,
    };
    std::deque<unsigned> FreeRegs;
    for (unsigned Reg : LoGPRs) {
      if (!MRI.isReserved(Reg) && !UsedRegs.contains(Reg)) {
        FreeRegs.push_back(Reg);
      }
    }
    if (!Thumb) {
      for (unsigned Reg : HiGPRs) {
        if (!MRI.isReserved(Reg) && !UsedRegs.contains(Reg)) {
          FreeRegs.push_back(Reg);
        }
      }
    }

    return FreeRegs;
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
