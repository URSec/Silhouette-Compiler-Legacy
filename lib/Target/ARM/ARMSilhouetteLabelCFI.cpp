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
#include "ARMSilhouetteConvertFuncList.h"
#include "ARMSilhouetteLabelCFI.h"
#include "ARMTargetMachine.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/FileSystem.h"

#include <vector>

using namespace llvm;

extern bool SilhouetteInvert;
extern bool SilhouetteStr2Strt;

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
// Function: BackupReister()
//
// Description:
//   This function inserts instructions that store the content of a lo register
//   (R0 -- R7) onto the stack.
//
// Inputs:
//   MI   - A reference to the instruction before which to insert instructions.
//   Reg1 - The register to spill.
//
static void
BackupRegister(MachineInstr & MI, unsigned Reg) {
  MachineBasicBlock & MBB = *MI.getParent();
  const TargetInstrInfo * TII = MBB.getParent()->getSubtarget().getInstrInfo();

  if (SilhouetteInvert || !SilhouetteStr2Strt) {
    // Build a PUSH
    BuildMI(MBB, &MI, DL, TII->get(ARM::tPUSH))
    .add(predOps(ARMCC::AL))
    .addReg(Reg);
  } else {
    //
    // Build the following instruction sequence:
    //
    // sub  sp, #4
    // strt reg, [sp, #0]
    //
    BuildMI(MBB, &MI, DL, TII->get(ARM::tSUBspi), ARM::SP)
    .addReg(ARM::SP)
    .addImm(1)
    .add(predOps(ARMCC::AL));
    BuildMI(MBB, &MI, DL, TII->get(ARM::t2STRT))
    .addReg(Reg)
    .addReg(ARM::SP)
    .addImm(0);
  }
}

//
// Function: RestoreRegister()
//
// Description:
//   This function inserts instructions that load the content of a lo register
//   (R0 -- R7) from the stack.
//
// Inputs:
//   MI   - A reference to the instruction before which to insert instructions.
//   Reg - The register to restore.
//
static void
RestoreRegister(MachineInstr & MI, unsigned Reg) {
  MachineBasicBlock & MBB = *MI.getParent();
  const TargetInstrInfo * TII = MBB.getParent()->getSubtarget().getInstrInfo();

  // Generate a POP that pops out the register content from stack
  BuildMI(MBB, &MI, DL, TII->get(ARM::tPOP))
  .add(predOps(ARMCC::AL))
  .addReg(Reg);
}

//
// Method: insertCFILabelForCall()
//
// Description:
//   This method inserts the CFI label for call before a machine function.
//
// Input:
//   MF - A reference to the machine function.
//
void
ARMSilhouetteLabelCFI::insertCFILabelForCall(MachineFunction & MF) {
  MachineBasicBlock & MBB = *MF.begin();
  const TargetInstrInfo * TII = MF.getSubtarget().getInstrInfo();

  // Use "movs r3, r3" as our CFI label
  BuildMI(MBB, MBB.begin(), DL, TII->get(ARM::tMOVSr), ARM::R3)
  .addReg(ARM::R3);
}

//
// Method: insertCFILabelForJump()
//
// Description:
//   This method inserts the CFI label for jump before a machine basic block.
//
// Input:
//   MBB - A reference to a machine basic block.
//
void
ARMSilhouetteLabelCFI::insertCFILabelForJump(MachineBasicBlock & MBB) {
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
//   MI    - A reference to the indirect forward control-flow transfer
//           instruction.
//   Reg   - The register used by @MI.
//   Label - The correct label to check.
//
void
ARMSilhouetteLabelCFI::insertCFICheck(MachineInstr & MI, unsigned Reg,
                                      uint16_t Label) {
  MachineBasicBlock & MBB = *MI.getParent();
  const TargetInstrInfo * TII = MBB.getParent()->getSubtarget().getInstrInfo();

  //
  // Try to find a free register first.  If we are unlucky, spill and (later)
  // restore R4.
  //
  unsigned ScratchReg;
  std::deque<unsigned> FreeRegs = findFreeRegisters(MI);
  if (!FreeRegs.empty()) {
    ScratchReg = FreeRegs[0];
  } else {
    errs() << "[CFI] Unable to find a free register for " << MI;
    ScratchReg = ARM::R4;
    BackupRegister(MI, ScratchReg);
  }

  //
  // Build the following instruction sequence:
  //
  // bfc   reg, #0, #1          ; optional
  // ldrh  scratch, [reg, #0]
  // cmp   scratch, #CFI_LABEL
  // it    ne
  // bfcne reg, #0, #32
  // orr   reg, reg, #1         ; optional
  //

  //
  // Clear the LSB of @Reg for instructions like BX and BLX; ARM uses the LSB
  // to indicate an instruction set exchange between ARM and Thumb.
  //
  if (MI.getOpcode() != ARM::tBRIND) {
    BuildMI(MBB, &MI, DL, TII->get(ARM::t2BFC), Reg)
    .addReg(Reg)
    .addImm(~0x1)
    .add(predOps(ARMCC::AL));
  }
  // Load the target CFI label to @ScratchReg
  BuildMI(MBB, &MI, DL, TII->get(ARM::t2LDRHi12), ScratchReg)
  .addReg(Reg)
  .addImm(0);
  // Compare the target label with the correct label
  assert(ARM_AM::getT2SOImmVal(Label) != -1 && "Invalid value for T2SOImm!");
  BuildMI(MBB, &MI, DL, TII->get(ARM::t2CMPri))
  .addReg(ScratchReg)
  .addImm(Label);
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
    BuildMI(MBB, &MI, DL, TII->get(ARM::t2ORRri), Reg)
    .addReg(Reg)
    .addImm(0x1)
    .add(predOps(ARMCC::AL))
    .add(condCodeOp());
  }

  // Restore the scratch register if we spilled it
  if (FreeRegs.empty()) {
    RestoreRegister(MI, ScratchReg);
  }
}

//
// Method: insertCFICheckForCall()
//
// Description:
//   This method inserts a CFI check before a specified indirect call
//   instruction that calls a target function in a register.
//
// Inputs:
//   MI    - A reference to the indirect call instruction.
//   Reg   - The register used by @MI.
//   Label - The correct label to check.
//
void
ARMSilhouetteLabelCFI::insertCFICheckForCall(MachineInstr & MI, unsigned Reg) {
  insertCFICheck(MI, Reg, CFI_LABEL_CALL);
}

//
// Method: insertCFICheckForJump()
//
// Description:
//   This method inserts a CFI check before a specified indirect jump
//   instruction that jumps to a target in a register.
//
// Inputs:
//   MI    - A reference to the indirect jump instruction.
//   Reg   - The register used by @MI.
//   Label - The correct label to check.
//
void
ARMSilhouetteLabelCFI::insertCFICheckForJump(MachineInstr & MI, unsigned Reg) {
  insertCFICheck(MI, Reg, CFI_LABEL_JMP);
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

  unsigned long OldCodeSize = getFunctionCodeSize(MF);

  //
  // Iterate through all the instructions within the function to locate
  // indirect branches and calls.
  //
  std::vector<MachineInstr *> IndirectBranches;
  std::vector<MachineInstr *> JTJs;
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
        JTJs.push_back(&MI);
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

#if 1
  //
  // Insert a CFI label before the function if it is visible to other
  // compilation units or has its address taken.
  //
  const Function & F = MF.getFunction();
  if ((!F.hasInternalLinkage() && !F.hasPrivateLinkage()) ||
      F.hasAddressTaken()) {
    if (MF.begin() != MF.end()) {
      insertCFILabelForCall(MF);
    }
  }
#else
  // Insert a CFI label before the function
  if (MF.begin() != MF.end()) {
    insertCFILabel(MF);
  }
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
        insertCFILabelForJump(*SuccMBB);
      }
      insertCFICheckForJump(*MI, MI->getOperand(0).getReg());
      break;

    case ARM::tBLXr:      // 0: predCC, 1: predReg, 2: GPR
    case ARM::tBLXNSr:    // 0: predCC, 1: predReg, 2: GPRnopc
      insertCFICheckForCall(*MI, MI->getOperand(2).getReg());
      break;

    case ARM::tBX_CALL:   // 0: tGPR
    case ARM::tTAILJMPr:  // 0: tcGPR
      insertCFICheckForCall(*MI, MI->getOperand(0).getReg());
      break;

    default:
      llvm_unreachable("Unexpected opcode");
    }
  }

  unsigned long NewCodeSize = getFunctionCodeSize(MF);

  // Output code size information
  std::error_code EC;
  raw_fd_ostream MemStat("./code_size_cfi.stat", EC,
                         sys::fs::OpenFlags::F_Append);
  MemStat << MF.getName() << ":" << OldCodeSize << ":" << NewCodeSize << "\n";

  // Output jump table jump information
  raw_fd_ostream JTJStat("./jump_table_jump.stat", EC,
                         sys::fs::OpenFlags::F_Append);
  for (MachineInstr * MI : JTJs) {
    JTJStat << MI->getMF()->getName() << "\n";
  }

  return true;
}

namespace llvm {
  FunctionPass * createARMSilhouetteLabelCFI(void) {
    return new ARMSilhouetteLabelCFI();
  }
}
