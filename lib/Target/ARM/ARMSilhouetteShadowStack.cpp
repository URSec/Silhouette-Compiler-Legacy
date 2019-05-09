//===- ARMSilhouetteShadowStack - Modify Prolog and Epilog for Shadow Stack --===//
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
#include "ARMSilhouetteShadowStack.h"
#include "ARMSilhouetteConvertFuncList.h"

#include <vector>

using namespace llvm;

#define SHADOW_STACK_OFFSET 2048

// Revert the least significant bit (LSB) of the firstcond of an IT instruction.
// From ARMSilhouetteSTR2STRT.cpp
#define invertLSB(num) (num ^ 0x00000001)

char ARMSilhouetteShadowStack::ID = 0;

static DebugLoc DL;

 

ARMSilhouetteShadowStack::ARMSilhouetteShadowStack() : MachineFunctionPass(ID) {
  return;
}

StringRef ARMSilhouetteShadowStack::getPassName() const {
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
//   This method builds a store instruction before given MachineInstr.
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
  // NOTE: Due to difficulties with IT instructions, 
  // negative offset or offset larger than 4095 is not supported anymore.  
  // t2STRi12 only support immediate within range 0 <= imm <= 4095
  // so imm is not within this range, add it to SP first, and subtract it after store. 
  if ((imm < 4096) && (imm >= 0)){
    // Insert new STR instruction
    return AddDefaultPred(BuildMI(MBB, MI, DL, TII->get(ARM::t2STRi12)).addReg(spillReg).addReg(ARM::SP).addImm(imm)).setMIFlag(MachineInstr::ShadowStack);
  } else {
    unsigned addOp = (imm >= 0) ? ARM::t2ADDri12 : ARM::t2SUBri12;
    unsigned subOp = (imm >= 0) ? ARM::t2SUBri12 : ARM::t2ADDri12;
    int64_t imm_left = (imm >= 0) ? imm : -imm;
    MachineInstr *subInstr = MI;
    while (imm_left > 4095){
      subInstr = AddDefaultPred(BuildMI(MBB, subInstr, DL, TII->get(subOp), ARM::SP).addReg(ARM::SP).addImm(4095)).setMIFlag(MachineInstr::ShadowStack);
      imm_left -= 4095;
    }
    if (imm < 0){
      subInstr = AddDefaultPred(BuildMI(MBB, subInstr, DL, TII->get(subOp), ARM::SP).addReg(ARM::SP).addImm(imm_left)).setMIFlag(MachineInstr::ShadowStack);
      imm_left = 0;
    }
    MachineInstr *strInstr = buildStrSSInstr(MBB, subInstr, spillReg, imm_left, DL, TII);
    imm_left = (imm >= 0) ? imm : -imm;
    MachineInstr *addInstr = strInstr;
    while (imm_left > 4095){
      addInstr = AddDefaultPred(BuildMI(MBB, addInstr, DL, TII->get(addOp), ARM::SP).addReg(ARM::SP).addImm(4095)).setMIFlag(MachineInstr::ShadowStack);
      imm_left -= 4095;
    }
    if (imm < 0){
      addInstr = AddDefaultPred(BuildMI(MBB, addInstr, DL, TII->get(addOp), ARM::SP).addReg(ARM::SP).addImm(imm_left)).setMIFlag(MachineInstr::ShadowStack);
    }
    return strInstr;
  }
  
}


//
// Method: buildLdrSSInstr
//
// Description:
//   This method builds a load instruction before given MachineInstr.
//
// Inputs:
//   MBB - The MachineBasicBlock in which to insert the new unprivileged store.
//   MI - The MachineInstr before which to insert the the new unprivileged store.
//   spillReg - The register that needs to be spilled to shadow stack.
//   imm - The immediate that is added to the baseReg to compute the memory address.
//   DL - A reference to the DebugLoc structure.
//   TII - A pointer to the TargetInstrInfo structure.
//   extra_operands - Predicate information of original POP instruction
//
// Return:
//   None
//
static MachineInstr *buildLdrSSInstr(MachineBasicBlock &MBB,
                      MachineInstr *MI,
                      unsigned spillReg, int64_t imm,
                      DebugLoc &DL,
                      const TargetInstrInfo *TII,
                      std::vector<MachineOperand *> extra_operands) {
  // NOTE: Due to difficulties with IT instructions, 
  // negative offset or offset larger than 4095 is not supported anymore. 
  // t2LDRi12 only support immediate within range 0 <= imm <= 4095
  // so imm is not within this range, add it to SP first, and subtract it after load. 
  if ((imm < 4096) && (imm >= 0)){
    // Insert new LDR instruction
    MachineInstrBuilder MIB;
    if (MI->getOpcode() == ARM::tPOP_RET || MI->getOpcode() == ARM::tPOP || MI->getOpcode() == ARM::t2LDMIA_RET){
      MIB = BuildMI(MBB, MI, DL, TII->get(ARM::t2LDRi12)).addReg(spillReg).addReg(ARM::SP).addImm(imm);
      // Add predicates of original POP to new LDR instruction
      for (MachineOperand* MO : extra_operands){
        MIB.addOperand(*MO);
      }
    } else{
      MIB = BuildMI(MBB, MI, DL, TII->get(ARM::t2LDRi12)).addReg(spillReg).addReg(ARM::SP).addImm(imm);
      // Add predicates of original POP to new LDR instruction, but do not kill predicate register
      // because later instructions also depends on them. 
      for (MachineOperand* MO : extra_operands){
        if (MO->isReg()){
          MIB.addReg(MO->getReg(), getRegState(*MO) & !RegState::Kill);
        } else{
          MIB.addOperand(*MO);
        }
      }
    }
    MIB.setMIFlag(MachineInstr::ShadowStack);
    return MIB.getInstr();
  } else {
    // Add/subtract SP to locate to shadow stack
    unsigned addOp = (imm >= 0) ? ARM::t2ADDri12 : ARM::t2SUBri12;
    unsigned subOp = (imm >= 0) ? ARM::t2SUBri12 : ARM::t2ADDri12;
    int64_t imm_left = (imm >= 0) ? imm : -imm;
    MachineInstr* subInstr;
    while (imm_left > 4095){
      MachineInstrBuilder MIB;
      if (subInstr == NULL){
        // if (MI == NULL){
        //   MIB = BuildMI(MBB, MBB.end(), DL, TII->get(subOp), ARM::SP).addReg(ARM::SP).addImm(4095);
        // } else{
        //   MIB = BuildMI(MBB, MI, DL, TII->get(subOp), ARM::SP).addReg(ARM::SP).addImm(4095);
        // }
        MIB = BuildMI(MBB, MI, DL, TII->get(subOp), ARM::SP).addReg(ARM::SP).addImm(4095);
      } else{
        MIB = BuildMI(MBB, subInstr, DL, TII->get(subOp), ARM::SP).addReg(ARM::SP).addImm(4095);
      }
      for (MachineOperand* MO : extra_operands){
        if (MO->isReg()){
          MIB.addReg(MO->getReg(), getRegState(*MO) & !RegState::Kill);
        } else{
          MIB.addOperand(*MO);
        }
      }
      MIB.setMIFlag(MachineInstr::ShadowStack);
      subInstr = MIB.getInstr();
      imm_left -= 4095;
    }
    // Add/subtract remaining amount to SP
    if (imm < 0){
      MachineInstrBuilder MIB;
      if (subInstr == NULL){
        MIB = BuildMI(MBB, MBB.end(), DL, TII->get(subOp), ARM::SP).addReg(ARM::SP).addImm(imm_left);
      } else{
        MIB = BuildMI(MBB, subInstr, DL, TII->get(subOp), ARM::SP).addReg(ARM::SP).addImm(imm_left);
      }
      for (MachineOperand* MO : extra_operands){
        if (MO->isReg()){
          MIB.addReg(MO->getReg(), getRegState(*MO) & !RegState::Kill);
        } else{
          MIB.addOperand(*MO);
        }
      }
      MIB.setMIFlag(MachineInstr::ShadowStack);
      subInstr = MIB.getInstr();
      imm_left = 0;
    }
    // Add actual load instruction
    MachineInstr *strInstr = buildLdrSSInstr(MBB, subInstr, spillReg, imm_left, DL, TII, extra_operands);
    // Revert changes to SP
    imm_left = (imm >= 0) ? imm : -imm;
    MachineInstr *addInstr = strInstr;
    while (imm_left > 4095){
      MachineInstrBuilder MIB = AddDefaultPred(BuildMI(MBB, addInstr, DL, TII->get(addOp), ARM::SP).addReg(ARM::SP).addImm(4095)).setMIFlag(MachineInstr::ShadowStack);
      imm_left -= 4095;
      if (imm_left < 4095 && imm_left >= 0){
        for (MachineOperand* MO : extra_operands){
          MIB.addOperand(*MO);
        }
      } else {
        for (MachineOperand* MO : extra_operands){
          if (MO->isReg()){
            MIB.addReg(MO->getReg(), getRegState(*MO) & !RegState::Kill);
          } else{
            MIB.addOperand(*MO);
          }
        }
      }
      addInstr = MIB.getInstr();
    }
    if (imm < 0){
      MachineInstrBuilder MIB = AddDefaultPred(BuildMI(MBB, addInstr, DL, TII->get(addOp), ARM::SP).addReg(ARM::SP).addImm(imm_left)).setMIFlag(MachineInstr::ShadowStack);
      for (MachineOperand* MO : extra_operands){
        MIB.addOperand(*MO);
      }
      addInstr = MIB.getInstr();
    }
    return strInstr;
  }
  
}

//
// Method: rebuildPopInstr
//
// Description:
//   This method builds a new POP_RET instruction that does not contain
//   PC, before certain instruction. 
//
// Inputs:
//   MBB - The MachineBasicBlock in which to insert the new unprivileged store.
//   MI - The MachineInstr before which to insert the the new unprivileged store.
//   spillReg - The register that needs to be spilled to shadow stack.
//   imm - The immediate that is added to the baseReg to compute the memory address.
//   DL - A reference to the DebugLoc structure.
//   TII - A pointer to the TargetInstrInfo structure.
//   extra_operands - Predicate information of original POP instruction
//   MI_order - The MachineInstr that should be right after the new POP_RET instruction
//
// Return:
//   None
//
static MachineInstr *rebuildPopInstr(MachineBasicBlock &MBB,
                      MachineInstr *MI,
                      DebugLoc &DL,
                      const TargetInstrInfo *TII,
                      std::vector<MachineOperand*> extra_operands,
                      MachineInstr *MI_order) {
  // Generate new instruction
  MachineInstrBuilder MIB = BuildMI(MBB, MI_order, DL, TII->get(MI->getOpcode()));
  unsigned i;
  // Add predicate of original instruction to new instruction, but 
  // remove the Kill flag to register as they are still needed for 
  // LDR instruction afterward. 
  for (MachineOperand* MO : extra_operands){
    if (MO->isReg()){
      MIB.addReg(MO->getReg(), getRegState(*MO) & !RegState::Kill);
    } else{
      MIB.addOperand(*MO);
    }
  }
  // Add operands of original instruction to new instruction except for
  // PC register. 
  for (i = 2; i < MI->getNumOperands(); i++){
    MachineOperand MO = MI->getOperand(i);
    if (MO.isReg() && MO.getReg() == ARM::PC){

    } else{
      MIB.addOperand(MO);
    }
  }
  
  MIB.getInstr()->setFlags(MI->getFlags());
  return MIB.getInstr();
}



//
// Method: runOnMachineFunction()
//
// Description:
//   This method is called when the PassManager wants this pass to transform
//   the specified MachineFunction.
//   This method finds instructions that need to be processed (PUSH, POP, IT)
//   and call functions accordingly to process them. 
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
bool ARMSilhouetteShadowStack::runOnMachineFunction(MachineFunction &MF) {
  StringRef funcName = MF.getName();
  // skip certain functions
  if (funcBlacklist.find(funcName) != funcBlacklist.end()) return false;

#if 0
  // instrument certain functions
  if (funcWhitelist.find(funcName) == funcWhitelist.end()) return false;
#endif

  // for debugging
  // errs() << "Silhouette SS: hello from function: " << funcName << "\n";

  // Compute the code size of the original machine function.
  unsigned codeSize = getFuncCodeSize(MF); 
  unsigned codeSizeNew = 0;

  const TargetInstrInfo *TII = MF.getSubtarget<ARMSubtarget>().getInstrInfo();
  DebugLoc DL;
  // MachineInstr *pop_orig = NULL;

  // iterate over all MachineInstr
  for (MachineBasicBlock &MBB : MF) {
    std::vector<MachineInstr *> originalStores; // Need delete the original instructions.
    std::vector<unsigned> ITconds; // Condition for splitted IT instructions
    for (MachineInstr &MI : MBB) {
      unsigned opcode = MI.getOpcode();
      std::vector<MachineOperand*> additional_operands; // Predicates for original instructions
      int64_t imm = SHADOW_STACK_OFFSET; 
      switch (opcode) {
        // A 7.7.157: STMDB writing to SP! is treated the same as PUSH
        case ARM::t2STMDB:
        case ARM::tPUSH: {
          // If this instruction is not prolog/epilog, then we don't care
          // If this instruction is inside an IT block, add appropiate IT instruction
          if (!MI.getFlag(MachineInstr::FrameSetup)){
            if (!ITconds.empty()){
              BuildMI(MBB, MI, DL, TII->get(ARM::t2IT)).addImm(ITconds.front()).addImm(8);
              ITconds.erase(ITconds.begin());
            }
            break;
          }
          bool hasLR = false; // PUSH instruction may not contain LR
          for (MachineOperand &MO : MI.operands()){
            if (MO.isReg()){
              if (MO.getReg() == ARM::LR){
                // Since it is storing LR to shadow stack BEFORE push instruction, 
                // the immediate should be imm - 4 to find the corresponding address of 
                // the normal stack
                hasLR = true;
                // If this instruction is inside an IT block, add IT instruction
                // for 2 instructions, STR and POP. 
                if (!ITconds.empty()){
                  BuildMI(MBB, MI, DL, TII->get(ARM::t2IT)).addImm(ITconds.front()).addImm(4);
                  ITconds.erase(ITconds.begin());
                }
                // Build STR instr
                buildStrSSInstr(MBB, &MI, MO.getReg(), imm - 4, DL, TII);
              }
            }
          }
          // If it does not contain LR, but it is inside IT block, 
          // we still need to add IT instruction
          if (!hasLR){
            if (!ITconds.empty()){
              BuildMI(MBB, MI, DL, TII->get(ARM::t2IT)).addImm(ITconds.front()).addImm(8);
              ITconds.erase(ITconds.begin());
            }
          }
          // MI.print(errs());
          // errs() << "\n";
          break;
        }
        // A 7.7.40: LDMIA writing to SP! is treated the same as POP
        case ARM::t2LDMIA_RET:
        case ARM::tPOP_RET:
        case ARM::tPOP:{
          bool hasPC = false; // POP instruction may not contain PC reg
          MCInstrDesc MIdesc = MI.getDesc();
          int pred;
          // Save predicates of original POP instruction
          for (pred = MIdesc.findFirstPredOperandIdx(); pred >= 0 && pred < MI.getNumOperands(); pred++){
            if (MIdesc.OpInfo[pred].isPredicate()){
              // errs() << "predicate index: " << pred << "\r\n";
              additional_operands.push_back(&MI.getOperand(pred));
            }
          }
          for (MachineOperand &MO : MI.operands()){
            if (MO.isReg()){
              if (MO.getReg() == ARM::PC){
                hasPC = true;
                // If this instruction is inside IT block, add IT instruction 
                // for the next 3 instructions: POP, ADD, LDR. 
                if (!ITconds.empty()){
                  unsigned firstcondLSB = (MI.getOperand(0).getImm()) & 0x00000001;
                  unsigned mask = 2;
                  mask = mask | (firstcondLSB << 3) | (firstcondLSB << 2);
                  BuildMI(MBB, MI, DL, TII->get(ARM::t2IT)).addImm(ITconds.front()).addImm(mask).setMIFlag(MachineInstr::ShadowStack);
                  ITconds.erase(ITconds.begin());
                }
                // errs() << "Found PC: ";
                // MI.print(errs());
                // errs() << "\n";
                // Build LDR instruction
                MachineInstr* ldrMI = buildLdrSSInstr(MBB, &MI, MO.getReg(), imm - 4, DL, TII, additional_operands);
                // Build ADD instruction to add SP since we don't pop PC anymore. Add it before LDR instruction
                MachineInstrBuilder addSP = BuildMI(MBB, ldrMI, DL, TII->get(ARM::tADDspi), ARM::SP).addReg(ARM::SP).addImm(1).setMIFlag(MachineInstr::ShadowStack);
                // Build POP instruction that does not contain PC. Add it before ADD instruction. 
                MachineInstr* newMI = rebuildPopInstr(MBB, &MI, DL, TII, additional_operands, addSP.getInstr());
                // Add original predicates to ADD instruction but remove Kill flag for registers
                for (MachineOperand* MO : additional_operands){
                  if (MO->isReg()){
                    addSP.addReg(MO->getReg(), getRegState(*MO) & !RegState::Kill);
                  } else{
                    addSP.addOperand(*MO);
                  }
                }
                
                // errs() << "New MI: ";
                // ldrMI->print(errs());
                // errs() << "\r\n";
                originalStores.push_back(&MI);
                break;
              } 
            } else {
            }
          }
          // If it does not contain PC, but it is inside IT block, 
          // we still need to add IT instruction
          if (!hasPC){
            if (!ITconds.empty()){
              BuildMI(MBB, MI, DL, TII->get(ARM::t2IT)).addImm(ITconds.front()).addImm(8);
              ITconds.erase(ITconds.begin());
            }
          }
          break;
        } 
        case ARM::t2IT:{
          // IT (If-then) Instruction needs special handling.
          // Same idea as splitITBlockWithSTR() function in ARMSilhouetteSTR2STRT.cpp
          assert(ITconds.empty() && "Nested IT Instruction");
          // errs() << "IT instruction!\r\n";
          unsigned numCondInstr;
          unsigned firstcond = MI.getOperand(0).getImm();
          // Refer to ARM manual for mask format
          unsigned mask = MI.getOperand(1).getImm() & 0x0000000f;
          if (mask & 0x00000001) {
            numCondInstr = 4;
          } else if (mask & 0x00000002) {
            numCondInstr = 3;
          } else if (mask & 0x00000004) {
            numCondInstr = 2;
          } else {
            numCondInstr = 1;
          }
          MachineInstr *condMI = &MI;
          // We don't need to modify this IT instruction at all if there's no 
          // POP instruction that needs to be processed
          bool hasPOP = false; 
          int i;
          for (i = numCondInstr; i > 0; i--){
            assert(condMI != NULL && "getNextNode() returns a NULL!\n");
            condMI = condMI->getNextNode();
            if (condMI->getOpcode() == ARM::tPOP_RET || 
                condMI->getOpcode() == ARM::tPOP || 
                condMI->getOpcode() == ARM::t2LDMIA_RET){
              for (MachineOperand MO : condMI->operands()){
                if (MO.isReg() && MO.getReg() == ARM::PC){
                  assert(SHADOW_STACK_OFFSET <= 4096 && SHADOW_STACK_OFFSET >= 0 && "Shadow Stack offset cannot be larger than 4096");
                  // BuildMI(MBB, MI, DL, TII->get(ARM::t2IT))
                  //   .addImm(firstcond).addImm(2);
                  // originalStores.push_back(&MI);
                  hasPOP = true;
                  break;
                }
              }
              if (hasPOP)
                break;
            }
            if (condMI->isDebugValue())
              i++;
          }
          // Split IT instruction to multiple IT instructions.
          // Save new condition to ITconds, apply them for next few instructions
          // and remove original IT instruction. 
          if (hasPOP) {
            ITconds.push_back(firstcond);
            unsigned firstcondLSB = firstcond & 0x00000001;
            unsigned firstcondLSBInverted = invertLSB(firstcond);

            numCondInstr--;
            for (unsigned i = 3; numCondInstr > 0; numCondInstr--, i--) {
              unsigned CC = (firstcondLSB == ((mask & (1u << i)) >> i)) ? 
                firstcond : firstcondLSBInverted;
              ITconds.push_back(CC);
            }
            originalStores.push_back(&MI);
          }
          break;
        }
        default:
          // Although we don't care about other instructions, 
          // we still need to add IT instructions accordingly
          if (!MI.isDebugValue()){
            if (!ITconds.empty()){
              BuildMI(MBB, MI, DL, TII->get(ARM::t2IT)).addImm(ITconds.front()).addImm(8);
              ITconds.erase(ITconds.begin());
            }
          }
          break;
      }
    } 
    // Remove all instructions replaced
    for(MachineInstr* MI : originalStores){
      MI->eraseFromParent();
    }
  }
  // errs() << "Silhouette SS: end of function " << funcName << "\n";

  // Compute the code size of the transformed machine function.
  codeSizeNew = getFuncCodeSize(MF);

  // Write the result to a file.
  // The code size string is very small (funcName + original_code_size +
  // new_code_size); raw_fd_ostream's "<<" operator ensures that the write is 
  // atomic because essentially it uses write() to write to a file and according
  // to http://man7.org/linux/man-pages/man7/pipe.7.html, any write with less
  // than PIPE_BUF bytes (at least 4096) is guaranteed to be atomic.
  std::error_code EC;
  StringRef memStatFile("./code_size_ss.stat");
  raw_fd_ostream memStat(memStatFile, EC, sys::fs::OpenFlags::F_Append);
  std::string funcCodeSize = funcName.str() + ":" + std::to_string(codeSize) \
    + ":" + std::to_string(codeSizeNew) + "\n";
  memStat << funcCodeSize;

  // This pass modifies the program.
  return true;
}

//
// Create a new pass.
namespace llvm {
  FunctionPass *createARMSilhouetteShadowStack(void) {
    return new ARMSilhouetteShadowStack();
  }
}
