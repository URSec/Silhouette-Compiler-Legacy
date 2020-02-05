//===- ARMSilhouetteShadowStack - Modify Prologue/Epilogue for Shadow Stack ==//
//
//         Protecting Control Flow of Real-time OS applications
//
// This file was written by at the University of Rochester.
// All Rights Reserved.
//
//===----------------------------------------------------------------------===//
//
// This pass instruments the function prologue/epilogue to save/load the return
// address from a parallel shadow stack.
//
//===----------------------------------------------------------------------===//
//

#include "ARM.h"
#include "ARMBaseInstrInfo.h"
#include "ARMSilhouetteConvertFuncList.h"
#include "ARMSilhouetteShadowStack.h"
#include "ARMTargetMachine.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/FileSystem.h"

#include <deque>

using namespace llvm;

extern bool SilhouetteInvert;

char ARMSilhouetteShadowStack::ID = 0;

static DebugLoc DL;

static cl::opt<int>
ShadowStackOffset("arm-silhouette-shadowstack-offset",
                  cl::desc("Silhouette shadow stack offset"),
                  cl::init(14680064), cl::Hidden);

ARMSilhouetteShadowStack::ARMSilhouetteShadowStack()
    : MachineFunctionPass(ID) {
  return;
}

StringRef
ARMSilhouetteShadowStack::getPassName() const {
  return "ARM Silhouette Shadow Stack Pass";
}

//
// Function: findTailJmp()
//
// Description:
//   This function finds a TAILJMP instruction after a given instruction MI in
//   the same basic block.
//
// Input:
//   MI - A reference to the instruction after which to find TAILJMP.
//
// Return value:
//   A pointer to TAILJMP if found, nullptr otherwise.
//
static MachineInstr *
findTailJmp(MachineInstr & MI) {
  MachineInstr * I = MI.getNextNode();
  while (I != nullptr) {
    switch (I->getOpcode()) {
    case ARM::tTAILJMPr:
    case ARM::tTAILJMPd:
    case ARM::tTAILJMPdND:
    case ARM::tBX_RET:  // This is also the case!
      return I;

    default:
      I = I->getNextNode();
      break;
    }
  }

  return nullptr;
}

//
// Method: setupShadowStack()
//
// Description:
//   This method inserts instructions that store the return address onto the
//   shadow stack.
//
// Input:
//   MI - A reference to a PUSH instruction before which to insert instructions.
//
void
ARMSilhouetteShadowStack::setupShadowStack(MachineInstr & MI) {
  MachineFunction & MF = *MI.getMF();
  const TargetInstrInfo * TII = MF.getSubtarget().getInstrInfo();

  int offset = ShadowStackOffset;

  unsigned PredReg;
  ARMCC::CondCodes Pred = getInstrPredicate(MI, PredReg);

  std::deque<MachineInstr *> NewMIs;

  if (offset >= 0 && offset <= 4092 && !SilhouetteInvert) {
    // Single-instruction shortcut
    NewMIs.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRi12))
                     .addReg(ARM::LR)
                     .addReg(ARM::SP)
                     .addImm(offset)
                     .add(predOps(Pred, PredReg))
                     .setMIFlag(MachineInstr::ShadowStack));
  } else {
    // First encode the shadow stack offset into the scratch register
    if (ARM_AM::getT2SOImmVal(offset) != -1) {
      // Use one MOV if the offset can be expressed in Thumb modified constant
      NewMIs.push_back(BuildMI(MF, DL, TII->get(ARM::t2MOVi), ARM::R12)
                       .addImm(offset)
                       .add(predOps(Pred, PredReg))
                       .add(condCodeOp()) // No 'S' bit
                       .setMIFlag(MachineInstr::ShadowStack));
    } else {
      // Otherwise use MOV/MOVT to load lower/upper 16 bits of the offset
      NewMIs.push_back(BuildMI(MF, DL, TII->get(ARM::t2MOVi16), ARM::R12)
                       .addImm(offset & 0xffff)
                       .add(predOps(Pred, PredReg))
                       .setMIFlag(MachineInstr::ShadowStack));
      if ((offset >> 16) != 0) {
        NewMIs.push_back(BuildMI(MF, DL, TII->get(ARM::t2MOVTi16), ARM::R12)
                         .addReg(ARM::R12)
                         .addImm(offset >> 16)
                         .add(predOps(Pred, PredReg))
                         .setMIFlag(MachineInstr::ShadowStack));
      }
    }

    // Store the return address onto the shadow stack
    if (SilhouetteInvert) {
      // Add SP with the offset to the scratch register
      NewMIs.push_back(BuildMI(MF, DL, TII->get(ARM::tADDrSP), ARM::R12)
                       .addReg(ARM::SP)
                       .addReg(ARM::R12)
                       .add(predOps(Pred, PredReg))
                       .setMIFlag(MachineInstr::ShadowStack));
      // Generate an STRT to the shadow stack
      NewMIs.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRT))
                       .addReg(ARM::LR)
                       .addReg(ARM::R12)
                       .addImm(0)
                       .add(predOps(Pred, PredReg))
                       .setMIFlag(MachineInstr::ShadowStack));
    } else {
      // Generate an STR to the shadow stack
      NewMIs.push_back(BuildMI(MF, DL, TII->get(ARM::t2STRs))
                       .addReg(ARM::LR)
                       .addReg(ARM::SP)
                       .addReg(ARM::R12)
                       .addImm(0)
                       .add(predOps(Pred, PredReg))
                       .setMIFlag(MachineInstr::ShadowStack));
    }
  }

  // Now insert these new instructions into the basic block
  insertInstsBefore(MI, NewMIs);
}

//
// Method: popFromShadowStack()
//
// Description:
//   This method modifies a POP instruction to not write to PC/LR and inserts
//   new instructions that load the return address from the shadow stack into
//   PC/LR.
//
// Input:
//   MI   - A reference to a POP instruction after which to insert instructions.
//   PCLR - A reference to the PC or LR operand of the POP.
//
void
ARMSilhouetteShadowStack::popFromShadowStack(MachineInstr & MI,
                                             MachineOperand & PCLR) {
  MachineFunction & MF = *MI.getMF();
  const TargetInstrInfo * TII = MF.getSubtarget().getInstrInfo();

  int offset = ShadowStackOffset;

  unsigned PredReg;
  ARMCC::CondCodes Pred = getInstrPredicate(MI, PredReg);

  std::deque<MachineInstr *> NewMIs;

  // Adjust SP to skip PC/LR on the stack
  NewMIs.push_back(BuildMI(MF, DL, TII->get(ARM::tADDspi), ARM::SP)
                   .addReg(ARM::SP)
                   .addImm(1)
                   .add(predOps(Pred, PredReg))
                   .setMIFlag(MachineInstr::ShadowStack));

  if (offset >= 0 && offset <= 4092) {
    // Single-instruction shortcut
    NewMIs.push_back(BuildMI(MF, DL, TII->get(ARM::t2LDRi12), PCLR.getReg())
                     .addReg(ARM::SP)
                     .addImm(offset)
                     .add(predOps(Pred, PredReg))
                     .setMIFlag(MachineInstr::ShadowStack));
  } else {
    // First encode the shadow stack offset into the scratch register
    if (ARM_AM::getT2SOImmVal(offset) != -1) {
      // Use one MOV if the offset can be expressed in Thumb modified constant
      NewMIs.push_back(BuildMI(MF, DL, TII->get(ARM::t2MOVi), ARM::R12)
                       .addImm(offset)
                       .add(predOps(Pred, PredReg))
                       .add(condCodeOp()) // No 'S' bit
                       .setMIFlag(MachineInstr::ShadowStack));
    } else {
      // Otherwise use MOV/MOVT to load lower/upper 16 bits of the offset
      NewMIs.push_back(BuildMI(MF, DL, TII->get(ARM::t2MOVi16), ARM::R12)
                       .addImm(offset & 0xffff)
                       .add(predOps(Pred, PredReg))
                       .setMIFlag(MachineInstr::ShadowStack));
      if ((offset >> 16) != 0) {
        NewMIs.push_back(BuildMI(MF, DL, TII->get(ARM::t2MOVTi16), ARM::R12)
                         .addReg(ARM::R12)
                         .addImm(offset >> 16)
                         .add(predOps(Pred, PredReg))
                         .setMIFlag(MachineInstr::ShadowStack));
      }
    }

    // Generate an LDR from the shadow stack to PC/LR
    NewMIs.push_back(BuildMI(MF, DL, TII->get(ARM::t2LDRs), PCLR.getReg())
                     .addReg(ARM::SP)
                     .addReg(ARM::R12)
                     .addImm(0)
                     .add(predOps(Pred, PredReg))
                     .setMIFlag(MachineInstr::ShadowStack));
  }

  // Now insert these new instructions into the basic block
  insertInstsAfter(MI, NewMIs);

  // At last, replace the old POP with a new one that doesn't write to PC/LR
  switch (MI.getOpcode()) {
  case ARM::t2LDMIA_RET:
    MI.setDesc(TII->get(ARM::t2LDMIA_UPD));
    break;

  case ARM::tPOP_RET:
    MI.setDesc(TII->get(ARM::tPOP));
    break;

  default:
    break;
  }
  MI.RemoveOperand(MI.getOperandNo(&PCLR));
}

//
// Method: runOnMachineFunction()
//
// Description:
//   This method is called when the PassManager wants this pass to transform
//   the specified MachineFunction.  This method instruments the
//   prologue/epilogue of a MachineFunction so that the return address is saved
//   into/loaded from the shadow stack.
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
ARMSilhouetteShadowStack::runOnMachineFunction(MachineFunction & MF) {
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

  // Warn if the function has variable-sized objects; we assume the program is
  // transformed by store-to-heap promotion, either via a compiler pass or
  // manually
  if (MF.getFrameInfo().hasVarSizedObjects()) {
    errs() << "[SS] Variable-sized objects not promoted in "
           << MF.getName() << "\n";
  }

  unsigned long OldCodeSize = getFunctionCodeSize(MF);

  for (MachineBasicBlock & MBB : MF) {
    for (MachineInstr & MI : MBB) {
      switch (MI.getOpcode()) {
      // Frame setup instructions in function prologue
      case ARM::t2STMDB_UPD:
        // STMDB_UPD writing to SP! is treated same as PUSH
        if (MI.getOperand(0).getReg() != ARM::SP) {
          break;
        }
        LLVM_FALLTHROUGH;
      case ARM::tPUSH:
        // LR can appear as a GPR not in prologue, in which case we don't care
        if (MI.getFlag(MachineInstr::FrameSetup)) {
          for (MachineOperand & MO : MI.operands()) {
            if (MO.isReg() && MO.getReg() == ARM::LR) {
              setupShadowStack(MI);
              break;
            }
          }
        }
        break;

      // Frame destroy instructions in function epilogue
      case ARM::t2LDMIA_UPD:
      case ARM::t2LDMIA_RET:
        // LDMIA_UPD writing to SP! is treated same as POP
        if (MI.getOperand(0).getReg() != ARM::SP) {
          break;
        }
        LLVM_FALLTHROUGH;
      case ARM::tPOP:
      case ARM::tPOP_RET:
        // Handle 2 cases:
        // (1) POP writing to LR followed by TAILJMP.
        // (2) POP writing to PC
        for (MachineOperand & MO : MI.operands()) {
          if (MO.isReg()) {
            if ((MO.getReg() == ARM::LR && findTailJmp(MI) != nullptr) ||
                MO.getReg() == ARM::PC) {
              popFromShadowStack(MI, MO);
              // Bail out as POP cannot write to both LR and PC
              break;
            }
          }
        }
        break;

      default:
        break;
      }
    }
  }

  unsigned long NewCodeSize = getFunctionCodeSize(MF);

  // Output code size information
  std::error_code EC;
  raw_fd_ostream MemStat("./code_size_ss.stat", EC,
                         sys::fs::OpenFlags::F_Append);
  MemStat << MF.getName() << ":" << OldCodeSize << ":" << NewCodeSize << "\n";

  return true;
}

//
// Create a new pass.
//
namespace llvm {
  FunctionPass * createARMSilhouetteShadowStack(void) {
    return new ARMSilhouetteShadowStack();
  }
}
