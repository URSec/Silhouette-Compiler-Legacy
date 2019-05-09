//===-- ARMSilhouetteLabelCFI - Label-Based Forward Control-Flow Integrity ===//
//
//         Protecting Control Flow of Real-time OS applications
//
// This file was written by at the University of Rochester.
// All Rights Reserved.
//
//===----------------------------------------------------------------------===//
//
// This pass implements the label-based single-label control-flow integrity for
// forward indirect control-flow transfer instructions on ARM.
//
//===----------------------------------------------------------------------===//
//

#include "ARM.h"
#include "ARMSilhouetteLabelCFI.h"
#include "ARMSilhouetteConvertFuncList.h"
#include "ARMTargetMachine.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"

#include <vector>

using namespace llvm;

static DebugLoc DL;

char ARMSilhouetteLabelCFI::ID = 0;

ARMSilhouetteLabelCFI::ARMSilhouetteLabelCFI()
    : MachineFunctionPass(ID) {
}

StringRef
ARMSilhouetteLabelCFI::getPassName() const {
  return "ARM Silhouette Label-Based Forward CFI Pass";
}

//
// Function: BackupReisters()
//
// Description:
//   This function inserts instructions that store the content of two lo
//   registers (R0 -- R7) onto the stack.  The second register should be
//   greater than the first one.
//
// Inputs:
//   MI   - A reference to the instruction before which to insert instructions.
//   Reg1 - The first register to spill.
//   Reg2 - The second register to spill.
//
static void
BackupRegisters(MachineInstr & MI, unsigned Reg1, unsigned Reg2) {
  MachineBasicBlock & MBB = *MI.getParent();
  const TargetInstrInfo * TII = MBB.getParent()->getSubtarget().getInstrInfo();

  //
  // Build the following instruction sequence:
  //
  // sub  sp, #8
  // strt reg1, [sp, #0]
  // strt reg1, [sp, #4]
  //
  AddDefaultPred(
    BuildMI(MBB, &MI, DL, TII->get(ARM::tSUBspi), ARM::SP)
    .addReg(ARM::SP)
    .addImm(2)
  );
  BuildMI(MBB, &MI, DL, TII->get(ARM::t2STRT), Reg1)
  .addReg(ARM::SP)
  .addImm(0);
  BuildMI(MBB, &MI, DL, TII->get(ARM::t2STRT), Reg2)
  .addReg(ARM::SP)
  .addImm(4);
}

//
// Function: RestoreRegisters()
//
// Description:
//   This function inserts instructions that load the content of two lo
//   registers (R0 -- R7) from the stack.  The second register should be
//   greater than the first one.
//
// Inputs:
//   MI   - A reference to the instruction before which to insert instructions.
//   Reg1 - The first register to restore.
//   Reg2 - The second register to restore.
//
static void
RestoreRegisters(MachineInstr & MI, unsigned Reg1, unsigned Reg2) {
  MachineBasicBlock & MBB = *MI.getParent();
  const TargetInstrInfo * TII = MBB.getParent()->getSubtarget().getInstrInfo();

  // Generate a POP that pops out the register content from stack
  AddDefaultPred(BuildMI(MBB, &MI, DL, TII->get(ARM::tPOP)))
  .addReg(Reg1)
  .addReg(Reg2);
}

//
// Method: insertCFILabel()
//
// Description:
//   This method inserts the CFI label before a machine function.
//
// Input:
//   MF - A reference to the machine function.
//
void
ARMSilhouetteLabelCFI::insertCFILabel(MachineFunction & MF) {
  auto MBB = MF.begin();

  if (MBB != MF.end()) {
    insertCFILabel(*MBB);
  }
}

//
// Method: insertCFILabel()
//
// Description:
//   This method inserts the CFI label before a machine basic block.
//
// Input:
//   MBB - A reference to a machine basic block.
//
void
ARMSilhouetteLabelCFI::insertCFILabel(MachineBasicBlock & MBB) {
  const TargetInstrInfo * TII = MBB.getParent()->getSubtarget().getInstrInfo();

  // Use "mov r0, r0" as our CFI label
  BuildMI(MBB, MBB.begin(), DL, TII->get(ARM::tMOVr), ARM::R0)
  .addReg(ARM::R0);
}

//
// Method: insertCFICheck()
//
// Description:
//   This method inserts a CFI check before a specified indirect forward
//   control-flow transfer instruction that jumps to a target in a register.
//
// Inputs:
//   MI  - A reference to the indirect forward control-flow transfer
//         instruction.
//   Reg - The register used by @MI.
//
void
ARMSilhouetteLabelCFI::insertCFICheck(MachineInstr & MI, unsigned Reg) {
  MachineBasicBlock & MBB = *MI.getParent();
  const TargetInstrInfo * TII = MBB.getParent()->getSubtarget().getInstrInfo();

  unsigned ScratchReg1 = Reg == ARM::R5 ? ARM::R4 : ARM::R5;
  unsigned ScratchReg2 = Reg == ARM::R7 ? ARM::R6 : ARM::R7;

  // Backup two scratch registers so that they are free to use
  BackupRegisters(MI, ScratchReg1, ScratchReg2);

  //
  // Build the following instruction sequence:
  //
  // bfc   reg, #0, #1          ; optional
  // movw  scratch1, #CFI_LABEL
  // ldrh  scratch2, [reg, #0]
  // cmp   scratch1, scratch2
  // ite   ne
  // bfcne reg, #0, #32
  // orr   reg, reg, #1         ; optional
  //

  //
  // Clear the LSB of @Reg for instructions like BX and BLX; ARM uses the LSB
  // to indicate an instruction set exchange between ARM and Thumb.
  //
  if (MI.getOpcode() != ARM::tBRIND) {
    AddDefaultPred(
      BuildMI(MBB, &MI, DL, TII->get(ARM::t2BFC), Reg)
      .addReg(Reg)
      .addImm(~0x1)
    );
  }
  // Load the correct CFI label to @ScratchReg1
  BuildMI(MBB, &MI, DL, TII->get(ARM::t2MOVi16), ScratchReg1)
  .addImm(CFI_LABEL);
  // Load the target CFI label to @ScratchReg2
  BuildMI(MBB, &MI, DL, TII->get(ARM::t2LDRHi12), ScratchReg2)
  .addReg(Reg)
  .addImm(0);
  // Compare two labels
  BuildMI(MBB, &MI, DL, TII->get(ARM::tCMPr))
  .addReg(ScratchReg1)
  .addReg(ScratchReg2);
  // Clear all the bits of @Reg if two labels are not equal (a CFI violation)
  BuildMI(MBB, &MI, DL, TII->get(ARM::t2IT))
  .addImm(ARMCC::NE)
  .addImm(0x8);
  BuildMI(MBB, &MI, DL, TII->get(ARM::t2BFC), Reg)
  .addReg(Reg)
  .addImm(0)
  .addImm(ARMCC::NE).addReg(ARM::CPSR, RegState::Kill);
  // Set the LSB of @Reg for instructions like BX and BLX
  if (MI.getOpcode() != ARM::tBRIND) {
    AddDefaultCC(
      AddDefaultPred(
        BuildMI(MBB, &MI, DL, TII->get(ARM::t2ORRri), Reg)
        .addReg(Reg)
        .addImm(0x1)
      )
    );
  }

  // Restore the two scratch registers
  RestoreRegisters(MI, ScratchReg1, ScratchReg2);
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
ARMSilhouetteLabelCFI::runOnMachineFunction(MachineFunction & MF) {
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
  for (MachineBasicBlock & MBB : MF) {
    for (MachineInstr & MI : MBB) {
      switch (MI.getOpcode()) {
      // Indirect branch
      case ARM::tBRIND:     // 0: GPR, 1: predCC, 2: predReg
      case ARM::tBX:        // 0: GPR, 1: predCC, 2: predReg
      case ARM::tBXNS:      // 0: GPR, 1: predCC, 2: predReg
      // Indirect call
      case ARM::tBLXr:      // 0: predCC, 1: predReg, 2: GPR
      case ARM::tBLXNSr:    // 0: predCC, 1: predReg, 2: GPRnopc
      case ARM::tBX_CALL:   // 0: tGPR
      case ARM::tTAILJMPr:  // 0: tcGPR
        IndirectBranches.push_back(&MI);
        break;

      // Jump table jump is complicated and not dealt with for now
      case ARM::tBR_JTr:    // 0: tGPR, 1: i32imm
      case ARM::tTBB_JT:    // 0: tGPR, 1: tGPR, 2: i32imm, 3: i32imm
      case ARM::tTBH_JT:    // 0: tGPR, 1: tGPR, 2: i32imm, 3: i32imm
      case ARM::t2BR_JT:    // 0: GPR, 1: GPR, 2: i32imm
      case ARM::t2TBB_JT:   // 0: GPR, 1: GPR, 2: i32imm, 3: i32imm
      case ARM::t2TBH_JT:   // 0: GPR, 1: GPR, 2: i32imm, 3: i32imm
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

#if 0
  //
  // Insert a CFI label before the function if it is visible to other
  // compilation units or has its address taken.
  //
  const Function * F = MF.getFunction();
  if ((!F->hasInternalLinkage() && !F->hasPrivateLinkage()) ||
      F->hasAddressTaken()) {
    insertCFILabel(MF);
  }
#else
  // Insert a CFI label before the function
  insertCFILabel(MF);
#endif

  //
  // Insert a CFI check before each indirect branch and call, and insert a CFI
  // label before every successor MBB of each indirect branch.
  //
  for (MachineInstr * MI : IndirectBranches) {
    switch (MI->getOpcode()) {
    case ARM::tBRIND:     // 0: GPR, 1: predCC, 2: predReg
    case ARM::tBX:        // 0: GPR, 1: predCC, 2: predReg
    case ARM::tBXNS:      // 0: GPR, 1: predCC, 2: predReg
      for (MachineBasicBlock * SuccMBB : MI->getParent()->successors()) {
        insertCFILabel(*SuccMBB);
      }
      insertCFICheck(*MI, MI->getOperand(0).getReg());
      break;

    case ARM::tBLXr:      // 0: predCC, 1: predReg, 2: GPR
    case ARM::tBLXNSr:    // 0: predCC, 1: predReg, 2: GPRnopc
      insertCFICheck(*MI, MI->getOperand(2).getReg());
      break;

    case ARM::tBX_CALL:   // 0: tGPR
    case ARM::tTAILJMPr:  // 0: tcGPR
      insertCFICheck(*MI, MI->getOperand(0).getReg());
      break;

    default:
      llvm_unreachable("Unexpected opcode");
    }
  }

  return true;
}

namespace llvm {
  FunctionPass * createARMSilhouetteLabelCFI(void) {
    return new ARMSilhouetteLabelCFI();
  }
}
