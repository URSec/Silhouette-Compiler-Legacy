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

ARMSilhouetteShadowStack::ARMSilhouetteShadowStack()
    : MachineFunctionPass(ID) {
  return;
}

StringRef
ARMSilhouetteShadowStack::getPassName() const {
  return "ARM Silhouette Shadow Stack Pass";
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
  int offsetToGo = offset >= 0 ? offset : -offset;
  unsigned addOpc = offset >= 0 ? ARM::t2ADDri12 : ARM::t2SUBri12;
  unsigned subOpc = offset >= 0 ? ARM::t2SUBri12 : ARM::t2ADDri12;
  unsigned strOpc = SilhouetteInvert ? ARM::t2STRT : ARM::t2STRi12;

  unsigned PredReg;
  ARMCC::CondCodes Pred = getInstrPredicate(MI, PredReg);

  std::deque<MachineInstr *> NewMIs;

  // Adjust SP properly
  while (offsetToGo > 4092) {
    NewMIs.push_back(BuildMI(MF, DL, TII->get(addOpc), ARM::SP)
                     .addReg(ARM::SP)
                     .addImm(4092)
                     .add(predOps(Pred, PredReg))
                     .setMIFlag(MachineInstr::ShadowStack));
    offsetToGo -= 4092;
  }
  if (offset < 0 || (SilhouetteInvert && offsetToGo > 255)) {
    NewMIs.push_back(BuildMI(MF, DL, TII->get(addOpc), ARM::SP)
                     .addReg(ARM::SP)
                     .addImm(offsetToGo)
                     .add(predOps(Pred, PredReg))
                     .setMIFlag(MachineInstr::ShadowStack));
    offsetToGo = 0;
  }

  // Generate an STR to the shadow stack
  auto MIB = BuildMI(MF, DL, TII->get(strOpc), ARM::LR)
             .addReg(ARM::SP)
             .addImm(offsetToGo);
  if (strOpc == ARM::t2STRi12) {
    MIB.add(predOps(Pred, PredReg));
  }
  NewMIs.push_back(MIB.setMIFlag(MachineInstr::ShadowStack));

  // Restore SP
  offsetToGo = offset >= 0 ? offset : -offset;
  while (offsetToGo > 4092) {
    NewMIs.push_back(BuildMI(MF, DL, TII->get(subOpc), ARM::SP)
                     .addReg(ARM::SP)
                     .addImm(4092)
                     .add(predOps(Pred, PredReg))
                     .setMIFlag(MachineInstr::ShadowStack));
    offsetToGo -= 4092;
  }
  if (offset < 0 || (SilhouetteInvert && offsetToGo > 255)) {
    NewMIs.push_back(BuildMI(MF, DL, TII->get(subOpc), ARM::SP)
                     .addReg(ARM::SP)
                     .addImm(offsetToGo)
                     .add(predOps(Pred, PredReg))
                     .setMIFlag(MachineInstr::ShadowStack));
    offsetToGo = 0;
  }

  // Now insert these new instructions into the basic block
  insertInstsBefore(MI, NewMIs);
}

//
// Method: popFromShadowStack()
//
// Description:
//   This method modifies a POP instruction to not write to PC and inserts new
//   instructions that load the return address from the shadow stack into PC.
//
// Input:
//   MI   - A reference to a POP instruction after which to insert instructions.
//   PCMO - A reference to the PC operand of the POP.
//
void
ARMSilhouetteShadowStack::popFromShadowStack(MachineInstr & MI,
                                             MachineOperand & PCMO) {
  MachineFunction & MF = *MI.getMF();
  const TargetInstrInfo * TII = MF.getSubtarget().getInstrInfo();

  // We cannot support an offset that is negative or beyond 4096 because we
  // don't get to adjust it back after loading the PC from the shadow stack.
  int offset = ShadowStackOffset;
  assert((offset > 0 && offset < 4096) && "Shadow stack offset not fit in range!");

  unsigned PredReg;
  ARMCC::CondCodes Pred = getInstrPredicate(MI, PredReg);

  std::deque<MachineInstr *> NewMIs;

  // Adjust SP to skip PC on the stack
  NewMIs.push_back(BuildMI(MF, DL, TII->get(ARM::tADDspi), ARM::SP)
                   .addReg(ARM::SP)
                   .addImm(1)
                   .add(predOps(Pred, PredReg))
                   .setMIFlag(MachineInstr::ShadowStack));

  // Generate an LDR from the shadow stack
  NewMIs.push_back(BuildMI(MF, DL, TII->get(ARM::t2LDRi12), ARM::PC)
                   .addReg(ARM::SP)
                   .addImm(offset)
                   .add(predOps(Pred, PredReg))
                   .setMIFlag(MachineInstr::ShadowStack));

  // Now insert these new instructions into the basic block
  insertInstsAfter(MI, NewMIs);

  // At last, replace the old POP with a new one that doesn't write to PC
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
  MI.RemoveOperand(MI.getOperandNo(&PCMO));
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
#endif

  unsigned long OldCodeSize = getFunctionCodeSize(MF);

  for (MachineBasicBlock & MBB : MF) {
    for (MachineInstr & MI : MBB) {
      switch (MI.getOpcode()) {
      // Frame setup instructions in function prologue
      case ARM::tPUSH:
      case ARM::t2STMDB_UPD:  // STMDB_UPD writing to SP! is treated same as PUSH
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
      case ARM::tPOP:
      case ARM::tPOP_RET:
      case ARM::t2LDMIA_UPD:
      case ARM::t2LDMIA_RET:
        for (MachineOperand & MO : MI.operands()) {
          if (MO.isReg() && MO.getReg() == ARM::PC) {
            popFromShadowStack(MI, MO);
            break;
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
