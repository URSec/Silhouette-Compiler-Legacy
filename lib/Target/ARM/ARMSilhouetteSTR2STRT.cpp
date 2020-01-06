//===-- ARMSilhouetteSTR2STRT - Store to Unprivileged Store convertion-----===//
//
//         Protecting Control Flow of Real-time OS applications
//
// This file was written by at the University of Rochester.
// All Rights Reserved.
//
//===----------------------------------------------------------------------===//
//
// This pass converts all regular store instructions to the unprivileged store
// instructions.
//
//===----------------------------------------------------------------------===//
//

#include "ARM.h"
#include "ARMSilhouetteConvertFuncList.h"
#include "ARMSilhouetteSFI.h"
#include "ARMSilhouetteSTR2STRT.h"
#include "ARMTargetMachine.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/FileSystem.h"

#include <deque>

using namespace llvm;

extern SilhouetteSFIOption SilhouetteSFI;

char ARMSilhouetteSTR2STRT::ID = 0;

static DebugLoc DL;

ARMSilhouetteSTR2STRT::ARMSilhouetteSTR2STRT()
    : MachineFunctionPass(ID) {
}

StringRef
ARMSilhouetteSTR2STRT::getPassName() const {
  return "ARM Silhouette Store Promotion Pass";
}

//
// Function: backupRegisters()
//
// Description:
//   This function backs up at most 2 core registers onto the stack and puts
//   the new instruction(s) at the end of a deque.  Either register or both
//   (which does't quite make sense, though) can be left out by passing
//   ARM::NoRegister.  There is no ordering requirement between the two
//   registers.
//
// Inputs:
//   MI    - A reference to the store instruction before which to insert new
//           instructions.
//   Reg1  - The first register to back up.
//   Reg2  - The second register to back up.
//   Insts - A reference to a deque that contains new instructions.
//
static void
backupRegisters(MachineInstr & MI, unsigned Reg1, unsigned Reg2,
                std::deque<MachineInstr *> & Insts) {
  MachineFunction & MF = *MI.getMF();
  const TargetInstrInfo * TII = MF.getSubtarget().getInstrInfo();

  unsigned PredReg;
  ARMCC::CondCodes Pred = getInstrPredicate(MI, PredReg);

  unsigned offset = 0;
  unsigned numRegs = 0;
  if (Reg1 != ARM::NoRegister) {
    ++numRegs;
  }
  if (Reg2 != ARM::NoRegister) {
    ++numRegs;
  }

  //
  // Build the following instruction sequence:
  //
  // sub  sp, #offset
  // strt reg1, [sp, #0]
  // strt reg2, [sp, #4]
  //
  if (numRegs != 0) {
    Insts.push_back(BuildMI(MF, DL, TII->get(ARM::tSUBspi), ARM::SP)
                    .addReg(ARM::SP)
                    .addImm(numRegs)
                    .add(predOps(Pred, PredReg)));
    if (Reg1 != ARM::NoRegister) {
      Insts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRT))
                      .addReg(Reg1)
                      .addReg(ARM::SP)
                      .addImm(offset));
      offset += 4;
    }
    if (Reg2 != ARM::NoRegister) {
      Insts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRT))
                      .addReg(Reg2)
                      .addReg(ARM::SP)
                      .addImm(offset));
      offset += 4;
    }
  }
}

//
// Function: restoreRegisters()
//
// Description:
//   This function restores at most 2 core registers from the stack and puts
//   the new instruction(s) at the end of a deque.  Either register or both
//   (which does't quite make sense, though) can be left out by passing
//   ARM::NoRegister.  The two registers should be lo registers (R0 - R7), and
//   the first one should be stricly smaller than the second one.
//
// Inputs:
//   MI    - A reference to the store instruction before which to insert new
//           instructions.
//   Reg1  - The first register to restore.
//   Reg2  - The second register to restore.
//   Insts - A reference to a deque that contains new instructions.
//
static void
restoreRegisters(MachineInstr & MI, unsigned Reg1, unsigned Reg2,
                 std::deque<MachineInstr *> & Insts) {
  assert(((Reg1 >= ARM::R0 && Reg1 < ARM::R8) || Reg1 == ARM::NoRegister) &&
         "Cannot restore a hi register using T1 POP!");
  assert(((Reg2 >= ARM::R0 && Reg2 < ARM::R8) || Reg2 == ARM::NoRegister) &&
         "Cannot restore a hi register using T1 POP!");
  if (Reg1 != ARM::NoRegister && Reg2 != ARM::NoRegister) {
    assert(Reg1 < Reg2 && "Invalid register order for T1 POP!");
  }

  MachineFunction & MF = *MI.getMF();
  const TargetInstrInfo * TII = MF.getSubtarget().getInstrInfo();

  unsigned PredReg;
  ARMCC::CondCodes Pred = getInstrPredicate(MI, PredReg);

  // Build a POP that pops out the register content from stack
  if (Reg1 != ARM::NoRegister || Reg2 != ARM::NoRegister) {
    MachineInstrBuilder MIB = BuildMI(MF, DL, TII->get(ARM::tPOP))
                              .add(predOps(Pred, PredReg));
    if (Reg1 != ARM::NoRegister) {
      MIB.addReg(Reg1);
    }
    if (Reg2 != ARM::NoRegister) {
      MIB.addReg(Reg2);
    }
    Insts.push_back(MIB.getInstr());
  }
}

//
// Function: handleSPWithUncommonImm()
//
// Description:
//   This function takes care of cases where the base register of a store is SP
//   and the immediate offset is not aligned by 4 or greater than 255.
//
// Inputs:
//   MI       - A reference to a store instruction before which to insert new
//              instructions.
//   SrcReg   - The source register of the store.
//   Imm      - The immediate offset of the store.
//   strOpc   - The opcode of the new unprivileged store.
//   Insts    - A reference to a deque that contains new instructions.
//   FreeRegs - A reference to a deque that contains free registers before MI.
//   SrcReg2  - The second register of the store in case this is a double word
//              store.
//
static void
handleSPWithUncommonImm(MachineInstr & MI, unsigned SrcReg, int64_t Imm,
                        unsigned strOpc, std::deque<MachineInstr *> & Insts,
                        std::deque<unsigned> & FreeRegs,
                        unsigned SrcReg2 = ARM::NoRegister) {
  MachineFunction & MF = *MI.getMF();
  const TargetInstrInfo * TII = MF.getSubtarget().getInstrInfo();

  unsigned PredReg;
  ARMCC::CondCodes Pred = getInstrPredicate(MI, PredReg);

  // First try to find a free register
  unsigned ScratchReg = ARM::NoRegister;
  bool needSpill = true;
  for (unsigned Reg : FreeRegs) {
    if (Reg != SrcReg && Reg != SrcReg2) {
      ScratchReg = Reg;
      needSpill = false;
      break;
    }
  }

  if (needSpill) {
    errs() << "[SP] Unable to find a free register for SP in " << MI;
    // Find a register to spill
    ScratchReg = ARM::R4;
    while (ScratchReg == SrcReg || ScratchReg == SrcReg2) ScratchReg++;
    backupRegisters(MI, ScratchReg, ARM::NoRegister, Insts);
    Imm += 4; // Compensate for SP decrement
  }

  // Add SP with the uncommon immediate to the scratch register
  unsigned addOpc = Imm < 0 ? ARM::t2SUBri12 : ARM::t2ADDri12;
  Insts.push_back(BuildMI(MF, DL, TII->get(addOpc), ScratchReg)
                  .addReg(ARM::SP)
                  .addImm(Imm < 0 ? -Imm : Imm)
                  .add(predOps(Pred, PredReg)));

  // Do store
  Insts.push_back(BuildMI(MF, DL, TII->get(strOpc))
                  .addReg(SrcReg)
                  .addReg(ScratchReg)
                  .addImm(0));
  if (SrcReg2 != ARM::NoRegister) {
    Insts.push_back(BuildMI(MF, DL, TII->get(strOpc))
                    .addReg(SrcReg2)
                    .addReg(ScratchReg)
                    .addImm(4));
  }

  if (needSpill) {
    // Restore the scratch register from the stack
    restoreRegisters(MI, ScratchReg, ARM::NoRegister, Insts);
  }
}

//
// Function: handleSPWithOffsetReg()
//
// Description:
//   This function handles the case where the base register of a Store Register
//   instruction is SP.  In this case, we cannot put a pair of ADD/SUB around
//   the store because a hardware interrupt would corrupt the stack if it
//   happens right after the ADD.
//
// Inputs:
//   MI        - A reference to a store instruction before which to insert new
//               instructions.
//   SrcReg    - The source register of the store.
//   OffserReg - The offset register of the store.
//   ShiftImm  - The left shift immediate of the store.
//   strOpc    - The opcode of the new unprivileged store.
//   Insts     - A reference to a deque that contains new instructions.
//   FreeRegs  - A reference to a deque that contains free registers before MI.
//
static void
handleSPWithOffsetReg(MachineInstr & MI, unsigned SrcReg, unsigned OffsetReg,
                      unsigned ShiftImm, unsigned strOpc,
                      std::deque<MachineInstr *> & Insts,
                      std::deque<unsigned> & FreeRegs) {
  MachineFunction & MF = *MI.getMF();
  const TargetInstrInfo * TII = MF.getSubtarget().getInstrInfo();

  unsigned PredReg;
  ARMCC::CondCodes Pred = getInstrPredicate(MI, PredReg);

  // First try to find a free register
  unsigned ScratchReg = ARM::NoRegister;
  bool needSpill = true;
  for (unsigned Reg : FreeRegs) {
    if (Reg != SrcReg && Reg != OffsetReg) {
      ScratchReg = Reg;
      needSpill = false;
      break;
    }
  }

  if (needSpill) {
    errs() << "[SP] Unable to find a free register for SP in " << MI;
    // Save a scratch register onto the stack.
    ScratchReg = ARM::R0;
    while (ScratchReg == SrcReg || ScratchReg == OffsetReg) ScratchReg++;
    backupRegisters(MI, ScratchReg, ARM::NoRegister, Insts);
  }

  // Add SP and the offset register to the scratch register.
  if (ShiftImm > 0) {
    Insts.push_back(BuildMI(MF, DL, TII->get(ARM::t2ADDrs), ScratchReg)
                    .addReg(ARM::SP)
                    .addReg(OffsetReg)
                    .addImm(ShiftImm)
                    .add(predOps(Pred, PredReg))
                    .add(condCodeOp()));
  } else {
    Insts.push_back(BuildMI(MF, DL, TII->get(ARM::t2ADDrr), ScratchReg)
                    .addReg(ARM::SP)
                    .addReg(OffsetReg)
                    .add(predOps(Pred, PredReg))
                    .add(condCodeOp()));
  }

  // If we spiiled a register, we have to compensate the SP decrement by an ADD
  if (needSpill) {
    Insts.push_back(BuildMI(MF, DL, TII->get(ARM::t2ADDri12), ScratchReg)
                    .addReg(ScratchReg)
                    .addImm(4)
                    .add(predOps(Pred, PredReg)));
  }

  // Do the store
  Insts.push_back(BuildMI(MF, DL, TII->get(strOpc))
                  .addReg(SrcReg)
                  .addReg(ScratchReg)
                  .addImm(0));

  // Restore the scratch register from the stack if we spilled it
  if (needSpill) {
    restoreRegisters(MI, ScratchReg, ARM::NoRegister, Insts);
  }
}

//
// Method: runOnMachineFunction()
//
// Description:
//   This method is called when the PassManager wants this pass to transform
//   the specified MachineFunction.  This method deletes all the regular store
//   instructions and inserts unprivileged store instructions.
//
// Inputs:
//   MF - A reference to the MachineFunction to transform.
//
// Outputs:
//   MF - The transformed MachineFunction.
//
// Return value:
//   true  - The MachineFunction was transformed.
//   false - The MachineFunction was not transformed.
//
bool
ARMSilhouetteSTR2STRT::runOnMachineFunction(MachineFunction & MF) {
#if 1
  // Skip certain functions
  if (funcBlacklist.find(MF.getName()) != funcBlacklist.end()) {
    return false;
  }
  // Skip privileged functions in FreeRTOS
  if (MF.getFunction().getSection().equals("privileged_functions")){
    errs() << "Privileged function! skipped\n";
    return false;
  }
#endif

  const TargetInstrInfo * TII = MF.getSubtarget().getInstrInfo();

  unsigned long OldCodeSize = getFunctionCodeSize(MF);

  // Iterate over all machine instructions to find stores
  std::deque<MachineInstr *> Stores;
  for (MachineBasicBlock & MBB : MF) {
    for (MachineInstr & MI : MBB) {
      if (!MI.mayStore() || MI.getFlag(MachineInstr::ShadowStack)) {
        continue;
      }

      switch (MI.getOpcode()) {
      // Store word immediate
      case ARM::tSTRi:       // A7.7.158 Encoding T1
      case ARM::tSTRspi:     // A7.7.158 Encoding T2
      case ARM::t2STRi12:    // A7.7.158 Encoding T3
      case ARM::t2STRi8:     // A7.7.158 Encoding T4; no write-back
      // Store halfword immediate
      case ARM::tSTRHi:      // A7.7.167 Encoding T1
      case ARM::t2STRHi12:   // A7.7.167 Encoding T2
      case ARM::t2STRHi8:    // A7.7.167 Encoding T3; no write-back
      // Store byte immediate
      case ARM::tSTRBi:      // A7.7.160 Encoding T1
      case ARM::t2STRBi12:   // A7.7.160 Encoding T2
      case ARM::t2STRBi8:    // A7.7.160 Encoding T3; no write-back
      // Store word with write-back
      case ARM::t2STR_PRE:   // A7.7.158 Encoding T4; pre-indexed
      case ARM::t2STR_POST:  // A7.7.158 Encoding T4; post-indexed
      // Store halfword with write-back
      case ARM::t2STRH_PRE:  // A7.7.167 Encoding T3; pre-indexed
      case ARM::t2STRH_POST: // A7.7.167 Encoding T3; post-indexed
      // Store byte with write-back
      case ARM::t2STRB_PRE:  // A7.7.160 Encoding T3; pre-indexed
      case ARM::t2STRB_POST: // A7.7.160 Encoding T3; post-indexed
      // Store word register
      case ARM::tSTRr:       // A7.7.159 Encoding T1
      case ARM::t2STRs:      // A7.7.159 Encoding T2
      // Store halfword register
      case ARM::tSTRHr:      // A7.7.168 Encoding T1
      case ARM::t2STRHs:     // A7.7.168 Encoding T2
      // Store byte register
      case ARM::tSTRBr:      // A7.7.161 Encoding T1
      case ARM::t2STRBs:     // A7.7.161 Encoding T2
      // Store dual
      case ARM::t2STRDi8:    // A7.7.163 Encoding T1; no write-back
      case ARM::t2STRD_PRE:  // A7.7.163 Encoding T1; pre-indexed
      case ARM::t2STRD_POST: // A7.7.163 Encoding T1; post-indexed
        // Lightweight stores; leave them as is only if we are using full SFI
        if (SilhouetteSFI != FullSFI) {
          Stores.push_back(&MI);
        }
        break;

      // Floating-point store
      case ARM::VSTRD:       // A7.7.256 Encoding T1
      case ARM::VSTRS:       // A7.7.256 Encoding T2
      // Store multiple
      case ARM::tSTMIA_UPD:  // A7.7.156 Encoding T1
      case ARM::t2STMIA:     // A7.7.156 Encoding T2; no write-back
      case ARM::t2STMIA_UPD: // A7.7.156 Encoding T2; with write-back
      case ARM::t2STMDB:     // A7.7.157 Encoding T1; no write-back
      case ARM::t2STMDB_UPD: // A7.7.157 Encoding T1; with write-back
      // Push
      case ARM::tPUSH:       // A7.7.99 Encoding T1
      // Floating-point store multiple
      case ARM::VSTMDIA:     // A7.7.255 Encoding T1; increment after; no write-back
      case ARM::VSTMDIA_UPD: // A7.7.255 Encoding T1; increment after; with write-back
      case ARM::VSTMDDB_UPD: // A7.7.255 Encoding T1; decrement before; with write-back
      case ARM::VSTMSIA:     // A7.7.255 Encoding T2; increment after; no write-back
      case ARM::VSTMSIA_UPD: // A7.7.255 Encoding T2; increment after; with write-back
      case ARM::VSTMSDB_UPD: // A7.7.255 Encoding T2; decrement before; with write-back
        // Heavyweight stores; instrument them only if we are not using SFI
        if (SilhouetteSFI == NoSFI) {
          Stores.push_back(&MI);
        }
        break;

      case ARM::INLINEASM:
        break;

      default:
        errs() << "[SP] Unidentified store: " << MI;
        break;
      }
    }
  }

  // Instrument each different type of stores
  for (MachineInstr * Store : Stores) {
    MachineInstr & MI = *Store;

    unsigned PredReg;
    ARMCC::CondCodes Pred = getInstrPredicate(MI, PredReg);

    unsigned BaseReg, OffsetReg;
    unsigned SrcReg, SrcReg2;
    unsigned ScratchReg, ScratchReg2;
    int64_t Imm, Imm2;
    std::deque<unsigned> RegList;
    std::deque<unsigned> FreeRegs;

    std::deque<MachineInstr *> NewInsts;
    switch (MI.getOpcode()) {
    //================================================================
    // Store word immediate.
    //================================================================

    // A7.7.158 Encoding T1: STR<c> <Rt>,[<Rn>{,#<imm5>}]
    case ARM::tSTRi:
      SrcReg = MI.getOperand(0).getReg();
      BaseReg = MI.getOperand(1).getReg();
      Imm = MI.getOperand(2).getImm() << 2; // Not ZeroExtend(imm5:'00', 32) yet
      // imm5:'00' is small enough to be encoded in STRT
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRT))
                         .addReg(SrcReg)
                         .addReg(BaseReg)
                         .addImm(Imm));
      break;

    // A7.7.158 Encoding T2: STR<c> <Rt>,[SP,#<imm8>]
    case ARM::tSTRspi:
      SrcReg = MI.getOperand(0).getReg();
      BaseReg = MI.getOperand(1).getReg();
      Imm = MI.getOperand(2).getImm() << 2; // Not ZeroExtend(imm8:'00', 32) yet
      // imm8:'00' might go beyond 255; we surround STRT with ADD/SUB
      if (Imm > 255) {
        FreeRegs = findFreeRegisters(MI);
        handleSPWithUncommonImm(MI, SrcReg, Imm, ARM::t2STRT, NewInsts,
                                FreeRegs);
        break;
      }
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRT))
                         .addReg(SrcReg)
                         .addReg(BaseReg)
                         .addImm(Imm));
      break;

    // A7.7.158 Encoding T3: STR<c>.W <Rt>,[<Rn>,#<imm12>]
    case ARM::t2STRi12:
      SrcReg = MI.getOperand(0).getReg();
      BaseReg = MI.getOperand(1).getReg();
      Imm = MI.getOperand(2).getImm();
      // imm12 might go beyond 255.
      if (BaseReg == ARM::SP && Imm > 255) {
        FreeRegs = findFreeRegisters(MI);
        handleSPWithUncommonImm(MI, SrcReg, Imm, ARM::t2STRT, NewInsts,
                                FreeRegs);
        break;
      }
      if (Imm > 255) {
        addImmediateToRegister(MI, BaseReg, Imm, NewInsts);
      }
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRT))
                         .addReg(SrcReg)
                         .addReg(BaseReg)
                         .addImm(Imm > 255 ? 0 : Imm));
      if (Imm > 255) {
        subtractImmediateFromRegister(MI, BaseReg, Imm, NewInsts);
      }
      break;

    // A7.7.158 Encoding T4: STR<c> <Rt>,[<Rn>,#-<imm8>]
    case ARM::t2STRi8:
      SrcReg = MI.getOperand(0).getReg();
      BaseReg = MI.getOperand(1).getReg();
      Imm = MI.getOperand(2).getImm();
      if (BaseReg == ARM::SP && Imm % 4 != 0) {
        // This case shouldn't happen as this store stores a word.
        // What we do here is a "just in case".
        FreeRegs = findFreeRegisters(MI);
        handleSPWithUncommonImm(MI, SrcReg, Imm, ARM::t2STRT, NewInsts,
                                FreeRegs);
        break;
      }
      // -imm8 might be 0 (-256 counting the 'U' bit), in which case we don't
      // build SUB/ADD
      if (Imm != -256) {
        addImmediateToRegister(MI, BaseReg, Imm, NewInsts);
      }
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRT))
                         .addReg(SrcReg)
                         .addReg(BaseReg)
                         .addImm(0));
      if (Imm != -256) {
        subtractImmediateFromRegister(MI, BaseReg, Imm, NewInsts);
      }
      break;

    //================================================================
    // Store halfword immediate.
    //================================================================

    // A7.7.167 Encoding T1: STRH<c> <Rt>,[<Rn>{,#<imm5>}]
    case ARM::tSTRHi:
      SrcReg = MI.getOperand(0).getReg();
      BaseReg = MI.getOperand(1).getReg();
      Imm = MI.getOperand(2).getImm() << 1; // Not ZeroExtend(imm5:'0', 32) yet
      // imm5:'0' is small enough to be encoded in STRHT
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRHT))
                         .addReg(SrcReg)
                         .addReg(BaseReg)
                         .addImm(Imm));
      break;

    // A7.7.167 Encoding T2: STRH<c>.W <Rt>,[<Rn>,#<imm12>]
    case ARM::t2STRHi12:
      SrcReg = MI.getOperand(0).getReg();
      BaseReg = MI.getOperand(1).getReg();
      Imm = MI.getOperand(2).getImm();
      // Special case.
      if (BaseReg == ARM::SP && Imm > 255) {
        FreeRegs = findFreeRegisters(MI);
        handleSPWithUncommonImm(MI, SrcReg, Imm, ARM::t2STRHT, NewInsts,
                                FreeRegs);
        break;
      }
      // imm12 might go beyond 255.
      if (Imm > 255) {
        addImmediateToRegister(MI, BaseReg, Imm, NewInsts);

      }
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRHT))
                         .addReg(SrcReg)
                         .addReg(BaseReg)
                         .addImm(Imm > 255 ? 0 : Imm));
      if (Imm > 255) {
        subtractImmediateFromRegister(MI, BaseReg, Imm, NewInsts);
      }
      break;

    // A7.7.167 Encoding T3: STRH<c> <Rt>,[<Rn>,#-<imm8>]
    case ARM::t2STRHi8:
      SrcReg = MI.getOperand(0).getReg();
      BaseReg = MI.getOperand(1).getReg();
      Imm = MI.getOperand(2).getImm();
      // SP has to be 4 byte aligned; if the easy ways won't apply,
      // special-case it
      if (BaseReg == ARM::SP && Imm % 4 != 0) {
        FreeRegs = findFreeRegisters(MI);
        handleSPWithUncommonImm(MI, SrcReg, Imm, ARM::t2STRHT, NewInsts,
                                FreeRegs);
        break;
      }
      // -imm8 might be 0 (-256 counting the 'U' bit), in which case we don't
      // build SUB/ADD
      if (Imm != -256) {
        addImmediateToRegister(MI, BaseReg, Imm, NewInsts);
      }
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRHT))
                         .addReg(SrcReg)
                         .addReg(BaseReg)
                         .addImm(0));
      if (Imm != -256) {
        subtractImmediateFromRegister(MI, BaseReg, Imm, NewInsts);
      }
      break;

    //================================================================
    // Store byte immediate.
    //================================================================

    // A7.7.160 Encoding T1: STRB<c> <Rt>,[<Rn>{,#<imm5>}]
    case ARM::tSTRBi:
      SrcReg = MI.getOperand(0).getReg();
      BaseReg = MI.getOperand(1).getReg();
      Imm = MI.getOperand(2).getImm();
      // imm5 is small enough to be encoded in STRBT
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRBT))
                         .addReg(SrcReg)
                         .addReg(BaseReg)
                         .addImm(Imm));
      break;

    // A7.7.160 Encoding T2: STRB<c>.W <Rt>,[<Rn>,#<imm12>]
    case ARM::t2STRBi12:
      SrcReg = MI.getOperand(0).getReg();
      BaseReg = MI.getOperand(1).getReg();
      Imm = MI.getOperand(2).getImm();
      if (BaseReg == ARM::SP && Imm > 255) {
        FreeRegs = findFreeRegisters(MI);
        handleSPWithUncommonImm(MI, SrcReg, Imm, ARM::t2STRBT, NewInsts,
                                FreeRegs);
        break;
      }
      // imm12 might go beyond 255; surround STRBT with ADD/SUB
      if (Imm > 255) {
        addImmediateToRegister(MI, BaseReg, Imm, NewInsts);
      }
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRBT))
                         .addReg(SrcReg)
                         .addReg(BaseReg)
                         .addImm(Imm > 255 ? 0 : Imm));
      if (Imm > 255) {
        subtractImmediateFromRegister(MI, BaseReg, Imm, NewInsts);
      }
      break;

    // A7.7.160 Encoding T3: STRB<c> <Rt>,[<Rn>,#-<imm8>]
    case ARM::t2STRBi8:
      SrcReg = MI.getOperand(0).getReg();
      BaseReg = MI.getOperand(1).getReg();
      Imm = MI.getOperand(2).getImm();
      // SP has to be 4 byte aligned; if the easy ways won't apply,
      // special-case it
      if (BaseReg == ARM::SP && Imm % 4 != 0) {
        FreeRegs = findFreeRegisters(MI);
        handleSPWithUncommonImm(MI, SrcReg, Imm, ARM::t2STRBT, NewInsts,
                                FreeRegs);
        break;
      }
      // -imm8 might be 0 (-256 counting the 'U' bit), in which case we don't
      // build SUB/ADD
      if (Imm != -256) {
        addImmediateToRegister(MI, BaseReg, Imm, NewInsts);
      }
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRBT))
                         .addReg(SrcReg)
                         .addReg(BaseReg)
                         .addImm(0));
      if (Imm != -256) {
        subtractImmediateFromRegister(MI, BaseReg, Imm, NewInsts);
      }
      break;

    //================================================================
    // Store word with write-back.
    //================================================================

    // A7.7.158 Encoding T4: STR<c> <Rt>,[<Rn>,#+/-<imm8>]!
    case ARM::t2STR_PRE:
      BaseReg = MI.getOperand(0).getReg();
      SrcReg = MI.getOperand(1).getReg();
      Imm = MI.getOperand(3).getImm();
      // Pre-indexed: first ADD/SUB then STRT
      if (BaseReg == ARM::SP && Imm > 0) {
        // When the base register is SP, we need to specially handle it;
        // otherwise we might encounter an error if a hardware interrupt
        // kicks in after the ADD/SUB operation.
        FreeRegs = findFreeRegisters(MI);
        handleSPWithUncommonImm(MI, SrcReg, Imm, ARM::t2STRT, NewInsts,
                                FreeRegs);
        addImmediateToRegister(MI, BaseReg, Imm, NewInsts);
        break;
      }
      addImmediateToRegister(MI, BaseReg, Imm, NewInsts);
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRT))
                         .addReg(SrcReg)
                         .addReg(BaseReg)
                         .addImm(0));
      break;

    // A7.7.158 Encoding T4: STR<c> <Rt>,[<Rn>],#+/-<imm8>
    case ARM::t2STR_POST:
      BaseReg = MI.getOperand(0).getReg();
      SrcReg = MI.getOperand(1).getReg();
      Imm = MI.getOperand(3).getImm();
      // Post-indexed: first STRT then ADD/SUB
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRT))
                         .addReg(SrcReg)
                         .addReg(BaseReg)
                         .addImm(0));
      addImmediateToRegister(MI, BaseReg, Imm, NewInsts);
      break;

    //================================================================
    // Store halfword with write-back.
    //================================================================

    // A7.7.167 Encoding T3: STRH<c> <Rt>,[<Rn>,#+/-<imm8>]!
    case ARM::t2STRH_PRE:
      BaseReg = MI.getOperand(0).getReg();
      SrcReg = MI.getOperand(1).getReg();
      Imm = MI.getOperand(3).getImm();
      // Pre-indexed: first ADD/SUB then STRHT
      if (BaseReg == ARM::SP && Imm > 0) {
        FreeRegs = findFreeRegisters(MI);
        handleSPWithUncommonImm(MI, SrcReg, Imm, ARM::t2STRHT, NewInsts,
                                FreeRegs);
        addImmediateToRegister(MI, BaseReg, Imm, NewInsts);
        break;
      }
      addImmediateToRegister(MI, BaseReg, Imm, NewInsts);
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRHT))
                         .addReg(SrcReg)
                         .addReg(BaseReg)
                         .addImm(0));
      break;

    // A7.7.167 Encoding T3: STRH<c> <Rt>,[<Rn>],#+/-<imm8>
    case ARM::t2STRH_POST:
      BaseReg = MI.getOperand(0).getReg();
      SrcReg = MI.getOperand(1).getReg();
      Imm = MI.getOperand(3).getImm();
      // Post-indexed: first STRHT then ADD/SUB
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRHT))
                         .addReg(SrcReg)
                         .addReg(BaseReg)
                         .addImm(0));
      addImmediateToRegister(MI, BaseReg, Imm, NewInsts);
      break;

    //================================================================
    // Store byte with write-back.
    //================================================================

    // A7.7.160 Encoding T3: STRB<c> <Rt>,[<Rn>,#+/-<imm8>]!
    case ARM::t2STRB_PRE:
      BaseReg = MI.getOperand(0).getReg();
      SrcReg = MI.getOperand(1).getReg();
      Imm = MI.getOperand(3).getImm();
      // Pre-indexed: first ADD/SUB then STRBT
      if (BaseReg == ARM::SP && Imm > 0) {
        FreeRegs = findFreeRegisters(MI);
        handleSPWithUncommonImm(MI, SrcReg, Imm, ARM::t2STRBT, NewInsts,
                                FreeRegs);
        addImmediateToRegister(MI, BaseReg, Imm, NewInsts);
        break;
      }
      addImmediateToRegister(MI, BaseReg, Imm, NewInsts);
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRBT))
                         .addReg(SrcReg)
                         .addReg(BaseReg)
                         .addImm(0));
      break;

    // A7.7.160 Encoding T3: STRB<c> <Rt>,[<Rn>],#+/-<imm8>
    case ARM::t2STRB_POST:
      BaseReg = MI.getOperand(0).getReg();
      SrcReg = MI.getOperand(1).getReg();
      Imm = MI.getOperand(3).getImm();
      // Post-indexed: first STRBT then ADD/SUB
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRBT))
                         .addReg(SrcReg)
                         .addReg(BaseReg)
                         .addImm(0));
      addImmediateToRegister(MI, BaseReg, Imm, NewInsts);
      break;

    //================================================================
    // Store word register.
    //================================================================

    // A7.7.159 Encoding T1: STR<c> <Rt>,[<Rn>,<Rm>]
    case ARM::tSTRr:
      SrcReg = MI.getOperand(0).getReg();
      BaseReg = MI.getOperand(1).getReg();
      OffsetReg = MI.getOperand(2).getReg();
      // Add offset to base, do store, and subtract offset from base
      if (BaseReg == ARM::SP) {
        FreeRegs = findFreeRegisters(MI);
        handleSPWithOffsetReg(MI, SrcReg, OffsetReg, 0, ARM::t2STRT,
                              NewInsts, FreeRegs);
        break;
      }
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2ADDrr), BaseReg)
                         .addReg(BaseReg)
                         .addReg(OffsetReg)
                         .add(predOps(Pred, PredReg))
                         .add(condCodeOp()));
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRT))
                         .addReg(SrcReg)
                         .addReg(BaseReg)
                         .addImm(0));
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2SUBrr), BaseReg)
                         .addReg(BaseReg)
                         .addReg(OffsetReg)
                         .add(predOps(Pred, PredReg))
                         .add(condCodeOp()));
      break;

    // A7.7.159 Encoding T2: STR<c>.W <Rt>,[<Rn>,<Rm>{,LSL #<imm2>}]
    case ARM::t2STRs:
      SrcReg = MI.getOperand(0).getReg();
      BaseReg = MI.getOperand(1).getReg();
      OffsetReg = MI.getOperand(2).getReg();
      Imm = ARM_AM::getSORegOpc(ARM_AM::lsl, MI.getOperand(3).getImm());
      // Add offset with LSL to base, do store, and subtract offset with LSL
      // from base
      if (BaseReg == ARM::SP) {
        FreeRegs = findFreeRegisters(MI);
        handleSPWithOffsetReg(MI, SrcReg, OffsetReg, Imm, ARM::t2STRT,
                              NewInsts, FreeRegs);
        break;
      }
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2ADDrs), BaseReg)
                         .addReg(BaseReg)
                         .addReg(OffsetReg)
                         .addImm(Imm)
                         .add(predOps(Pred, PredReg))
                         .add(condCodeOp()));
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRT))
                         .addReg(SrcReg)
                         .addReg(BaseReg)
                         .addImm(0));
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2SUBrs), BaseReg)
                         .addReg(BaseReg)
                         .addReg(OffsetReg)
                         .addImm(Imm)
                         .add(predOps(Pred, PredReg))
                         .add(condCodeOp()));
      break;

    //================================================================
    // Store halfword register.
    //================================================================

    // A7.7.168 Encoding T1: STRH<c> <Rt>,[<Rn>,<Rm>]
    case ARM::tSTRHr:
      SrcReg = MI.getOperand(0).getReg();
      BaseReg = MI.getOperand(1).getReg();
      OffsetReg = MI.getOperand(2).getReg();
      if (BaseReg == ARM::SP) {
        FreeRegs = findFreeRegisters(MI);
        handleSPWithOffsetReg(MI, SrcReg, OffsetReg, 0, ARM::t2STRHT,
                              NewInsts, FreeRegs);
        break;
      }
      // Add offset to base, do store, and subtract offset from base
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2ADDrr), BaseReg)
                         .addReg(BaseReg)
                         .addReg(OffsetReg)
                         .add(predOps(Pred, PredReg))
                         .add(condCodeOp()));
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRHT))
                         .addReg(SrcReg)
                         .addReg(BaseReg)
                         .addImm(0));
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2SUBrr), BaseReg)
                         .addReg(BaseReg)
                         .addReg(OffsetReg)
                         .add(predOps(Pred, PredReg))
                         .add(condCodeOp()));
      break;

    // A7.7.168 Encoding T2: STRH<c>.W <Rt>,[<Rn>,<Rm>{,LSL #<imm2>}]
    case ARM::t2STRHs:
      SrcReg = MI.getOperand(0).getReg();
      BaseReg = MI.getOperand(1).getReg();
      OffsetReg = MI.getOperand(2).getReg();
      Imm = ARM_AM::getSORegOpc(ARM_AM::lsl, MI.getOperand(3).getImm());
      if (BaseReg == ARM::SP) {
        FreeRegs = findFreeRegisters(MI);
        handleSPWithOffsetReg(MI, SrcReg, OffsetReg, Imm, ARM::t2STRHT,
                              NewInsts, FreeRegs);
        break;
      }
      // Add offset with LSL to base, do store, and subtract offset with LSL
      // from base
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2ADDrs), BaseReg)
                         .addReg(BaseReg)
                         .addReg(OffsetReg)
                         .addImm(Imm)
                         .add(predOps(Pred, PredReg))
                         .add(condCodeOp()));
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRHT))
                         .addReg(SrcReg)
                         .addReg(BaseReg)
                         .addImm(0));
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2SUBrs), BaseReg)
                         .addReg(BaseReg)
                         .addReg(OffsetReg)
                         .addImm(Imm)
                         .add(predOps(Pred, PredReg))
                         .add(condCodeOp()));
      break;

    //================================================================
    // Store byte register.
    //================================================================

    // A7.7.161 Encoding T1: STRB<c> <Rt>,[<Rn>,<Rm>]
    case ARM::tSTRBr:
      SrcReg = MI.getOperand(0).getReg();
      BaseReg = MI.getOperand(1).getReg();
      OffsetReg = MI.getOperand(2).getReg();
      if (BaseReg == ARM::SP) {
        FreeRegs = findFreeRegisters(MI);
        handleSPWithOffsetReg(MI, SrcReg, OffsetReg, 0, ARM::t2STRBT,
                              NewInsts, FreeRegs);
        break;
      }
      // Add offset to base, do store, and subtract offset from base
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2ADDrr), BaseReg)
                         .addReg(BaseReg)
                         .addReg(OffsetReg)
                         .add(predOps(Pred, PredReg))
                         .add(condCodeOp()));
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRBT))
                         .addReg(SrcReg)
                         .addReg(BaseReg)
                         .addImm(0));
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2SUBrr), BaseReg)
                         .addReg(BaseReg)
                         .addReg(OffsetReg)
                         .add(predOps(Pred, PredReg))
                         .add(condCodeOp()));
      break;

    // A7.7.161 Encoding T2: STRB<c>.W <Rt>,[<Rn>,<Rm>{,LSL #<imm2>}]
    case ARM::t2STRBs:
      SrcReg = MI.getOperand(0).getReg();
      BaseReg = MI.getOperand(1).getReg();
      OffsetReg = MI.getOperand(2).getReg();
      Imm = ARM_AM::getSORegOpc(ARM_AM::lsl, MI.getOperand(3).getImm());
      if (BaseReg == ARM::SP) {
        FreeRegs = findFreeRegisters(MI);
        handleSPWithOffsetReg(MI, SrcReg, OffsetReg, Imm, ARM::t2STRBT,
                              NewInsts, FreeRegs);
        break;
      }
      // Add offset with LSL to base, do store, and subtract offset with LSL
      // from base
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2ADDrs), BaseReg)
                         .addReg(BaseReg)
                         .addReg(OffsetReg)
                         .addImm(Imm)
                         .add(predOps(Pred, PredReg))
                         .add(condCodeOp()));
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRBT))
                         .addReg(SrcReg)
                         .addReg(BaseReg)
                         .addImm(0));
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2SUBrs), BaseReg)
                         .addReg(BaseReg)
                         .addReg(OffsetReg)
                         .addImm(Imm)
                         .add(predOps(Pred, PredReg))
                         .add(condCodeOp()));
      break;

    //================================================================
    // Store dual immediate.
    //================================================================

    // A7.7.163 Encoding T1: STRD<c> <Rt>,<Rt2>,[<Rn>{,#+/-<imm8>}]
    case ARM::t2STRDi8:
      SrcReg = MI.getOperand(0).getReg();
      SrcReg2 = MI.getOperand(1).getReg();
      BaseReg = MI.getOperand(2).getReg();
      Imm = MI.getOperand(3).getImm(); // Already ZeroExtend(imm8:'00', 32)
      Imm2 = Imm;
      // 251 comes from the fact that the immediate of the second STRT cannot
      // go beyond 255.
      if (BaseReg == ARM::SP && Imm > 251) {
        FreeRegs = findFreeRegisters(MI);
        handleSPWithUncommonImm(MI, SrcReg, Imm, ARM::t2STRT, NewInsts,
                                FreeRegs, SrcReg2);
        break;
      }
      // When Imm is negative, add a pair of add/sub to handle this store.
      if (Imm < 0 || Imm > 251) {
        addImmediateToRegister(MI, BaseReg, Imm, NewInsts);
        Imm2 = 0;
      }
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRT))
                         .addReg(SrcReg)
                         .addReg(BaseReg)
                         .addImm(Imm2));
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRT))
                         .addReg(SrcReg2)
                         .addReg(BaseReg)
                         .addImm(Imm2 + 4));
      if (Imm < 0 || Imm > 251) {
        subtractImmediateFromRegister(MI, BaseReg, Imm, NewInsts);
      }
      break;

    //================================================================
    // Store dual immediate with write-back.
    //================================================================

    // A7.7.163 Encoding T1: STRD<c> <Rt>,<Rt2>,[<Rn>,#+/-<imm8>]!
    case ARM::t2STRD_PRE:
      BaseReg = MI.getOperand(0).getReg();
      SrcReg = MI.getOperand(1).getReg();
      SrcReg2 = MI.getOperand(2).getReg();
      Imm = MI.getOperand(4).getImm(); // Already ZeroExtend(imm8:'00', 32)
      if (BaseReg == ARM::SP && Imm > 0) {
        FreeRegs = findFreeRegisters(MI);
        handleSPWithUncommonImm(MI, SrcReg, Imm, ARM::t2STRT, NewInsts,
                                FreeRegs, SrcReg2);
        addImmediateToRegister(MI, BaseReg, Imm, NewInsts);
        break;
      }
      // Pre-indexed: first ADD/SUB then 2 STRBTs
      addImmediateToRegister(MI, BaseReg, Imm, NewInsts);
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRT))
                         .addReg(SrcReg)
                         .addReg(BaseReg)
                         .addImm(0));
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRT))
                         .addReg(SrcReg2)
                         .addReg(BaseReg)
                         .addImm(4));
      break;

    // A7.7.163 Encoding T1: STRD<c> <Rt>,<Rt2>,[<Rn>],#+/-<imm8>
    case ARM::t2STRD_POST:
      BaseReg = MI.getOperand(0).getReg();
      SrcReg = MI.getOperand(1).getReg();
      SrcReg2 = MI.getOperand(2).getReg();
      Imm = MI.getOperand(4).getImm(); // Already ZeroExtend(imm8:'00', 32)
      // Post-indexed: first 2 STRBTs then ADD/SUB
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRT))
                         .addReg(SrcReg)
                         .addReg(BaseReg)
                         .addImm(0));
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRT))
                         .addReg(SrcReg2)
                         .addReg(BaseReg)
                         .addImm(4));
      addImmediateToRegister(MI, BaseReg, Imm, NewInsts);
      break;

    //================================================================
    // Floating-point store.
    //================================================================

    // A7.7.256 Encoding T1: VSTR<c> <Dd>,[<Rn>{,#+/-<imm8>}]
    case ARM::VSTRD:
      SrcReg = MI.getOperand(0).getReg();
      BaseReg = MI.getOperand(1).getReg();
      // Not ZeroExtend(imm8:'00', 32) yet
      Imm = ARM_AM::getAM5Offset(MI.getOperand(2).getImm()) << 2;
      if (ARM_AM::getAM5Op(MI.getOperand(2).getImm()) == ARM_AM::AddrOpc::sub) {
        Imm = -Imm;
      }
      // First try to find 2 free registers; if we couldn't find enough
      // registers, then resort to register spills
      FreeRegs = findFreeRegisters(MI);
      if (FreeRegs.size() >= 2) {
        ScratchReg = FreeRegs[0];
        ScratchReg2 = FreeRegs[1];
      } else {
        errs() << "[SP] Unable to find free registers for " << MI;
        // Saving 2 scratch registers onto the stack causes SP to decrement by
        // 8.  If the base register is SP, we compensate it by increasing the
        // immediate by the same amount.
        if (BaseReg == ARM::SP) {
          Imm += 8;
        }
        // Pick 2 core registers as scratch registers, because STRT can only
        // encode core registers
        ScratchReg = BaseReg == ARM::R0 ? ARM::R1 : ARM::R0;
        ScratchReg2 = BaseReg == ARM::R2 ? ARM::R3 : ARM::R2;
        // Back up scratch registers onto the stack
        backupRegisters(MI, ScratchReg, ScratchReg2, NewInsts);
      }
      Imm2 = Imm;
      // Move from the source register to 2 scratch registers
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::VMOVRRD))
                         .addReg(ScratchReg)
                         .addReg(ScratchReg2)
                         .addReg(SrcReg)
                         .add(predOps(Pred, PredReg)));
      if (BaseReg == ARM::SP && Imm > 251) {
        handleSPWithUncommonImm(MI, ScratchReg, Imm, ARM::t2STRT, NewInsts,
                                FreeRegs, ScratchReg2);
      } else {
        // imm8 could be either negative or beyond 251, in which cases we
        // surround 2 STRTs with ADD/SUB.  251 comes from the fact that the
        // immediate of the second STRT cannot go beyond 255.
        if (Imm < 0 || Imm > 251) {
          addImmediateToRegister(MI, BaseReg, Imm, NewInsts);
          Imm2 = 0;
        }
        NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRT))
                           .addReg(ScratchReg)
                           .addReg(BaseReg)
                           .addImm(Imm2));
        NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRT))
                           .addReg(ScratchReg2)
                           .addReg(BaseReg)
                           .addImm(Imm2 + 4));
        if (Imm < 0 || Imm > 251) {
          subtractImmediateFromRegister(MI, BaseReg, Imm, NewInsts);
        }
      }
      if (FreeRegs.size() < 2) {
        // Restore scratch registers from the stack
        restoreRegisters(MI, ScratchReg, ScratchReg2, NewInsts);
      }
      break;

    // A7.7.256 Encoding T2: VSTR<c> <Sd>,[<Rn>{,#+/-<imm8>}]
    case ARM::VSTRS:
      SrcReg = MI.getOperand(0).getReg();
      BaseReg = MI.getOperand(1).getReg();
      // Not ZeroExtend(imm8:'00', 32) yet
      Imm = ARM_AM::getAM5Offset(MI.getOperand(2).getImm()) << 2;
      if (ARM_AM::getAM5Op(MI.getOperand(2).getImm()) == ARM_AM::AddrOpc::sub) {
        Imm = -Imm;
      }
      // First try to find a free register; if we couldn't find one, then
      // resort to register spill
      FreeRegs = findFreeRegisters(MI);
      if (!FreeRegs.empty()) {
        ScratchReg = FreeRegs[0];
      } else {
        errs() << "[SP] Unable to find a free register for " << MI;
        // Saving a scratch register onto the stack causes SP to decrement by
        // 4.  If the base register is SP, we compensate it by increasing the
        // immediate by the same amount.
        if (BaseReg == ARM::SP) {
          Imm += 4;
        }
        // Pick a core register as a scratch register, because STRT can only
        // encode core registers
        ScratchReg = BaseReg == ARM::R0 ? ARM::R1 : ARM::R0;
        // Back up the scratch register onto the stack
        backupRegisters(MI, ScratchReg, ARM::NoRegister, NewInsts);
      }
      Imm2 = Imm;
      // Move from the source register to the scratch register
      NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::VMOVRS), ScratchReg)
                         .addReg(SrcReg)
                         .add(predOps(Pred, PredReg)));
      if (BaseReg == ARM::SP && Imm > 255) {
        handleSPWithUncommonImm(MI, ScratchReg, Imm, ARM::t2STRT, NewInsts,
                                FreeRegs);
      } else {
        // imm8 could be either negative or beyond 255 (after compensating),
        // in which cases we surround STRT with ADD/SUB
        if (Imm < 0 || Imm > 255) {
          addImmediateToRegister(MI, BaseReg, Imm, NewInsts);
          Imm2 = 0;
        }
        NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRT))
                           .addReg(ScratchReg)
                           .addReg(BaseReg)
                           .addImm(Imm2));
        if (Imm < 0 || Imm > 255) {
          subtractImmediateFromRegister(MI, BaseReg, Imm, NewInsts);
        }
      }
      if (FreeRegs.empty()) {
        // Restore scratch registers from the stack
        restoreRegisters(MI, ScratchReg, ARM::NoRegister, NewInsts);
      }
      break;

    //================================================================
    // Store multiple.
    //================================================================

    // A7.7.156 Encoding T1: STM<c> <Rn>!,<registers>
    case ARM::tSTMIA_UPD:
    // A7.7.156 Encoding T2; STM<c>.W <Rn>!,<registers>
    case ARM::t2STMIA_UPD:
      BaseReg = MI.getOperand(0).getReg();
      // Construct a register list
      for (unsigned i = MI.findFirstPredOperandIdx() + 2;
           i < MI.getNumOperands();
           ++i) {
        RegList.push_back(MI.getOperand(i).getReg());
      }
      // Build an STRT for each register in the list
      for (unsigned i = 0; i < RegList.size(); ++i) {
        NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRT))
                           .addReg(RegList[i])
                           .addReg(BaseReg)
                           .addImm(i * 4));
      }
      // Increment the base register
      addImmediateToRegister(MI, BaseReg, RegList.size() * 4, NewInsts);
      break;

    // A7.7.156 Encoding T2; STM<c>.W <Rn>,<registers>
    case ARM::t2STMIA:
      BaseReg = MI.getOperand(0).getReg();
      // Construct a register list
      for (unsigned i = MI.findFirstPredOperandIdx() + 2;
           i < MI.getNumOperands();
           ++i) {
        RegList.push_back(MI.getOperand(i).getReg());
      }
      // Build an STRT for each register in the list
      for (unsigned i = 0; i < RegList.size(); ++i) {
        NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRT))
                           .addReg(RegList[i])
                           .addReg(BaseReg)
                           .addImm(i * 4));
      }
      break;

    // A7.7.157 Encoding T1; STMDB<c> <Rn>,<registers>
    case ARM::t2STMDB:
      BaseReg = MI.getOperand(0).getReg();
      // Construct a register list
      for (unsigned i = MI.findFirstPredOperandIdx() + 2;
           i < MI.getNumOperands();
           ++i) {
        RegList.push_back(MI.getOperand(i).getReg());
      }
      // Decrement the base register
      subtractImmediateFromRegister(MI, BaseReg, RegList.size() * 4, NewInsts);
      // Build an STRT for each register in the list
      for (unsigned i = 0; i < RegList.size(); ++i) {
        NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRT))
                           .addReg(RegList[i])
                           .addReg(BaseReg)
                           .addImm(i * 4));
      }
      // Restore the incremented base register
      addImmediateToRegister(MI, BaseReg, RegList.size() * 4, NewInsts);
      break;

    // A7.7.157 Encoding T1; STMDB<c> <Rn>!,<registers>
    case ARM::t2STMDB_UPD:
      BaseReg = MI.getOperand(0).getReg();
      // Construct a register list
      for (unsigned i = MI.findFirstPredOperandIdx() + 2;
           i < MI.getNumOperands();
           ++i) {
        RegList.push_back(MI.getOperand(i).getReg());
      }
      // Decrement the base register
      subtractImmediateFromRegister(MI, BaseReg, RegList.size() * 4, NewInsts);
      // Build an STRT for each register in the list
      for (unsigned i = 0; i < RegList.size(); ++i) {
        NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRT))
                           .addReg(RegList[i])
                           .addReg(BaseReg)
                           .addImm(i * 4));
      }
      break;

    //================================================================
    // Push.
    //================================================================

    // A7.7.99 Encoding T1: PUSH<c> <registers>
    case ARM::tPUSH:
      BaseReg = ARM::SP;
      // Construct a register list
      for (unsigned i = MI.findFirstPredOperandIdx() + 2;
           i < MI.getNumOperands();
           ++i) {
        MachineOperand & MO = MI.getOperand(i);
        // PUSH implicitly defines and uses SP, so we exclude them explicitly
        if (!MO.isImplicit()) {
          RegList.push_back(MO.getReg());
        }
      }
      // Decrement the base register
      subtractImmediateFromRegister(MI, BaseReg, RegList.size() * 4, NewInsts);
      // Build an STRT for each register in the list
      for (unsigned i = 0; i < RegList.size(); ++i) {
        NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRT))
                           .addReg(RegList[i])
                           .addReg(BaseReg)
                           .addImm(i * 4));
      }
      break;

    //================================================================
    // Floating-point store multiple.
    //================================================================

    // A7.7.255 Encoding T1: VSTMDIA<c> <Rn>,<list>
    case ARM::VSTMDIA:
      BaseReg = MI.getOperand(0).getReg();
      // Construct a register list
      for (unsigned i = MI.findFirstPredOperandIdx() + 2;
           i < MI.getNumOperands();
           ++i) {
        RegList.push_back(MI.getOperand(i).getReg());
      }
      // First try to find 2 free registers; if we couldn't find enough
      // registers, then resort to register spills
      FreeRegs = findFreeRegisters(MI);
      if (FreeRegs.size() >= 2) {
        ScratchReg = FreeRegs[0];
        ScratchReg2 = FreeRegs[1];
      } else {
        errs() << "[SP] Unable to find free registers for " << MI;
        // Pick 2 core registers as scratch registers, because STRT can only
        // encode core registers
        ScratchReg = BaseReg == ARM::R0 ? ARM::R1 : ARM::R0;
        ScratchReg2 = BaseReg == ARM::R2 ? ARM::R3 : ARM::R2;
        // Back up scratch registers onto the stack
        backupRegisters(MI, ScratchReg, ScratchReg2, NewInsts);
      }
      // Build 2 STRTs for each doubleword register in the list
      for (unsigned i = 0; i < RegList.size(); ++i) {
        Imm = i * 8;
        // Saving 2 scratch registers onto the stack causes SP to decrement by
        // 8.  If the base register is SP, we compensate it by increasing the
        // immediate by the same amount.
        if (BaseReg == ARM::SP && FreeRegs.size() < 2) {
          Imm += 8;
        }
        // Move from the source register to 2 scratch registers
        NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::VMOVRRD))
                           .addReg(ScratchReg)
                           .addReg(ScratchReg2)
                           .addReg(RegList[i])
                           .add(predOps(Pred, PredReg)));
        NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRT))
                           .addReg(ScratchReg)
                           .addReg(BaseReg)
                           .addImm(Imm));
        NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRT))
                           .addReg(ScratchReg2)
                           .addReg(BaseReg)
                           .addImm(Imm + 4));
      }
      if (FreeRegs.size() < 2) {
        // Restore scratch registers from the stack
        restoreRegisters(MI, ScratchReg, ScratchReg2, NewInsts);
      }
      break;

    // A7.7.255 Encoding T1: VSTMDIA<c> <Rn>!,<list>
    case ARM::VSTMDIA_UPD:
      BaseReg = MI.getOperand(0).getReg();
      // Construct a register list
      for (unsigned i = MI.findFirstPredOperandIdx() + 2;
           i < MI.getNumOperands();
           ++i) {
        RegList.push_back(MI.getOperand(i).getReg());
      }
      // First try to find 2 free registers; if we couldn't find enough
      // registers, then resort to register spills
      FreeRegs = findFreeRegisters(MI);
      if (FreeRegs.size() >= 2) {
        ScratchReg = FreeRegs[0];
        ScratchReg2 = FreeRegs[1];
      } else {
        errs() << "[SP] Unable to find free registers for " << MI;
        // Pick 2 core registers as scratch registers, because STRT can only
        // encode core registers
        ScratchReg = BaseReg == ARM::R0 ? ARM::R1 : ARM::R0;
        ScratchReg2 = BaseReg == ARM::R2 ? ARM::R3 : ARM::R2;
        // Back up scratch registers onto the stack
        backupRegisters(MI, ScratchReg, ScratchReg2, NewInsts);
      }
      // Build 2 STRTs for each doubleword register in the list
      for (unsigned i = 0; i < RegList.size(); ++i) {
        Imm = i * 8;
        // Saving 2 scratch registers onto the stack causes SP to decrement by
        // 8.  If the base register is SP, we compensate it by increasing the
        // immediate by the same amount.
        if (BaseReg == ARM::SP && FreeRegs.size() < 2) {
          Imm += 8;
        }
        // Move from the source register to 2 scratch registers
        NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::VMOVRRD))
                           .addReg(ScratchReg)
                           .addReg(ScratchReg2)
                           .addReg(RegList[i])
                           .add(predOps(Pred, PredReg)));
        NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRT))
                           .addReg(ScratchReg)
                           .addReg(BaseReg)
                           .addImm(Imm));
        NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRT))
                           .addReg(ScratchReg2)
                           .addReg(BaseReg)
                           .addImm(Imm + 4));
      }
      if (FreeRegs.size() < 2) {
        // Restore scratch registers from the stack
        restoreRegisters(MI, ScratchReg, ScratchReg2, NewInsts);
      }
      // Increment the base register
      addImmediateToRegister(MI, BaseReg, RegList.size() * 8, NewInsts);
      break;

    // A7.7.255 Encoding T1: VSTMDDB<c> <Rn>!,<list>
    case ARM::VSTMDDB_UPD:
      BaseReg = MI.getOperand(0).getReg();
      // Construct a register list
      for (unsigned i = MI.findFirstPredOperandIdx() + 2;
           i < MI.getNumOperands();
           ++i) {
        RegList.push_back(MI.getOperand(i).getReg());
      }
      // Decrement the base register
      subtractImmediateFromRegister(MI, BaseReg, RegList.size() * 8, NewInsts);
      // First try to find 2 free registers; if we couldn't find enough
      // registers, then resort to register spills
      FreeRegs = findFreeRegisters(MI);
      if (FreeRegs.size() >= 2) {
        ScratchReg = FreeRegs[0];
        ScratchReg2 = FreeRegs[1];
      } else {
        errs() << "[SP] Unable to find free registers for " << MI;
        // Pick 2 core registers as scratch registers, because STRT can only
        // encode core registers
        ScratchReg = BaseReg == ARM::R0 ? ARM::R1 : ARM::R0;
        ScratchReg2 = BaseReg == ARM::R2 ? ARM::R3 : ARM::R2;
        // Back up scratch registers onto the stack
        backupRegisters(MI, ScratchReg, ScratchReg2, NewInsts);
      }
      // Build 2 STRTs for each doubleword register in the list
      for (unsigned i = 0; i < RegList.size(); ++i) {
        Imm = i * 8;
        // Saving 2 scratch registers onto the stack causes SP to decrement by
        // 8.  If the base register is SP, we compensate it by increasing the
        // immediate by the same amount.
        if (BaseReg == ARM::SP && FreeRegs.size() < 2) {
          Imm += 8;
        }
        // Move from the source register to 2 scratch registers
        NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::VMOVRRD))
                           .addReg(ScratchReg)
                           .addReg(ScratchReg2)
                           .addReg(RegList[i])
                           .add(predOps(Pred, PredReg)));
        NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRT))
                           .addReg(ScratchReg)
                           .addReg(BaseReg)
                           .addImm(Imm));
        NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRT))
                           .addReg(ScratchReg2)
                           .addReg(BaseReg)
                           .addImm(Imm + 4));
      }
      if (FreeRegs.size() < 2) {
        // Restore scratch registers from the stack
        restoreRegisters(MI, ScratchReg, ScratchReg2, NewInsts);
      }
      break;

    // A7.7.255 Encoding T2: VSTMSIA<c> <Rn>,<list>
    case ARM::VSTMSIA:
      BaseReg = MI.getOperand(0).getReg();
      // Construct a register list
      for (unsigned i = MI.findFirstPredOperandIdx() + 2;
           i < MI.getNumOperands();
           ++i) {
        RegList.push_back(MI.getOperand(i).getReg());
      }
      // First try to find a free register; if we couldn't find one, then
      // resort to register spill
      FreeRegs = findFreeRegisters(MI);
      if (!FreeRegs.empty()) {
        ScratchReg = FreeRegs[0];
      } else {
        errs() << "[SP] Unable to find a free register for " << MI;
        // Pick a core register as a scratch register, because STRT can only
        // encode core registers
        ScratchReg = BaseReg == ARM::R0 ? ARM::R1 : ARM::R0;
        // Back up the scratch register onto the stack
        backupRegisters(MI, ScratchReg, ARM::NoRegister, NewInsts);
      }
      // Build STRT for each singleword register in the list
      for (unsigned i = 0; i < RegList.size(); ++i) {
        Imm = i * 4;
        // Saving a scratch register onto the stack causes SP to decrement by
        // 4.  If the base register is SP, we compensate it by increasing the
        // immediate by the same amount.
        if (BaseReg == ARM::SP && FreeRegs.empty()) {
          Imm += 4;
        }
        // Move from the source register to the scratch register
        NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::VMOVRS), ScratchReg)
                           .addReg(RegList[i])
                           .add(predOps(Pred, PredReg)));
        NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRT))
                           .addReg(ScratchReg)
                           .addReg(BaseReg)
                           .addImm(Imm));
      }
      if (FreeRegs.empty()) {
        // Restore the scratch register from the stack
        restoreRegisters(MI, ScratchReg, ARM::NoRegister, NewInsts);
      }
      break;

    // A7.7.255 Encoding T2: VSTMSIA<c> <Rn>!,<list>
    case ARM::VSTMSIA_UPD:
      BaseReg = MI.getOperand(0).getReg();
      // Construct a register list
      for (unsigned i = MI.findFirstPredOperandIdx() + 2;
           i < MI.getNumOperands();
           ++i) {
        RegList.push_back(MI.getOperand(i).getReg());
      }
      // First try to find a free register; if we couldn't find one, then
      // resort to register spill
      FreeRegs = findFreeRegisters(MI);
      if (!FreeRegs.empty()) {
        ScratchReg = FreeRegs[0];
      } else {
        errs() << "[SP] Unable to find a free register for " << MI;
        // Pick a core register as a scratch register, because STRT can only
        // encode core registers
        ScratchReg = BaseReg == ARM::R0 ? ARM::R1 : ARM::R0;
        // Back up the scratch register onto the stack
        backupRegisters(MI, ScratchReg, ARM::NoRegister, NewInsts);
      }
      // Build STRT for each singleword register in the list
      for (unsigned i = 0; i < RegList.size(); ++i) {
        Imm = i * 4;
        // Saving a scratch register onto the stack causes SP to decrement by
        // 4.  If the base register is SP, we compensate it by increasing the
        // immediate by the same amount.
        if (BaseReg == ARM::SP && FreeRegs.empty()) {
          Imm += 4;
        }
        // Move from the source register to the scratch register
        NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::VMOVRS), ScratchReg)
                           .addReg(RegList[i])
                           .add(predOps(Pred, PredReg)));
        NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRT))
                           .addReg(ScratchReg)
                           .addReg(BaseReg)
                           .addImm(Imm));
      }
      if (FreeRegs.empty()) {
        // Restore the scratch register from the stack
        restoreRegisters(MI, ScratchReg, ARM::NoRegister, NewInsts);
      }
      // Increment the base register
      addImmediateToRegister(MI, BaseReg, RegList.size() * 4, NewInsts);
      break;

    // A7.7.255 Encoding T2: VSTMSDB<c> <Rn>!,<list>
    case ARM::VSTMSDB_UPD:
      BaseReg = MI.getOperand(0).getReg();
      // Construct a register list
      for (unsigned i = MI.findFirstPredOperandIdx() + 2;
           i < MI.getNumOperands();
           ++i) {
        RegList.push_back(MI.getOperand(i).getReg());
      }
      // Decrement the base register
      subtractImmediateFromRegister(MI, BaseReg, RegList.size() * 4, NewInsts);
      // First try to find a free register; if we couldn't find one, then
      // resort to register spill
      FreeRegs = findFreeRegisters(MI);
      if (!FreeRegs.empty()) {
        ScratchReg = FreeRegs[0];
      } else {
        errs() << "[SP] Unable to find a free register for " << MI;
        // Pick a core register as a scratch register, because STRT can only
        // encode core registers
        ScratchReg = BaseReg == ARM::R0 ? ARM::R1 : ARM::R0;
        // Back up the scratch register onto the stack
        backupRegisters(MI, ScratchReg, ARM::NoRegister, NewInsts);
      }
      // Build STRT for each singleword register in the list
      for (unsigned i = 0; i < RegList.size(); ++i) {
        Imm = i * 4;
        // Saving a scratch register onto the stack causes SP to decrement by
        // 4.  If the base register is SP, we compensate it by increasing the
        // immediate by the same amount.
        if (BaseReg == ARM::SP && FreeRegs.empty()) {
          Imm += 4;
        }
        // Move from the source register to the scratch register
        NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::VMOVRS), ScratchReg)
                           .addReg(RegList[i])
                           .add(predOps(Pred, PredReg)));
        NewInsts.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRT))
                           .addReg(ScratchReg)
                           .addReg(BaseReg)
                           .addImm(Imm));
      }
      if (FreeRegs.empty()) {
        // Restore the scratch register from the stack
        restoreRegisters(MI, ScratchReg, ARM::NoRegister, NewInsts);
      }
      break;

    default:
      llvm_unreachable("Unexpected opcode!");
    }

    if (!NewInsts.empty()) {
      insertInstsBefore(MI, NewInsts);
      removeInst(MI);
    }
  }

  unsigned long NewCodeSize = getFunctionCodeSize(MF);

  // Output code size information
  std::error_code EC;
  raw_fd_ostream MemStat("./code_size_sp.stat", EC,
                         sys::fs::OpenFlags::F_Append);
  MemStat << MF.getName() << ":" << OldCodeSize << ":" << NewCodeSize << "\n";

  return true;
}

//
// Create a new pass.
//
namespace llvm {
  FunctionPass * createARMSilhouetteSTR2STRT(void) {
    return new ARMSilhouetteSTR2STRT();
  }
}
