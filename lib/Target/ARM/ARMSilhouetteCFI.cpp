//===-- ARMSilhouetteCFI - Minimal Forward Control-Flow Integrity ---------===//
//
//         Protecting Control Flow of Real-time OS applications
//
// This file was written by at the University of Rochester.
// All Rights Reserved.
//
//===----------------------------------------------------------------------===//
//
// This pass implements the minimal control-flow integrity for forward indirect
// control-flow transfer instructions on ARM.
//
//===----------------------------------------------------------------------===//
//

#include "ARM.h"
#include "ARMSilhouetteCFI.h"
#include "ARMSilhouetteConvertFuncList.h"
#include "ARMTargetMachine.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"

#include <vector>

// Macros for extracting and flipping the LSB
#define LSB(num)        ((num) & 0x1)
#define InvertLSB(num)  ((num) ^ 0x1)

using namespace llvm;

static DebugLoc DL;

char ARMSilhouetteCFI::ID = 0;

ARMSilhouetteCFI::ARMSilhouetteCFI()
    : MachineFunctionPass(ID) {
}

StringRef
ARMSilhouetteCFI::getPassName() const {
  return "ARM Silhouette Forward CFI Pass";
}

//
// Function: FindPrecedingIT()
//
// Description:
//   This function searches for an IT instruction that covers the specified
//   instruction in its IT block.
//
// Inputs:
//   MI - A reference to the specified instruction.
//   CC - The condition code of @MI.
//
// Outputs:
//   Rank   - If there is an IT, which of the up-to-four instructions @MI is.
//   IsFull - If there is an IT, whether the IT block is full.
//
// Return value:
//   A pointer to the IT instruction on success, or nullptr if there is no
//   such IT instruction.
//
static MachineInstr *
FindPrecedingIT(MachineInstr & MI, ARMCC::CondCodes CC, unsigned & Rank,
                bool & IsFull) {
  // Check if IT is the preceding instruction
  MachineInstr * PrevMI = MI.getPrevNode();
  if (PrevMI == nullptr) {
    return nullptr;
  }
  if (PrevMI->getOpcode() == ARM::t2IT) {
    unsigned Mask = PrevMI->getOperand(1).getImm() & 0xf;
    ARMCC::CondCodes Cond1 = (ARMCC::CondCodes)PrevMI->getOperand(0).getImm();
    assert(Cond1 == CC && "Unmatched condition code!");
    Rank = 1;
    IsFull = LSB(Mask) == 1;
    return PrevMI;
  }

  // Check if IT is preceding by 2 instructions
  PrevMI = PrevMI->getPrevNode();
  if (PrevMI == nullptr) {
    return nullptr;
  }
  if (PrevMI->getOpcode() == ARM::t2IT) {
    unsigned Mask = PrevMI->getOperand(1).getImm() & 0xf;
    if ((Mask & 0x7) == 0) {
      return nullptr;
    }

    ARMCC::CondCodes Cond1 = (ARMCC::CondCodes)PrevMI->getOperand(0).getImm();
    ARMCC::CondCodes Cond2 = LSB(Cond1) == LSB(Mask >> 3) ?
                             Cond1 : (ARMCC::CondCodes)InvertLSB(Cond1);
    assert(Cond2 == CC && "Unmatched condition code!");
    Rank = 2;
    IsFull = LSB(Mask) == 1;
    return PrevMI;
  }

  // Check if IT is preceding by 3 instructions
  PrevMI = PrevMI->getPrevNode();
  if (PrevMI == nullptr) {
    return nullptr;
  }
  if (PrevMI->getOpcode() == ARM::t2IT) {
    unsigned Mask = PrevMI->getOperand(1).getImm() & 0xf;
    if ((Mask & 0x3) == 0) {
      return nullptr;
    }

    ARMCC::CondCodes Cond1 = (ARMCC::CondCodes)PrevMI->getOperand(0).getImm();
    ARMCC::CondCodes Cond3 = LSB(Cond1) == LSB(Mask >> 2) ?
                             Cond1 : (ARMCC::CondCodes)InvertLSB(Cond1);
    assert(Cond3 == CC && "Unmatched condition code!");
    Rank = 3;
    IsFull = LSB(Mask) == 1;
    return PrevMI;
  }

  // Check if IT is preceding by 4 instructions
  PrevMI = PrevMI->getPrevNode();
  if (PrevMI != nullptr && PrevMI->getOpcode() == ARM::t2IT) {
    unsigned Mask = PrevMI->getOperand(1).getImm() & 0xf;
    if (LSB(Mask) == 0) {
      return nullptr;
    }

    ARMCC::CondCodes Cond1 = (ARMCC::CondCodes)PrevMI->getOperand(0).getImm();
    ARMCC::CondCodes Cond4 = LSB(Cond1) == LSB(Mask >> 1) ?
                             Cond1 : (ARMCC::CondCodes)InvertLSB(Cond1);
    assert(Cond4 == CC && "Unmatched condition code!");
    Rank = 4;
    IsFull = true;
    return PrevMI;
  }

  return nullptr;
}

//
// Function: SplitFullITBlock()
//
// Description:
//   This function splits a full IT block that has an indirect forward
//   control-flow transfer instruction as the fourth (last) instruction in the
//   IT block.  It changes the mask of the old IT and inserts a new IT before
//   the control-flow transfer instruction.
//
// Inputs:
//   OldIT - A reference to the old IT instruction.
//   MI    - A reference to the indirect forward control-flow transfer
//           instruction.
//   CC    - The condition code of @MI.
//
// Return value:
//   A pointer to the new IT instruction.
//
static MachineInstr *
SplitFullITBlock(MachineInstr & OldIT, MachineInstr & MI, ARMCC::CondCodes CC) {
  MachineBasicBlock & MBB = *MI.getParent();
  const TargetInstrInfo * TII = MBB.getParent()->getSubtarget().getInstrInfo();

  // Change the old IT to cover only 3 instructions
  MachineOperand & OldMaskMO = OldIT.getOperand(1);
  unsigned Mask = OldMaskMO.getImm() & 0xf;
  Mask |= 0x2;
  Mask &= ~0x1;
  OldMaskMO.setImm(Mask);

  // Insert a new IT that covers @MI
  BuildMI(MBB, &MI, DL, TII->get(ARM::t2IT))
  .addImm(CC)
  .addImm(0x8);
}

//
// Function: InsertBFC()
//
// Description:
//   This function inserts a BFC instruction before an indirect forward
//   control-flow transfer instruction to ensure that the target is aligned at
//   4-byte boundaries.
//
// Inputs:
//   MI  - A reference to the indirect forward control-flow transfer
//         instruction.
//   Reg - The register that @MI uses.
//
static void
InsertBFC(MachineInstr & MI, unsigned Reg) {
  MachineBasicBlock & MBB = *MI.getParent();
  const TargetInstrInfo * TII = MBB.getParent()->getSubtarget().getInstrInfo();

  // Insert a BFC to clear the second LSB
  AddDefaultPred(
    BuildMI(MBB, &MI, DL, TII->get(ARM::t2BFC), Reg)
    .addReg(Reg)
    .addImm(~0x2) // Don't clear the LSB; BX and BLX use it to exchange
                  // instruction set.  For others, we assume it's cleared.
  );
}

//
// Function: InsertBFCWithinITBlock()
//
// Description:
//   This function inserts a BFC instruction before an indirect forward
//   control-flow transfer instruction in an IT block that has at most 3
//   instructions.
//
// Inputs:
//   IT   - A reference to the IT instruction.
//   MI   - A reference to the indirect forward control-flow transfer
//          instruction.
//   CC   - The condition code of @MI.
//   Rank - Which of the up-to-four instructions @MI is.
//   Reg  - The register that @MI uses.
//
static void
InsertBFCWithinITBlock(MachineInstr & IT, MachineInstr & MI,
                       ARMCC::CondCodes CC, unsigned Rank, unsigned Reg) {
  MachineBasicBlock & MBB = *MI.getParent();
  const TargetInstrInfo * TII = MBB.getParent()->getSubtarget().getInstrInfo();

  assert(Rank != 4 && "Cannot insert AND into a full IT block!");

  // Change the IT to cover the BFC to be added
  MachineOperand & MaskMO = IT.getOperand(1);
  unsigned Mask = MaskMO.getImm() & 0xf;
  if (Rank != 3) {
    Mask >>= 1;
  } else {
    Mask |= 0x1;
  }
  Mask &= LSB(CC) << (3 - Rank);
  Mask |= LSB(CC) << (3 - Rank);
  MaskMO.setImm(Mask);

  // Insert a BFC to clear the second LSB
  BuildMI(MBB, &MI, DL, TII->get(ARM::t2BFC), Reg)
  .addReg(Reg)
  .addImm(~0x2) // Don't clear the LSB; BX and BLX use it to exchange
                // instruction set.  For others, we assume it's cleared.
  .addImm(CC).addReg(ARM::NoRegister);
}

//
// Function: BitMaskIndirectBranchCall()
//
// Description:
//   This function deals with inserting bit-masking instructions before
//   indirect forward control-flow transfer instructions of each kind.
//
// Input:
//   MI - A reference to the indirect forward control-flow transfer instruction.
//
static void
BitMaskIndirectBranchCall(MachineInstr & MI) {
  switch (MI.getOpcode()) {
  case ARM::tBRIND:     // 0: GPR, 1: predCC, 2: predReg
  case ARM::tBX:        // 0: GPR, 1: predCC, 2: predReg
  case ARM::tBXNS: {    // 0: GPR, 1: predCC, 2: predReg
    unsigned PredReg;
    ARMCC::CondCodes CC = getInstrPredicate(MI, PredReg);
    unsigned Rank;
    bool IsFull;
    MachineInstr * IT = FindPrecedingIT(MI, CC, Rank, IsFull);
    if (IT != nullptr) {
      if (IsFull) {
        assert(Rank == 4 && "Indirect call in the middle of IT block!");
        IT = SplitFullITBlock(*IT, MI, CC);
        Rank = 1;
      }
      InsertBFCWithinITBlock(*IT, MI, CC, Rank, MI.getOperand(0).getReg());
    } else {
      InsertBFC(MI, MI.getOperand(0).getReg());
    }
    break;
  }

  // Unfortunately, jump table jumps are complicated and not dealt with for now
  case ARM::tBR_JTr:    // 0: tGPR, 1: i32imm
  case ARM::tTBB_JT:    // 0: tGPR, 1: tGPR, 2: i32imm, 3: i32imm
  case ARM::tTBH_JT:    // 0: tGPR, 1: tGPR, 2: i32imm, 3: i32imm
  case ARM::t2BR_JT:    // 0: GPR, 1: GPR, 2: i32imm
  case ARM::t2TBB_JT:   // 0: GPR, 1: GPR, 2: i32imm, 3: i32imm
  case ARM::t2TBH_JT:   // 0: GPR, 1: GPR, 2: i32imm, 3: i32imm
    break;

  case ARM::tBLXr:      // 0: predCC, 1: predReg, 2: GPR
  case ARM::tBLXNSr: {  // 0: predCC, 1: predReg, 2: GPRnopc
    unsigned PredReg;
    ARMCC::CondCodes CC = getInstrPredicate(MI, PredReg);
    unsigned Rank;
    bool IsFull;
    MachineInstr * IT = FindPrecedingIT(MI, CC, Rank, IsFull);
    if (IT != nullptr) {
      if (IsFull) {
        assert(Rank == 4 && "Indirect call in the middle of IT block!");
        IT = SplitFullITBlock(*IT, MI, CC);
        Rank = 1;
      }
      InsertBFCWithinITBlock(*IT, MI, CC, Rank, MI.getOperand(2).getReg());
    } else {
      InsertBFC(MI, MI.getOperand(2).getReg());
    }
    break;
  }

  case ARM::tBX_CALL:   // 0: tGPR
  case ARM::tTAILJMPr:  // 0: tcGPR
    InsertBFC(MI, MI.getOperand(0).getReg());
    break;

  default:
    llvm_unreachable("Unexpected opcode");
  }
}

//
// Method: runOnMachineFunction()
//
// Description:
//   This method is called when the PassManager wants this pass to transform
//   the specified MachineFunction.  This method .
//
// Input:
//   MF - A reference to the MachineFunction to transform.
//
// Output:
//   MF - The transformed MachineFunction.
//
// Return value:
//   true  - The MachineFunction was transformed.
//   false - The MachineFunction was not transformed.
//
bool
ARMSilhouetteCFI::runOnMachineFunction(MachineFunction & MF) {
#if 0
  // Skip certain functions
  if (funcBlacklist.find(MF.getName()) != funcBlacklist.end()) {
    return false;
  }
#endif

  //
  // Iterate through all the instructions within the function to locate
  // indirect branches and calls.
  //
  std::vector<MachineInstr *> IndirectBranches;
  std::vector<MachineInstr *> IndirectCalls;
  for (MachineBasicBlock & MBB : MF) {
    for (MachineInstr & MI : MBB) {
      switch (MI.getOpcode()) {
      // Indirect branch
      case ARM::tBRIND:     // 0: GPR, 1: predCC, 2: predReg
      case ARM::tBX:        // 0: GPR, 1: predCC, 2: predReg
      case ARM::tBXNS:      // 0: GPR, 1: predCC, 2: predReg
      case ARM::tBR_JTr:    // 0: tGPR, 1: i32imm
      case ARM::tTBB_JT:    // 0: tGPR, 1: tGPR, 2: i32imm, 3: i32imm
      case ARM::tTBH_JT:    // 0: tGPR, 1: tGPR, 2: i32imm, 3: i32imm
      case ARM::t2BR_JT:    // 0: GPR, 1: GPR, 2: i32imm
      case ARM::t2TBB_JT:   // 0: GPR, 1: GPR, 2: i32imm, 3: i32imm
      case ARM::t2TBH_JT:   // 0: GPR, 1: GPR, 2: i32imm, 3: i32imm
        IndirectBranches.push_back(&MI);
        break;

      // Indirect call
      case ARM::tBLXr:      // 0: predCC, 1: predReg, 2: GPR
      case ARM::tBLXNSr:    // 0: predCC, 1: predReg, 2: GPRnopc
      case ARM::tBX_CALL:   // 0: tGPR
      case ARM::tTAILJMPr:  // 0: tcGPR
        IndirectCalls.push_back(&MI);
        break;

      //
      // Also list direct {function, system, hyper} calls here to make the
      // default branch be able to use MI.isCall().
      //
      case ARM::tBL:
      case ARM::tBLXi:
      case ARM::tTAILJMPd:
      case ARM::tTAILJMPdND:
      case ARM::tSVC:
      case ARM::t2SMC:
      case ARM::t2HVC:
        break;

      default:
        if (MI.isIndirectBranch() || MI.isCall()) {
          errs() << "[CFI]: unidentified branch/call: " << MI;
        }
        break;
      }
    }
  }

  //
  // Align each machine basic block at 4-byte boundaries if there is an
  // indirect branch.
  //
  if (!IndirectBranches.empty()) {
    for (MachineBasicBlock & MBB : MF) {
      if (MBB.getAlignment() < 2u) {
        MBB.setAlignment(2u);
      }
    }
  }

  //
  // Align the function at 4-byte boundaries if it is visible to other
  // compilation units or has its address taken.
  //
  const Function * F = MF.getFunction();
  if ((!F->hasInternalLinkage() && !F->hasPrivateLinkage()) ||
      F->hasAddressTaken()) {
    if (MF.getAlignment() < 2u) {
      MF.setAlignment(2u);
    }
  }

  //
  // Insert a bit-masking instruction before each indirect branch and call to
  // align the control-flow target at 4-byte boundaries.
  //
  for (MachineInstr * MI : IndirectBranches) {
    BitMaskIndirectBranchCall(*MI);
  }
  for (MachineInstr * MI : IndirectCalls) {
    BitMaskIndirectBranchCall(*MI);
  }

  return true;
}

namespace llvm {
  FunctionPass * createARMSilhouetteCFI(void) {
    return new ARMSilhouetteCFI();
  }
}
