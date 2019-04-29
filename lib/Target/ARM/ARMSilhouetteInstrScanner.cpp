//===- ARMSilhouetteInstrScanner - Modify Prolog and Epilog for Shadow Stack --===//
//
//         Protecting Control Flow of Real-time OS applications
//
// This file was written by at the University of Rochester.
// All Rights Reserved.
//
//===----------------------------------------------------------------------===//
//
// This pass adds STR imm to store LR to shadow stack and changes 
// popping LR to loading LR from shadow stack 
// instructions.
//
//===----------------------------------------------------------------------===//
//

#include "ARM.h"
#include "ARMTargetMachine.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/FileSystem.h"
#include "ARMSilhouetteInstrScanner.h"
#include "ARMSilhouetteConvertFuncList.h"

#include <vector>

using namespace llvm;

#define SHADOW_STACK_OFFSET 2048

char ARMSilhouetteInstrScanner::ID = 0;

static DebugLoc DL;

 

ARMSilhouetteInstrScanner::ARMSilhouetteInstrScanner() : MachineFunctionPass(ID) {
  return;
}

StringRef ARMSilhouetteInstrScanner::getPassName() const {
  return "ARM Silhouette Shadow Stack Pass";
}




//
// Method: buildUnprivStr
//
// Description:
//   This method builds an unprivileged store instruction.
//   Unprivileged load/store instructions only support 8-bit immediate, ranging
//   from 0 to 255; but a normal store can have an immediate as large as 1023.
//   For a negative immediate or one that is greater than 255, it's already
//   handled in the convertSTRimm() function.
//
// Inputs:
//   MBB - The MachineBasicBlock in which to insert the new unprivileged store.
//   MI - The MachineInstr before which to insert the the new unprivileged store.
//   sourceReg - The register whose contents will be stored to some memory address.
//   baseReg - The register used as the base register to compute the memory address.
//   imm - The immediate that is added to the baseReg to compute the memory address.
//   newOpcode - The opcode of the unprivileged store.
//   DL - A reference to the DebugLoc structure.
//   TII - A pointer to the TargetInstrInfo structure.
//
// Outputs:
//   A new unprivileged store.
//
// Return:
//   A newly created store instruction.
//
static MachineInstr *buildUnprivStr(MachineBasicBlock &MBB,
                      MachineInstr *MI,
                      unsigned sourceReg, unsigned baseReg, uint64_t imm,
                      unsigned newOpcode,
                      DebugLoc &DL,
                      const TargetInstrInfo *TII) {
  return BuildMI(MBB, MI, DL, TII->get(newOpcode))
          .addReg(sourceReg)
          .addReg(baseReg)
          .addImm(imm).operator->();
}





//
// Function getFuncCodeSize()
//
// Description:
//   This function computes the code size of a MachineFunction.
//
// Inputs:
//   MF - A reference to the target Machine Function.
// 
// Return:
//   The size (in bytes) of the Machine Function.
//
static unsigned long getFuncCodeSize(MachineFunction &MF) {
  const ARMBaseInstrInfo *ABII = MF.getSubtarget<ARMSubtarget>().getInstrInfo();
  unsigned long codeSize = 0;
  for (MachineBasicBlock &MB : MF) {
    for (MachineInstr &MI : MB) {
        codeSize += ABII->getInstSizeInBytes(MI);
    }
  }

  return codeSize;
}

//
// Method: buildStrSSInstr
//
// Description:
//   This method builds a store instruction after given MachineInstr.
//
// Inputs:
//   MBB - The MachineBasicBlock in which to insert the new unprivileged store.
//   MI - The MachineInstr before which to insert the the new unprivileged store.
//   spillReg - The register that needs to be spilled to shadow stack.
//   imm - The immediate that is added to the baseReg to compute the memory address.
//   DL - A reference to the DebugLoc structure.
//   TII - A pointer to the TargetInstrInfo structure.
//
// Return:
//   None
//
static MachineInstr *buildStrSSInstr(MachineBasicBlock &MBB,
                      MachineInstr *MI,
                      unsigned spillReg, int64_t imm,
                      DebugLoc &DL,
                      const TargetInstrInfo *TII) {
  // t2STRi12 only support immediate within range 0 <= imm <= 4095
  // so imm is not within this range, add it to SP first, and subtract it after store. 
  // errs() << "Imm: " << imm << "\n";
  // if ((imm <= 1020) && (imm > 0) && (imm % 4 == 0)){
  //   return BuildMI(MBB, MI, DL, TII->get(ARM::tSTRspi)).addReg(spillReg).addImm(imm);
  // } else 
  if ((imm < 4096) && (imm >= 0)){
    // Insert new STR instruction
    return AddDefaultPred(BuildMI(MBB, MI, DL, TII->get(ARM::t2STRi12)).addReg(spillReg).addReg(ARM::SP).addImm(imm));
  } else {
    unsigned addOp = (imm >= 0) ? ARM::t2ADDri12 : ARM::t2SUBri12;
    unsigned subOp = (imm >= 0) ? ARM::t2SUBri12 : ARM::t2ADDri12;
    int64_t imm_left = (imm >= 0) ? imm : -imm;
    MachineInstr *subInstr = MI;
    while (imm_left > 4095){
      subInstr = AddDefaultPred(BuildMI(MBB, subInstr, DL, TII->get(subOp), ARM::SP).addReg(ARM::SP).addImm(4095));
      imm_left -= 4095;
    }
    if (imm < 0){
      subInstr = AddDefaultPred(BuildMI(MBB, subInstr, DL, TII->get(subOp), ARM::SP).addReg(ARM::SP).addImm(imm_left));
      imm_left = 0;
    }
    MachineInstr *strInstr = buildStrSSInstr(MBB, subInstr, spillReg, imm_left, DL, TII);
    imm_left = (imm >= 0) ? imm : -imm;
    MachineInstr *addInstr = strInstr;
    while (imm_left > 4095){
      addInstr = AddDefaultPred(BuildMI(MBB, addInstr, DL, TII->get(addOp), ARM::SP).addReg(ARM::SP).addImm(4095));
      imm_left -= 4095;
    }
    if (imm < 0){
      addInstr = AddDefaultPred(BuildMI(MBB, addInstr, DL, TII->get(addOp), ARM::SP).addReg(ARM::SP).addImm(imm_left));
    }
    return strInstr;
  }
  
}


//
// Method: buildLdrSSInstr
//
// Description:
//   This method builds a store instruction after given MachineInstr.
//
// Inputs:
//   MBB - The MachineBasicBlock in which to insert the new unprivileged store.
//   MI - The MachineInstr before which to insert the the new unprivileged store.
//   spillReg - The register that needs to be spilled to shadow stack.
//   imm - The immediate that is added to the baseReg to compute the memory address.
//   DL - A reference to the DebugLoc structure.
//   TII - A pointer to the TargetInstrInfo structure.
//
// Return:
//   None
//
static MachineInstr *buildLdrSSInstr(MachineBasicBlock &MBB,
                      MachineInstr *MI,
                      unsigned spillReg, int64_t imm,
                      DebugLoc &DL,
                      const TargetInstrInfo *TII) {
  // t2STRi12 only support immediate within range 0 <= imm <= 4095
  // so imm is not within this range, add it to SP first, and subtract it after store. 
  // errs() << "Imm: " << imm << "\n";
  // if ((imm <= 1020) && (imm > 0) && (imm % 4 == 0)){
  //   return BuildMI(MBB, MI, DL, TII->get(ARM::tSTRspi)).addReg(spillReg).addImm(imm);
  // } else 
  if ((imm < 4096) && (imm >= 0)){
    // Insert new STR instruction
    if (MI == NULL){
      return AddDefaultPred(BuildMI(MBB, MBB.end(), DL, TII->get(ARM::t2LDRi12)).addReg(spillReg).addReg(ARM::SP).addImm(imm));
    } else{
      return AddDefaultPred(BuildMI(MBB, MI, DL, TII->get(ARM::t2LDRi12)).addReg(spillReg).addReg(ARM::SP).addImm(imm));
    }
  } else {
    unsigned addOp = (imm >= 0) ? ARM::t2ADDri12 : ARM::t2SUBri12;
    unsigned subOp = (imm >= 0) ? ARM::t2SUBri12 : ARM::t2ADDri12;
    int64_t imm_left = (imm >= 0) ? imm : -imm;
    MachineInstr *subInstr = NULL;
    while (imm_left > 4095){
      if (subInstr == NULL){
        if (MI == NULL){
          subInstr = AddDefaultPred(BuildMI(MBB, MBB.end(), DL, TII->get(subOp), ARM::SP).addReg(ARM::SP).addImm(4095));
        } else{
          subInstr = AddDefaultPred(BuildMI(MBB, MI, DL, TII->get(subOp), ARM::SP).addReg(ARM::SP).addImm(4095));
        }
        
      } else{
        subInstr = AddDefaultPred(BuildMI(MBB, subInstr, DL, TII->get(subOp), ARM::SP).addReg(ARM::SP).addImm(4095));
      }
      imm_left -= 4095;
    }
    if (imm < 0){
      if (subInstr == NULL){
        subInstr = AddDefaultPred(BuildMI(MBB, MBB.end(), DL, TII->get(subOp), ARM::SP).addReg(ARM::SP).addImm(imm_left));
      } else{
        subInstr = AddDefaultPred(BuildMI(MBB, subInstr, DL, TII->get(subOp), ARM::SP).addReg(ARM::SP).addImm(imm_left));
      }
      imm_left = 0;
    }
    MachineInstr *strInstr = buildLdrSSInstr(MBB, subInstr, spillReg, imm_left, DL, TII);
    imm_left = (imm >= 0) ? imm : -imm;
    MachineInstr *addInstr = strInstr;
    while (imm_left > 4095){
      addInstr = AddDefaultPred(BuildMI(MBB, addInstr, DL, TII->get(addOp), ARM::SP).addReg(ARM::SP).addImm(4095));
      imm_left -= 4095;
    }
    if (imm < 0){
      addInstr = AddDefaultPred(BuildMI(MBB, addInstr, DL, TII->get(addOp), ARM::SP).addReg(ARM::SP).addImm(imm_left));
    }
    return strInstr;
  }
  
}



//
// Method: runOnMachineFunction()
//
// Description:
//   This method is called when the PassManager wants this pass to transform
//   the specified MachineFunction.
//   This method deletes all the normal store instructions and insert store
//   unprivileged instructions.
//
// Inputs:
//   MF - A reference to the MachineFunction to transform.
//
// Outputs:
//   MF - The transformed MachineFunction.
//
// Return value:
//   true - The MachineFunction was transformed.
//   false - The MachineFunction was not transformed.
//
bool ARMSilhouetteInstrScanner::runOnMachineFunction(MachineFunction &MF) {
  StringRef funcName = MF.getName();
  // skip certain functions
  if (funcBlacklist.find(funcName) != funcBlacklist.end()) return false;

#if 0
  // instrument certain functions
  if (funcWhitelist.find(funcName) == funcWhitelist.end()) return false;
#endif

  // for debugging
  errs() << "Silhouette SS: hello from function: " << funcName << "\n";

  // Compute the code size of the original machine function.
  unsigned codeSize = getFuncCodeSize(MF); 
  unsigned codeSizeNew = 0;

  const TargetInstrInfo *TII = MF.getSubtarget<ARMSubtarget>().getInstrInfo();
  DebugLoc DL;

  // iterate over all MachineInstr
  for (MachineBasicBlock &MBB : MF) {
  
    for (MachineInstr &MI : MBB) {
      unsigned opcode = MI.getOpcode();
      unsigned sourceReg = 0;
      unsigned sourceReg2 = 0; // for STRD
      unsigned baseReg = 0;
      unsigned offsetReg = 0;  // for STR(register) 
      int64_t imm = SHADOW_STACK_OFFSET; 
      bool isPrivileged = false;

      switch (opcode) {
        //
        case ARM::t2MSR_M:
          for (MachineOperand &MO : MI.operands()){
            if (MO.isReg()){
              switch (MO.getReg()){
                case ARM::APSR_NZCV:
                 errs() << "LL:GOOD:MSR writes to APSR.\n";
                 break;
                case ARM::SP:
                 errs() << "LL:ERROR:MSR writes to SP reg.\n";
                 isPrivileged = true;
                 break;
                default:
                 errs() << "LL:WARNING: alien MSR operands "<< MO.getReg() << "\n";
              }
            }else if (MO.isImm()){

              // check the SYSm value
              unsigned ValLow = MO.getImm() & 0xff;
              switch (ValLow){
                 case  0: // apsr
                   errs() << "LL:GOOD:MSR writes to APSR.\n";
                   break;
                 case  1: // iapsr
                   errs() << "LL:ERROR:MSR writes to IAPSR.\n";
                   isPrivileged = true;
                   break;
                 case  2: // eapsr
                   errs() << "LL:ERROR:MSR writes to EAPSR.\n";
                   isPrivileged = true;
                   break;
                 case  3: // xpsr
                   errs() << "LL:ERROR:MSR writes to XPSR.\n";
                   isPrivileged = true;
                   break;
                 case  5: // ipsr
                 case  6: // epsr
                 case  7: // iepsr
                   errs() << "LL:ERROR:MSR writes to IPSR/EPSR.\n";
                   isPrivileged = true;
                   break;
                 case  8: // msp
                 case  9: // psp
                   errs() << "LL:ERROR:MSR writes to MSP/ESP.\n";
                   isPrivileged = true;
                   break;
                 case 16: // primask
                 case 20: // control
                   errs() << "LL:ERROR:MSR writes to MSP/ESP.\n";
                   isPrivileged = true;
                   break;
                 case 17: // basepri
                 case 18: // basepri_max
                 case 19: // faultmask
                   errs() << "LL:ERROR:MSR writes to MSP/ESP.\n";
                   isPrivileged = true;
                   break;
                 default:
                   errs() << "LL:WARNING: alien operand of MSR" << std::hex << ValLow << "\n";
              } // switch
            } // else if (MO.isImm())

            assert(isPrivileged && "LL:ERROR: privileged instruction not allowed\n");
            if (isPrivileged){
             errs() << "LL:ERROR:Privileged instruction detected; now exit\n";
             exit(-1);
            }
          break;
        } // for(MachineOperand &MO : MI.operands())
      } // switch(opcode) 
     } // for (MachineInstr &MI : MBB) 
  } //for  (MachineBasicBlock &MBB : MF)
  
  // This pass does not modify the program.
  return false;
}

//
// Create a new pass.
namespace llvm {
  FunctionPass *createARMSilhouetteInstrScanner(void) {
    return new ARMSilhouetteInstrScanner();
  }
}
