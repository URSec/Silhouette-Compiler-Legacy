//===-- ARMSilhouetteSFI - Software Fault Isolation on stores -------------===//
//
//         Protecting Control Flow of Real-time OS applications
//
// This file was written by at the University of Rochester.
// All Rights Reserved.
//
//===----------------------------------------------------------------------===//
//
// This pass applies bit-masking on addresses that the generated code stores
// into, either selectively (applying to only heavyweight stores) or entirely.
//
//===----------------------------------------------------------------------===//
//

#include "ARM.h"
#include "ARMSilhouetteConvertFuncList.h"
#include "ARMSilhouetteSFI.h"
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

char ARMSilhouetteSFI::ID = 0;

static DebugLoc DL;

ARMSilhouetteSFI::ARMSilhouetteSFI()
    : MachineFunctionPass(ID) {
}

StringRef
ARMSilhouetteSFI::getPassName() const {
  return "ARM Silhouette Store SFI Pass";
}

//
// Function: immediateStoreOpcode()
//
// Description:
//   This function takes an opcode of a 2-register store (i.e., a store with 2
//   registers as a memory operand) and returns the opcode of its corresponding
//   register-immediate store (i.e., a store whose memory operand consists of a
//   register and an immediate).
//
// Input:
//   opcode - The opcode of a two-register store.
//
// Return value:
//   The opcode of the corresponding register-immediate store.
//
static unsigned
immediateStoreOpcode(unsigned opcode) {
  switch (opcode) {
  case ARM::tSTRr:
    return ARM::tSTRi;

  case ARM::t2STRs:
    return ARM::t2STRi12;

  case ARM::tSTRHr:
    return ARM::tSTRHi;

  case ARM::t2STRHs:
    return ARM::t2STRHi12;

  case ARM::tSTRBr:
    return ARM::tSTRBi;

  case ARM::t2STRBs:
    return ARM::t2STRBi12;

  default:
    llvm_unreachable("Unexpected opcode!");
  }
}

//
// Function: doBitmasking()
//
// Description:
//   This function applies bit-masking on a register.
//
// Inputs:
//   MI    - A reference to the store instruction before which to insert new
//           instructions.
//   Reg   - The register to bit-mask.
//   Insts - A reference to a deque that contains new instructions.
//
static void
doBitmasking(MachineInstr & MI, unsigned Reg, std::deque<MachineInstr *> & Insts) {
  MachineFunction & MF = *MI.getMF();
  const TargetInstrInfo * TII = MF.getSubtarget().getInstrInfo();

  unsigned PredReg;
  ARMCC::CondCodes Pred = getInstrPredicate(MI, PredReg);

  Insts.push_back(BuildMI(MF, DL, TII->get(ARM::t2BICri), Reg)
                  .addReg(Reg)
                  .addImm(SFI_MASK)
                  .add(predOps(Pred, PredReg))
                  .add(condCodeOp()));
  Insts.push_back(BuildMI(MF, DL, TII->get(ARM::t2BICri), Reg)
                  .addReg(Reg)
                  .addImm(SFI_MASK2)
                  .add(predOps(Pred, PredReg))
                  .add(condCodeOp()));
}

//
// Function: handleSPUncommonImmediate()
//
// Description:
//   This function takes care of cases where the base register of a store is SP
//   and the immediate offset is either not aligned by 4 or too large.  It is
//   caller's responsibility to update MI's base register to the return value
//   and immediate to 0 after the function call.
//
// Inputs:
//   MI          - A reference to the store instruction before and after which
//                 to insert new instructions.
//   SrcReg      - The source register of MI.
//   Imm         - The immediate of MI.
//   InstsBefore - A reference to a deque that contains new instructions to
//                 insert before MI.
//   InstsAfter  - A reference to a deque that contains new instructions to
//                 insert after MI.
//   SrcReg2     - The second source register of MI in case MI is a double
//                 word store.
//
// Return value:
//   The new base register.
//
static unsigned
handleSPUncommonImmediate(MachineInstr & MI, unsigned SrcReg, int64_t Imm,
                          std::deque<MachineInstr *> & InstsBefore,
                          std::deque<MachineInstr *> & InstsAfter,
                          unsigned SrcReg2 = ARM::NoRegister) {
  MachineFunction & MF = *MI.getMF();
  const TargetInstrInfo * TII = MF.getSubtarget().getInstrInfo();

  unsigned PredReg;
  ARMCC::CondCodes Pred = getInstrPredicate(MI, PredReg);

  // Save a scratch register onto the stack.  Note that we are introducing a
  // new store here, so this store needs to be instrumented as well.
  unsigned ScratchReg = ARM::R0;
  while (ScratchReg == SrcReg || ScratchReg == SrcReg2) ScratchReg++;
  doBitmasking(MI, ARM::SP, InstsBefore);
  InstsBefore.push_back(BuildMI(MF, DL, TII->get(ARM::tPUSH))
                        .add(predOps(Pred, PredReg))
                        .addReg(ScratchReg));
  Imm += 4; // Compensate for SP decrement

  // Add SP with the unaligned immediate to the scratch register
  unsigned addOpc = Imm < 0 ? ARM::t2SUBri12 : ARM::t2ADDri12;
  InstsBefore.push_back(BuildMI(MF, DL, TII->get(addOpc), ScratchReg)
                        .addReg(ARM::SP)
                        .addImm(Imm < 0 ? -Imm : Imm)
                        .add(predOps(Pred, PredReg)));

  // Do bit-masking
  doBitmasking(MI, ScratchReg, InstsBefore);

  // Restore the scratch register from the stack
  InstsAfter.push_back(BuildMI(MF, DL, TII->get(ARM::tPOP))
                       .add(predOps(Pred, PredReg))
                       .addReg(ScratchReg));

  return ScratchReg;
}

//
// Method: runOnMachineFunction()
//
// Description:
//   This method is called when the PassManager wants this pass to transform
//   the specified MachineFunction.  This method applies bit-masking on the
//   address to which a store instruction writes, either for all regular stores
//   or selectively (applying to only heavyweight stores).
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
ARMSilhouetteSFI::runOnMachineFunction(MachineFunction & MF) {
#if 1
  // Skip certain functions
  if (funcBlacklist.find(MF.getName()) != funcBlacklist.end()) {
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
        // Lightweight stores; instrument them only if we are using full SFI
        if (SilhouetteSFI == FullSFI) {
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
        // Heavyweight stores; leave them as is only if we are not using SFI
        if (SilhouetteSFI != NoSFI) {
          Stores.push_back(&MI);
        }
        break;

      case ARM::INLINEASM:
        break;

      default:
        errs() << "[SFI] Unidentified store: " << MI;
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
    int64_t Imm;

    std::deque<MachineInstr *> InstsBefore;
    std::deque<MachineInstr *> InstsAfter;
    switch (MI.getOpcode()) {
    // A7.7.158 Encoding T1: STR<c> <Rt>,[<Rn>{,#<imm5>}]
    case ARM::tSTRi:
    // A7.7.167 Encoding T1: STRH<c> <Rt>,[<Rn>{,#<imm5>}]
    case ARM::tSTRHi:
    // A7.7.160 Encoding T1: STRB<c> <Rt>,[<Rn>{,#<imm5>}]
    case ARM::tSTRBi:
      BaseReg = MI.getOperand(1).getReg();
      // The immediate is small enough; just bit-mask and store
      doBitmasking(MI, BaseReg, InstsBefore);
      break;

    // A7.7.158 Encoding T2: STR<c> <Rt>,[SP,#<imm8>]
    case ARM::tSTRspi:
      BaseReg = MI.getOperand(1).getReg();
      Imm = MI.getOperand(2).getImm() << 2; // Not ZeroExtend(imm8:'00', 32) yet
      if (Imm < 256) {
        // For a small immediate, just bit-mask and store.
        doBitmasking(MI, BaseReg, InstsBefore);
      } else {
        // For a large immediate, handle it separately.
        BaseReg = handleSPUncommonImmediate(MI, MI.getOperand(0).getReg(), Imm,
                                            InstsBefore, InstsAfter);
        MI.setDesc(TII->get(ARM::t2STRi12));
        MI.getOperand(1).setReg(BaseReg);
        MI.getOperand(2).setImm(0);
      }
      break;

    // A7.7.158 Encoding T3: STR<c>.W <Rt>,[<Rn>,#<imm12>]
    case ARM::t2STRi12:
    // A7.7.167 Encoding T2: STRH<c>.W <Rt>,[<Rn>,#<imm12>]
    case ARM::t2STRHi12:
    // A7.7.160 Encoding T2: STRB<c>.W <Rt>,[<Rn>,#<imm12>]
    case ARM::t2STRBi12:
      BaseReg = MI.getOperand(1).getReg();
      Imm = MI.getOperand(2).getImm();
      // For a small immediate, just bit-mask and store
      if (Imm < 256) {
        doBitmasking(MI, BaseReg, InstsBefore);
        break;
      }
      if (BaseReg == ARM::SP) {
        // Special-case SP as it should not be incremented
        BaseReg = handleSPUncommonImmediate(MI, MI.getOperand(0).getReg(), Imm,
                                            InstsBefore, InstsAfter);
        MI.getOperand(1).setReg(BaseReg);
        MI.getOperand(2).setImm(0);
      } else {
        // Add, bit-mask, store, and subtract
        addImmediateToRegister(MI, BaseReg, Imm, InstsBefore);
        MI.getOperand(2).setImm(0);
        doBitmasking(MI, BaseReg, InstsBefore);
        subtractImmediateFromRegister(MI, BaseReg, Imm, InstsAfter);
      }
      break;

    // A7.7.158 Encoding T4: STR<c> <Rt>,[<Rn>,#-<imm8>]
    case ARM::t2STRi8:
    // A7.7.167 Encoding T3: STRH<c> <Rt>,[<Rn>,#-<imm8>]
    case ARM::t2STRHi8:
    // A7.7.160 Encoding T3: STRB<c> <Rt>,[<Rn>,#-<imm8>]
    case ARM::t2STRBi8:
      BaseReg = MI.getOperand(1).getReg();
      // The immediate is small enough; just bit-mask and store
      doBitmasking(MI, BaseReg, InstsBefore);
      break;

    // A7.7.158 Encoding T4: STR<c> <Rt>,[<Rn>,#+/-<imm8>]!
    case ARM::t2STR_PRE:
    // A7.7.167 Encoding T3: STRH<c> <Rt>,[<Rn>,#+/-<imm8>]!
    case ARM::t2STRH_PRE:
    // A7.7.160 Encoding T3: STRB<c> <Rt>,[<Rn>,#+/-<imm8>]!
    case ARM::t2STRB_PRE:
      BaseReg = MI.getOperand(0).getReg();
      // Pre-indexed stores: just bit-mask and store
      doBitmasking(MI, BaseReg, InstsBefore);
      break;

    // A7.7.158 Encoding T4: STR<c> <Rt>,[<Rn>],#+/-<imm8>
    case ARM::t2STR_POST:
    // A7.7.167 Encoding T3: STRH<c> <Rt>,[<Rn>],#+/-<imm8>
    case ARM::t2STRH_POST:
    // A7.7.160 Encoding T3: STRB<c> <Rt>,[<Rn>],#+/-<imm8>
    case ARM::t2STRB_POST:
      BaseReg = MI.getOperand(0).getReg();
      // Post-indexed stores: just bit-mask and store
      doBitmasking(MI, BaseReg, InstsBefore);
      break;

    // A7.7.159 Encoding T1: STR<c> <Rt>,[<Rn>,<Rm>]
    case ARM::tSTRr:
    // A7.7.168 Encoding T1: STRH<c> <Rt>,[<Rn>,<Rm>]
    case ARM::tSTRHr:
    // A7.7.161 Encoding T1: STRB<c> <Rt>,[<Rn>,<Rm>]
    case ARM::tSTRBr:
      BaseReg = MI.getOperand(1).getReg();
      OffsetReg = MI.getOperand(2).getReg();
      // Add, bit-mask, store, and subtract
      InstsBefore.push_back(BuildMI(MF, DL, TII->get(ARM::t2ADDrr), BaseReg)
                            .addReg(BaseReg)
                            .addReg(OffsetReg)
                            .add(predOps(Pred, PredReg))
                            .add(condCodeOp()));
      doBitmasking(MI, BaseReg, InstsBefore);
      // Need to change tSTR[BH]r to tSTR[BH]i
      MI.setDesc(TII->get(immediateStoreOpcode(MI.getOpcode())));
      MI.getOperand(2).ChangeToImmediate(0);
      InstsAfter.push_back(BuildMI(MF, DL, TII->get(ARM::t2SUBrr), BaseReg)
                           .addReg(BaseReg)
                           .addReg(OffsetReg)
                           .add(predOps(Pred, PredReg))
                           .add(condCodeOp()));
      break;

    // A7.7.159 Encoding T2: STR<c>.W <Rt>,[<Rn>,<Rm>{,LSL #<imm2>}]
    case ARM::t2STRs:
    // A7.7.168 Encoding T2: STRH<c>.W <Rt>,[<Rn>,<Rm>{,LSL #<imm2>}]
    case ARM::t2STRHs:
    // A7.7.161 Encoding T2: STRB<c>.W <Rt>,[<Rn>,<Rm>{,LSL #<imm2>}]
    case ARM::t2STRBs:
      BaseReg = MI.getOperand(1).getReg();
      OffsetReg = MI.getOperand(2).getReg();
      Imm = ARM_AM::getSORegOpc(ARM_AM::lsl, MI.getOperand(3).getImm());
      // Add, bit-mask, store, and subtract
      InstsBefore.push_back(BuildMI(MF, DL, TII->get(ARM::t2ADDrs), BaseReg)
                            .addReg(BaseReg)
                            .addReg(OffsetReg)
                            .addImm(Imm)
                            .add(predOps(Pred, PredReg))
                            .add(condCodeOp()));
      doBitmasking(MI, BaseReg, InstsBefore);
      // Need to change t2STR[BH]s to t2STR[BH]i12
      MI.setDesc(TII->get(immediateStoreOpcode(MI.getOpcode())));
      MI.getOperand(2).ChangeToImmediate(0);
      MI.RemoveOperand(3);
      InstsAfter.push_back(BuildMI(MF, DL, TII->get(ARM::t2SUBrs), BaseReg)
                           .addReg(BaseReg)
                           .addReg(OffsetReg)
                           .addImm(Imm)
                           .add(predOps(Pred, PredReg))
                           .add(condCodeOp()));
      break;

    // A7.7.163 Encoding T1: STRD<c> <Rt>,<Rt2>,[<Rn>{,#+/-<imm8>}]
    case ARM::t2STRDi8:
      BaseReg = MI.getOperand(2).getReg();
      Imm = MI.getOperand(3).getImm(); // Already ZeroExtend(imm8:'00', 32)
      // For a small immediate, just bit-mask and store
      if (Imm >= -256 && Imm < 256) {
        doBitmasking(MI, BaseReg, InstsBefore);
        break;
      }
      if (BaseReg == ARM::SP) {
        // Special-case SP as it should not be incremented
        BaseReg = handleSPUncommonImmediate(MI, MI.getOperand(0).getReg(), Imm,
                                            InstsBefore, InstsAfter,
                                            MI.getOperand(1).getReg());
        MI.getOperand(2).setReg(BaseReg);
        MI.getOperand(3).setImm(0);
      } else {
        // Add, bit-mask, store, and subtract
        addImmediateToRegister(MI, BaseReg, Imm, InstsBefore);
        MI.getOperand(3).setImm(0);
        doBitmasking(MI, BaseReg, InstsBefore);
        subtractImmediateFromRegister(MI, BaseReg, Imm, InstsAfter);
      }
      break;

    // A7.7.163 Encoding T1: STRD<c> <Rt>,<Rt2>,[<Rn>,#+/-<imm8>]!
    case ARM::t2STRD_PRE:
      BaseReg = MI.getOperand(0).getReg();
      Imm = MI.getOperand(4).getImm(); // Already ZeroExtend(imm8:'00', 32)
      // For a small immediate, just bit-mask and store
      if (Imm >= -256 && Imm < 256) {
        doBitmasking(MI, BaseReg, InstsBefore);
        break;
      }
      if (BaseReg == ARM::SP) {
        // Special-case SP as it should not be incremented
        BaseReg = handleSPUncommonImmediate(MI, MI.getOperand(1).getReg(), Imm,
                                            InstsBefore, InstsAfter,
                                            MI.getOperand(2).getReg());
        MI.getOperand(0).setReg(BaseReg);
        MI.getOperand(4).setImm(0);
      } else {
        // Pre-indexed store: add, bit-mask, and store
        addImmediateToRegister(MI, BaseReg, Imm, InstsBefore);
        MI.getOperand(4).setImm(0);
        doBitmasking(MI, BaseReg, InstsBefore);
      }
      break;

    // A7.7.163 Encoding T1: STRD<c> <Rt>,<Rt2>,[<Rn>],#+/-<imm8>
    case ARM::t2STRD_POST:
      BaseReg = MI.getOperand(0).getReg();
      // Post-indexed store: just bit-mask and store
      doBitmasking(MI, BaseReg, InstsBefore);
      break;

    // A7.7.256 Encoding T1: VSTR<c> <Dd>,[<Rn>{,#+/-<imm8>}]
    case ARM::VSTRD:
    // A7.7.256 Encoding T2: VSTR<c> <Sd>,[<Rn>{,#+/-<imm8>}]
    case ARM::VSTRS:
      BaseReg = MI.getOperand(1).getReg();
      // Not ZeroExtend(imm8:'00', 32) yet
      Imm = ARM_AM::getAM5Offset(MI.getOperand(2).getImm()) << 2;
      if (ARM_AM::getAM5Op(MI.getOperand(2).getImm()) == ARM_AM::AddrOpc::sub) {
        Imm = -Imm;
      }
      // For a small immediate, just bit-mask and store
      if (Imm >= -256 && Imm < 256) {
        doBitmasking(MI, BaseReg, InstsBefore);
        break;
      }
      if (BaseReg == ARM::SP) {
        // Special-case SP as it should not be incremented
        BaseReg = handleSPUncommonImmediate(MI, ARM::NoRegister, Imm,
                                            InstsBefore, InstsAfter);
        MI.getOperand(1).setReg(BaseReg);
        MI.getOperand(2).setImm(ARM_AM::getAM5Opc(ARM_AM::AddrOpc::add, 0));
      } else {
        // Add, bit-mask, store, and subtract
        addImmediateToRegister(MI, BaseReg, Imm, InstsBefore);
        MI.getOperand(2).setImm(ARM_AM::getAM5Opc(ARM_AM::AddrOpc::add, 0));
        doBitmasking(MI, BaseReg, InstsBefore);
        subtractImmediateFromRegister(MI, BaseReg, Imm, InstsAfter);
      }
      break;

    // A7.7.99 Encoding T1: PUSH<c> <registers>
    case ARM::tPUSH:
      BaseReg = ARM::SP;
      // Push: just bit-mask and store
      doBitmasking(MI, BaseReg, InstsBefore);
      break;

    // A7.7.156 Encoding T1: STM<c> <Rn>!,<registers>
    case ARM::tSTMIA_UPD:
    // A7.7.156 Encoding T2; STM<c>.W <Rn>,<registers>
    case ARM::t2STMIA:
    // A7.7.156 Encoding T2; STM<c>.W <Rn>!,<registers>
    case ARM::t2STMIA_UPD:
    // A7.7.157 Encoding T1; STMDB<c> <Rn>,<registers>
    case ARM::t2STMDB:
    // A7.7.157 Encoding T1; STMDB<c> <Rn>!,<registers>
    case ARM::t2STMDB_UPD:
    // A7.7.255 Encoding T1: VSTMDIA<c> <Rn>,<list>
    case ARM::VSTMDIA:
    // A7.7.255 Encoding T1: VSTMDIA<c> <Rn>!,<list>
    case ARM::VSTMDIA_UPD:
    // A7.7.255 Encoding T1: VSTMDDB<c> <Rn>!,<list>
    case ARM::VSTMDDB_UPD:
    // A7.7.255 Encoding T2: VSTMSIA<c> <Rn>,<list>
    case ARM::VSTMSIA:
    // A7.7.255 Encoding T2: VSTMSIA<c> <Rn>!,<list>
    case ARM::VSTMSIA_UPD:
    // A7.7.255 Encoding T2: VSTMSDB<c> <Rn>!,<list>
    case ARM::VSTMSDB_UPD:
      BaseReg = MI.getOperand(0).getReg();
      // Store multiple: just bit-mask and store
      doBitmasking(MI, BaseReg, InstsBefore);
      break;

    default:
      llvm_unreachable("Unexpected opcode!");
    }

    if (!InstsBefore.empty()) {
      insertInstsBefore(MI, InstsBefore);
    }
    if (!InstsAfter.empty()) {
      insertInstsAfter(MI, InstsAfter);
    }
  }

  unsigned long NewCodeSize = getFunctionCodeSize(MF);

  // Output code size information
  std::error_code EC;
  raw_fd_ostream MemStat("./code_size_sfi.stat", EC,
                         sys::fs::OpenFlags::F_Append);
  MemStat << MF.getName() << ":" << OldCodeSize << ":" << NewCodeSize << "\n";

  return true;
}

//
// Create a new pass.
//
namespace llvm {
  FunctionPass * createARMSilhouetteSFI(void) {
    return new ARMSilhouetteSFI();
  }
}
