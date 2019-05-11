//===-- ARMSilhouetteSTR2STRT - Store to Unprivileged Store convertion-----===//
//
//         Protecting Control Flow of Real-time OS applications
//
// This file was written by at the University of Rochester.
// All Rights Reserved.
//
//===----------------------------------------------------------------------===//
//
// This pass converts all normal store instructions to the store unprivileged 
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
#include "ARMSilhouetteSTR2STRT.h"
#include "ARMSilhouetteConvertFuncList.h"

#include <vector>

using namespace llvm;

// number of general-purpose registers R0 - R12, excluding SP, PC, and LR.
#define GP_REGULAR_REG_NUM 13  

// Revert the least significant bit (LSB) of the firstcond of an IT instruction.
#define invertLSB(num) (num ^ 0x00000001)

char ARMSilhouetteSTR2STRT::ID = 0;

static DebugLoc DL;

// Instructions that update the process stus flags.
std::set<unsigned> flagUpdateInstrs {
  ARM::tSUBi8, ARM::tADDi8,
  ARM::tADDrr, ARM::tSUBrr
};
 

ARMSilhouetteSTR2STRT::ARMSilhouetteSTR2STRT() : MachineFunctionPass(ID) {
  return;
}

StringRef ARMSilhouetteSTR2STRT::getPassName() const {
  return "ARM Silhouette store to store unprivileged convertion Pass";
}


// function declrations
static void printOperands(MachineInstr &MI);
static void splitITBlockWithSTR(MachineFunction &MF);
static void buildITInstr(MachineBasicBlock &MBB, MachineInstr *MI,
                        DebugLoc &DL, const TargetInstrInfo *TII,
                        unsigned condCode = ARMCC::AL);


// 
// Function: getNewOpcode()
//
// Description:
//   This function returns the opcode of an unprivileged load/store instruction
//   corresponding to a normal load/store. According to table A4-17,  some 
//   normal load/store have an unprivileged version while some don't. With some
//   extra work, we map each kind of load/store to one unprivileged load/store 
//   shown in this table.
//
// Inputs:
//   opcode - The opcode of a normal load/store.
//
// Return value:
//   newOpcode - The opcode of the corresponding unprivileged load/store.
//   -1 - Implementation error: the input opcode is unknown.
//
static unsigned getNewOpcode(unsigned opcode) {
  switch (opcode) {
    // store word
    case ARM::tSTRi:       // A7.7.158 Encoding T1
    case ARM::tSTRspi:     // A7.7.158 Encoding T2
    case ARM::t2STRi12:    // A7.7.158 Encoding T3
    case ARM::t2STRi8:     // A7.7.158 Encoding T4
    case ARM::t2STR_PRE:   // A7.7.158 Encoding T4; pre-indexed
    case ARM::t2STR_POST:  // A7.7.158 Encoding T4; post-indexed
    case ARM::tSTRr:       // A7.7.159 Encoding T1
    case ARM::t2STRs:      // A7.7.158 Encoding T2
    // store double word
    case ARM::t2STRDi8:    // A7.7.163 Encoding T1
    case ARM::t2STRD_PRE:  // A7.7.163 Encoding T1; pre-indexed
    case ARM::t2STRD_POST: // A7.7.163 Encoding T1; post-indexed
    // store floating-point register
    case ARM::VSTRD:       // A7.7.256 Encoding T1; store double word
    case ARM::VSTRS:       // A7.7.256 Encoding T2; store single word
    // store multiple registers to memory
    case ARM::tSTMIA_UPD:  // A7.7.156 Encoding T1; store with write back
    case ARM::t2STMIA:     // A7.7.156 Encoding T2; no write back
    case ARM::t2STMIA_UPD: // A7.7.156 Encoding T2; with write back
    case ARM::t2STMDB:     // A7.7.157 Encoding T1; no write back
    // push
    case ARM::tPUSH:            // A7.7.99 Encoding T1;
    case ARM::t2STMDB_UPD: // A7.7.99 Encoding T2; 
    // store multiple FP registers to memory; actually they're VPUSH.
    case ARM::VSTMDDB_UPD: // A7.7.249 Encoding T1; double-precision; update first
    case ARM::VSTMSDB_UPD: // A7.7.249 Encoding T1; single-precision; update first
    case ARM::VSTMDIA_UPD: // A7.7.255 Encoding T1; double-precision; update later
    case ARM::VSTMSIA_UPD: // A7.7.255 Encoding T2; single-precision; update later
      return ARM::t2STRT;

    // store half word
    case ARM::tSTRHi:      // A7.7.167 Encoding T1
    case ARM::t2STRHi12:   // A7.7.167 Encoding T2
    case ARM::t2STRHi8:    // A7.7.167 Encoding T3; not write back
    case ARM::t2STRH_PRE:  // A7.7.160 Encoding T3; pre-indexed
    case ARM::t2STRH_POST: // A7.7.160 Encoding T3; post-indexed
    case ARM::tSTRHr:      // A7.7.168 Encoding T1
    case ARM::t2STRHs:     // A7.7.168 Encoding T2
      return ARM::t2STRHT;

    // store byte
    case ARM::tSTRBi:      // A7.7.160 Encoding T1
    case ARM::t2STRBi12:   // A7.7.160 Encoding T2
    case ARM::t2STRBi8:    // A7.7.160 Encoding T3; not write back
    case ARM::t2STRB_PRE:  // A7.7.160 Encoding T3; pre-indexed
    case ARM::t2STRB_POST: // A7.7.160 Encoding T3; post-indexed
    case ARM::tSTRBr:      // A7.7.161 Encoding T1
    case ARM::t2STRBs:     // A7.7.161 Encoding T2
      return ARM::t2STRBT;
  }

  // unknown opcode
  return -1;
}


//
// Function splitITBlockWithSTR()
//
// Description:
//   This function splits an IT block into several ones if it has at least one
//   store instruction and has at least two instructions in it. It inserts an IT 
//   instruction for each of the instruction inside the IT block and removes the 
//   original IT instruction.
//  
// Reasons for this function:
//   There are some store instructions that are within an IT block. The
//   transformation of these instructions may add extra instructions, such as a 
//   pair of add and sub intructions; those added instructions should all be 
//   conditionally executed as directed by the IT instruction. Besides, the 
//   added instructions could affect other condtion instructions in the same 
//   IT block if there are at least two instructions in it: other intructions 
//   may get squeezed outside the IT block by the newly added instructions.
//
//   Sepaprating all the instructions and their corresponding IT instruction
//   makes the implementation of the store transformation clearer.
//
// For more details about the IT instruction, see section A7.7.37 of the 
// ARMv7-M manual.
//
// Inputs:
//   MF - The Machine Function on which this pass runs.
//
// Outputs:
//   A sequence of <IT, none-IT> instructions pairs. 
//
static void splitITBlockWithSTR(MachineFunction &MF) {
  const ARMBaseInstrInfo *ABII = MF.getSubtarget<ARMSubtarget>().getInstrInfo();

  for (MachineBasicBlock &MBB : MF) {
    // We need delete the original IT if more IT instructions are generated.
    std::vector<MachineInstr *> ITInstrs; 
    for (MachineInstr &MI : MBB) {
      if (MI.getOpcode() == ARM::t2IT) {
        // number of conditional instructions in the IT block
        unsigned numOfCondInstr = 1;  

        unsigned firstcond = MI.getOperand(0).getImm();
        unsigned mask = MI.getOperand(1).getImm() & 0x0000000f;
        // Find how many succeeding instructions are controlled by this IT.
        if (mask & 0x00000001) {
          numOfCondInstr = 4;
        } else if (mask & 0x00000002) {
          numOfCondInstr = 3;
        } else if (mask & 0x00000004) {
          numOfCondInstr = 2;
        } else {
          numOfCondInstr = 1;
          // If there is only one instruction in the IT block, we don't need to
          // do any spliting.
          continue;
        }

        // Check if there is at least one store instruction.
        MachineInstr *currMI = &MI;
        bool hasStore = false;
        for (unsigned i = 0; i < numOfCondInstr; i++) {
          assert(currMI != NULL && "getNextNode() returns a NULL!\n");

          currMI = currMI->getNextNode();
          if (currMI->mayStore()) {
            hasStore = true;
          }
        }
        // If there is no store in the IT block, we don't need to split it.
        if (hasStore == false) continue;

        // Start to split the IT block if it has more than one instructions.
        //
        // Seprate the first condtion instruction.
        ITInstrs.push_back(&MI);
        currMI = MI.getNextNode();
        buildITInstr(MBB, currMI, DL, ABII, firstcond);

        unsigned firstcondLSB = firstcond & 0x00000001;
        unsigned firstcondLSBInverted = invertLSB(firstcond);

        numOfCondInstr--;
        for (unsigned i = 3; numOfCondInstr > 0; numOfCondInstr--, i--) {
          currMI = currMI->getNextNode();
          unsigned CC = (firstcondLSB == ((mask & (1u << i)) >> i)) ? 
            firstcond : firstcondLSBInverted;
          buildITInstr(MBB, currMI, DL, ABII, CC);
        }
      }
    }
    
    // Delete the original IT.
    for (MachineInstr *MI : ITInstrs) {
      MI->eraseFromParent();
    }
  }
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
  return BuildMI(MBB, MI, DL, TII->get(newOpcode), sourceReg)
          .addReg(baseReg)
          .addImm(imm).operator->();
}


// 
// Function backupReg()
//
// Description:
//   This function backs up a register on the stack. First it decreases the SP
//   by 4, and then do a store. 
//
// Inputs:
//   MBB - The MachineBasicBlock in which to insert new instruction.
//   MI - The MachineInstr before which to insert the the new instruction.
//   reg - The register to be backed up.
//   newInstrs - All newly added instructions.
//   TII - A pointer to the TargetInstrInfo structure.
//
// Outputs:
//   A sub instruction and a store instruction.
//  
//
static void backupReg(MachineBasicBlock &MBB, MachineInstr *MI, 
                unsigned reg, std::vector<MachineInstr *> &newInstrs,
                const TargetInstrInfo *TII) {
    newInstrs.push_back(BuildMI(MBB, MI, DL, TII->get(ARM::tSUBspi), ARM::SP)
      .addReg(ARM::SP).addImm(1)
      .operator->());
    newInstrs.push_back(
        buildUnprivStr(MBB, MI, reg, ARM::SP, 0, ARM::t2STRT, DL, TII));
}


//
// Function: buildITInstr()
//
// Description:
//   This function inserts an IT instruction (A7.7.37) with an condition code
//   specified in the argument list. This inserted IT instruction will only 
//   manage the following one instruction.
//
//   If a newly added store instruction comes with auxiliary instructions that 
//   that would update the processor status flags if they are outside of an IT 
//   block, we need use ARMCC::AL as the condition code for this IT; otherwise 
//   the sementics of the program may get accidentally broken.
//
// Inputs:
//   MBB - The MachineBasicBlock in which to insert the new unprivileged store.
//   MI - The MachineInstr before which to insert the the new unprivileged store.
//   DL - A reference to the DebugLoc structure.
//   TII - A pointer to the TargetInstrInfo structure.
//   condCode - firstcond of the created IT.
//   
// Outputs:
//   An IT instruction with condcode as the firstcond and 8 as the mask.
//
static void buildITInstr(MachineBasicBlock &MBB, MachineInstr *MI,
                        DebugLoc &DL, const TargetInstrInfo *TII,
                        unsigned condCode) {
  BuildMI(MBB, MI, DL, TII->get(ARM::t2IT))
    .addImm(condCode) // always execute next instruction.
    // 8 as the mask means this IT only manages the next one instruction.
    // See A7.7.37 for more details about the mask.
    .addImm(8); 
}


// 
// Function: insertITInstrIfNeeded()
//
// Description:
//   This function inserts IT instructions for each newly added instruction 
//   introduced by store transformation if that store is inside an IT block,
//   or if the added instruction changes the status flags when it is outside an
//   IT block.
//
// Inputs:
//   newInstrs - All newly added instructions for a store convertion.
//   MBB - The MachineBasicBlock in which to insert the new unprivileged store.
//   store - The store instruction that was just got converted.   
//   TII - A pointer to the TargetInstrInfo structure.
// 
// Outputs:
//   0 or more IT instruction.
//
static void insertITInstrIfNeeded(std::vector<MachineInstr *> &newInstrs,
                                 MachineBasicBlock &MBB, MachineInstr *store,
                                 const TargetInstrInfo *TII) {
  unsigned numOfNewInstrs = newInstrs.size();

  // No need to add an IT if the intrumentation of this store does not introduce
  // extra auxiliary instructions.
  if (numOfNewInstrs == 1) return;

  MachineInstr *prevMI = newInstrs[0]->getPrevNode();
  if (prevMI != NULL && prevMI->getOpcode() == ARM::t2IT) {
    // Start to insert from the second instruction.
    for (unsigned i = 1; i < numOfNewInstrs; i++) {
      buildITInstr(MBB, newInstrs[i], DL, TII, prevMI->getOperand(0).getImm());
    }
  } else {
    // The store is not inside an IT block.
    for (MachineInstr *newMI : newInstrs) {
      if (flagUpdateInstrs.find(newMI->getOpcode()) != flagUpdateInstrs.end()) {
        buildITInstr(MBB, newMI, DL, TII);
      }
    }
  }
}


//
// Function buildADDi8()
//
// Description:
//   This function builds an tADDi8 or tSUBi8 instruction as part of building an
//   unprivileged store. The reason we need a whole function to build an add/sub
//   is that tADDi8 and tSUBi8 only supports R0 - R7 as the source/destination
//   register. Sometimes the base regisger of a store is from R8 - R12, and we 
//   need use t2ADDri12 for it.
//
// Inputs:
//   MBB - The MachineBasicBlock in which to insert the new unprivileged store.
//   MI - The MachineInstr before which to insert the the new unprivileged store.
//   baseReg - The register to build this add/sub.
//   imm -  The immediate to build the add/sub.
//   arithOpcode - tADDi8 or tSUBi8
//   newInstrs - A container of all the newly added instructions for a store.
//   TII - A pointer to the TargetInstrInfo structure.
//
// Outputs:
//   A sequence of newly added instructions.
//
static void buildAddorSub(MachineBasicBlock &MBB, MachineInstr *MI, 
                      unsigned baseReg, unsigned imm, bool isAdd,
                      std::vector<MachineInstr *> &newInstrs,
                      const TargetInstrInfo *TII) {
  unsigned opcode;
  if (baseReg <= ARM::R7) {
    opcode = isAdd ? ARM::tADDi8 : ARM::tSUBi8;
    newInstrs.push_back(BuildMI(MBB, MI, DL, TII->get(opcode), baseReg)
        .addReg(baseReg).addReg(baseReg).addImm(imm)
        .operator->());
  } else {
    opcode = isAdd ? ARM::t2ADDri12 : ARM::t2SUBri12;
    newInstrs.push_back(BuildMI(MBB, MI, DL, TII->get(opcode), baseReg)
          .addReg(baseReg).addImm(imm)
          .addImm(ARMCC::AL).addReg(0)
          .operator->());
  }                     
}

//
// Method: convertSTRimm()
//
// Description:
//   This method builds an unprivileged store for a STR(immediate). Currently it 
//   only handles STR -> STRT. We need support all the other stores
//   by expanding this method or adding new method(s).
//   Note that the imm field of a STRT instruction ranges 0 - 255.
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
//   calledByOtherConverter - Whether this function is called by another store
//   conversion function.
//
// Outputs:
//   A new unprivileged store.
//
// Return:
//   None.
//
static std::vector<MachineInstr *> convertSTRimm(MachineBasicBlock &MBB,
                       MachineInstr *MI,
                       unsigned sourceReg, unsigned baseReg, int64_t imm,
                       unsigned newOpcode, 
                       DebugLoc &DL, const TargetInstrInfo *TII,
                       bool calledByOtherConverter = false) {
  std::vector<MachineInstr *> newInstrs;

  // Unprivileged stores only support positive imm. If imm is a negative, then 
  // we need to sub this imm to the base register, give 0 to the imm field of 
  // the new str, and restore the base registr.
  if (imm < 0) {
    // This sub will update the status flags; we need put it in a IT block.
    buildAddorSub(MBB, MI, baseReg, -imm, false, newInstrs, TII);
    
    // insert a new unprivileged store
    newInstrs.push_back(
        buildUnprivStr(MBB, MI, sourceReg, baseReg, 0, newOpcode, DL, TII));

    // restore the base register
    buildAddorSub(MBB, MI, baseReg, -imm, true, newInstrs, TII);
  } else {
    if (imm > 255) {
      // The range of the imm of an unprivileged store is 0 - 255. If imm > 255,
      // we need extra instructions to assist the transformation.
      
      // If the base register is SP then the imm must be a multiple of 4 to do
      // a ADD (SP + imm). Although according to the manual, the Encoding T4
      // of ADD (SP + imm) allows imm to be any value in the range o 0 - 4095,
      // I couldn't find such an ADD in the .td file. So we pick a register,
      // back it up on the stack, use this register to compute the target 
      // address, and then restore it.
      if (baseReg == ARM::SP && imm % 4 != 0) {
        // Pick R0 or R1 as the interim register.
        unsigned interimReg = (sourceReg == ARM::R0 ? ARM::R1 : ARM::R0);
        backupReg(MBB, MI, interimReg, newInstrs, TII);

        // Compute the destination address.
        newInstrs.push_back(
            BuildMI(MBB, MI, DL, TII->get(ARM::t2ADDri12), interimReg)
            .addReg(ARM::SP).addImm(imm + 4)
            .addImm(ARMCC::AL).addReg(0)
            .operator->());

        // Do the store.
        newInstrs.push_back(
            buildUnprivStr(MBB, MI, sourceReg, interimReg, 0, newOpcode, DL, TII));

        // Restore the interim register.
        newInstrs.push_back(BuildMI(MBB, MI, DL, TII->get(ARM::tPOP))
            .addImm(ARMCC::AL).addReg(0)
            .addReg(interimReg)
            .operator->());
      } else {
        // Compute the destination register.
        newInstrs.push_back(
            BuildMI(MBB, MI, DL, TII->get(ARM::t2ADDri12), baseReg)
            .addReg(baseReg).addImm(imm)
            .addImm(ARMCC::AL).addReg(0)
            .operator->());

        // Do the store.
        newInstrs.push_back(
            buildUnprivStr(MBB, MI, sourceReg, baseReg, 0, newOpcode, DL, TII));

        // Restore the base register.
        newInstrs.push_back(
            BuildMI(MBB, MI, DL, TII->get(ARM::t2SUBri12), baseReg)
            .addReg(baseReg).addImm(imm)
            .addImm(ARMCC::AL).addReg(0)
            .operator->());
      }
    } else {
      newInstrs.push_back(
          buildUnprivStr(MBB, MI, sourceReg, baseReg, imm, newOpcode, DL, TII));
    }
  }
  if (calledByOtherConverter) {
    return newInstrs;
  } else {
    insertITInstrIfNeeded(newInstrs, MBB, MI, TII);
    return std::vector<MachineInstr *>();
  }
}


//
// Method: convertSTRimmIndexed()
//
// Description:
//   This method builds an unprivileged store for a normal STR(immediate).
//   Currently it only handles STR -> STRT. We need support all the other stores
//   by expanding this method or adding new method(s).
//
//   If this instruction uses the stack pointer (SP) as the base register,
//   then we need use a special add or sub: ADD (SP plus immediate) A7.7.5,
//   or SUB (SP minute immediate). Using a normal add/sub that adds/subs an imm
//   to/from a register would lead to wrong instructions. For example, 
//
//      BuildMI(......, ARM::tADDi8, ARM::SP)
//          .addReg(ARM::SP)
//          .addReg(ARM::SP)
//          .addImm(imm);
//
//   would generate the following instruction in the final executable:
//      
//      add r5, imm
//
//   We need use ARM::tADDspi to construct the special add.
//
// Inputs:
//   MBB - The MachineBasicBlock in which to insert the new unprivileged store.
//   MI - The MachineInstr before which to insert the the new unprivileged store.
//   sourceReg - The register whose content will be stored to some memory address.
//   sourceReg2 - The second register to store. Only valid for STRD.
//   baseReg - The register used as the base register to compute the memory address.
//   imm - The immediate that is added to the baseReg to compute the memory address.
//   newOpcode - The opcode of the unprivileged store.
//   TII - A pointer to the TargetInstrInfo structure.
//
// Outputs:
//   A new unprivileged store.
//
// Return:
//   None.
//
void convertSTRimmIndexed(MachineBasicBlock &MBB, MachineInstr *MI,
                          unsigned sourceReg, unsigned sourceReg2,
                          unsigned baseReg, int64_t imm,
                          unsigned newOpcode,
                          const TargetInstrInfo *TII) {
  std::vector<MachineInstr *> newInstrs;
  unsigned opcode = MI->getOpcode();
  // Indicate the original instruction is pre-indexed or post-indexed.
  bool preIndex = (opcode == ARM::t2STR_PRE ||
                   opcode == ARM::t2STRH_PRE ||
                   opcode == ARM::t2STRB_PRE ||
                   opcode == ARM::t2STRD_PRE);

  if (preIndex == true) {
    // This is a pre-indexed store.
    // First, update the base register.
    if (imm < 0) {
      if (baseReg == ARM::SP) {
        // If the baseReg is SP, then we need to use the special sub mentioned
        // in the comment of this function.

        // For SUB (SP minus imm), imm = ZeroExtend(imm7:'00', 32)
        assert((-imm) % 4 == 0 && "IMM is not a multiple of 4.");
        imm = (-imm) >> 2;
        newInstrs.push_back(BuildMI(MBB, MI, DL, TII->get(ARM::tSUBspi), baseReg)
          .addReg(baseReg).addImm(imm)
          .operator->());
      } else {
        buildAddorSub(MBB, MI, baseReg, -imm, false, newInstrs, TII);
      }
    } else {
      // imm >= 0.
      if (baseReg == ARM::SP) {
        // For ADD (SP plus imm), imm = ZeroExtend(imm7:'00', 32)
        assert(imm % 4 == 0 && "IMM is not a multiple of 4.");
        imm >>= 2;
        newInstrs.push_back(BuildMI(MBB, MI, DL, TII->get(ARM::tADDspi), baseReg)
          .addReg(baseReg).addImm(imm)
          .operator->());
      } else {
        buildAddorSub(MBB, MI, baseReg, imm, true, newInstrs, TII);
      }
    }

    // Second, build a new store.
    newInstrs.push_back(
        buildUnprivStr(MBB, MI, sourceReg, baseReg, 0, newOpcode, DL, TII));
    if (opcode == ARM::t2STRD_PRE) {
      // Store the second register if this is a STRD.
      newInstrs.push_back(
          buildUnprivStr(MBB, MI, sourceReg2, baseReg, 4, newOpcode, DL, TII));
    }
  } else {
    // This is a post-indexed store.
    // First, build a new store.
    newInstrs.push_back(
        buildUnprivStr(MBB, MI, sourceReg, baseReg, 0, newOpcode, DL, TII));
    if (opcode == ARM::t2STRD_POST) {
      // Store the second register if this is a STRD.
      newInstrs.push_back(
          buildUnprivStr(MBB, MI, sourceReg2, baseReg, 4, newOpcode, DL, TII));
    }

    // Second, update the imm to the base register
    if (imm < 0) {
      if (baseReg == ARM::SP) {
        assert((-imm) % 4 == 0 && "IMM is not a multiple of 4.");
        newInstrs.push_back(BuildMI(MBB, MI, DL, TII->get(ARM::tSUBspi), baseReg)
          .addReg(baseReg).addImm((-imm) >> 2)
          .operator->());
      } else {
        buildAddorSub(MBB, MI, baseReg, -imm, false, newInstrs, TII);
      }
    } else {
      if (baseReg == ARM::SP) {
        assert(imm % 4 == 0 && "IMM is not a multiple of 4.");
        newInstrs.push_back(BuildMI(MBB, MI, DL, TII->get(ARM::tADDspi), baseReg)
          .addReg(baseReg).addImm(imm >> 2)
          .operator->());
      } else {
        buildAddorSub(MBB, MI, baseReg, imm, true, newInstrs, TII);
      }
    }
  }
  insertITInstrIfNeeded(newInstrs, MBB, MI, TII);
}



//
// Function: convertSTRReg()
//
// Description:
//   This function builds an unprivileged store for a STR(Register). 
//   Since unprivileged stores don't support (base register + offset register)
//   to compute the target address, we need to add the offset register to the
//   base register first, build a store, and then restore the base register.
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
//   None.
//
static void convertSTRReg(MachineBasicBlock &MBB, MachineInstr *MI,
                   unsigned sourceReg, unsigned baseReg, unsigned offsetReg,
                   unsigned newOpcode,
                   DebugLoc &DL, const TargetInstrInfo *TII) {
  std::vector<MachineInstr *> newInstrs;

  if (MI->getNumExplicitOperands() == 5) {
    // STR(register) Encoding T1; no lsl
    // Add up the base and offset registers.
    newInstrs.push_back(BuildMI(MBB, MI, DL, TII->get(ARM::tADDrr), baseReg)
      .addReg(baseReg).addReg(baseReg).addReg(offsetReg)
      .operator->());

    // Build an unprivileged store.
    newInstrs.push_back(
        buildUnprivStr(MBB, MI, sourceReg, baseReg, 0, newOpcode, DL, TII));

    // Restore the base register.
    newInstrs.push_back(BuildMI(MBB, MI, DL, TII->get(ARM::tSUBrr), baseReg)
      .addReg(baseReg).addReg(baseReg).addReg(offsetReg)
      .operator->());
  } else {
    // STR(registr) Encoding T2; with lsl
    uint8_t imm = MI->getOperand(3).getImm();
    // Add up the base and offset registers (add with lsl).
    newInstrs.push_back(BuildMI(MBB, MI, DL, TII->get(ARM::t2ADDrs), baseReg)
                        .addReg(baseReg).addReg(offsetReg)
                        .addImm(ARM_AM::getSORegOpc(ARM_AM::lsl, imm))
                        .addImm(ARMCC::AL).addReg(0)  // pred:14, pred:%noreg
                        .addReg(0)  // opt:%noreg
                        .operator->());

    // Build an unprivileged store.
    newInstrs.push_back(
        buildUnprivStr(MBB, MI, sourceReg, baseReg, 0, newOpcode, DL, TII));

    // Restore the base register.
    newInstrs.push_back(BuildMI(MBB, MI, DL, TII->get(ARM::t2SUBrs), baseReg)
      .addReg(baseReg).addReg(offsetReg)
      .addImm(ARM_AM::getSORegOpc(ARM_AM::lsl, imm)) 
      .addImm(ARMCC::AL).addReg(0)  // pred:14, pred:%noreg
      .addReg(0)  // opt:%noreg
      .operator->());
  }

  insertITInstrIfNeeded(newInstrs, MBB, MI, TII);
}


//
// Function: convertVSTR()
//
// Description:
//   This function convert a floating-point store (VSTR) to STRT. Here are the
//   algorithm:
//   1. pick general-purpose register(s) and store it/them onto the stack.
//   2. move FP register to general-purpose register(s)
//   3. create unprivileged store(s).
//   4. restore (pop) general-purpose register(s).
//
//   ARMv7-M has instructions (VMOV) that can move a double-precision register 
//   to two general-purpose registers.
//
// Inputs:
//   MBB - The MachineBasicBlock in which to insert the new unprivileged store.
//   MI - The MachineInstr before which to insert the the new unprivileged store.
//   sourceReg - The register whose contents will be stored to some memory address.
//   baseReg - The register used as the base register to compute the memory address.
//   imm - The immediate that is added to the baseReg to compute the memory address.
//   isSinglePrecision - Indicate if it is a single-precision or double-precision store.
//   DL - A reference to the DebugLoc structure.
//   TII - A pointer to the TargetInstrInfo structure.
//   calledByOtherConverter - Whether this function is called by another store
//   conversion function.
//
// Outputs:
//   One or more unprivileged stores, one extra push, and one extra pop.
//
static std::vector<MachineInstr *> convertVSTR(MachineBasicBlock &MBB,
                           MachineInstr *MI,
                           unsigned sourceReg, unsigned baseReg, uint16_t imm,
                           bool isSinglePrecision,
                           DebugLoc &DL, const TargetInstrInfo *TII,
                           bool calledByOtherConverter = false) {
  std::vector<MachineInstr *> newInstrs;

  unsigned newOpcode = ARM::t2STRT;
  unsigned R0 = ARM::R0, R1 = ARM::R1, SP = ARM::SP;

  if (isSinglePrecision) {
    // store a single-precision register 
    
    // First, pick a general-purpose reigster.
    // There is a potential pitfall here: we cannot pick the base register 
    // otherwise it'd destroy the destination address to store.
    //
    unsigned interimReg = baseReg == R0 ? R1 : R0;
    
    // Second, store the selected register onto the stack.
    backupReg(MBB, MI, interimReg, newInstrs, TII);

    // Don't forget this.
    if (baseReg == SP) imm += 4;

    // Third, move from FP register to the general-purpose register.
    newInstrs.push_back(BuildMI(MBB, MI, DL, TII->get(ARM::VMOVRS), interimReg)
      .addReg(sourceReg)
      .addImm(ARMCC::AL).addReg(0)  // pred:14, pred:%noreg
      .operator->());

    // Forth, create an unprivileged store. 
    std::vector<MachineInstr *> newInstrsSTRImm = 
      convertSTRimm(MBB, MI, interimReg, baseReg, imm, newOpcode, DL, TII, true);
    newInstrs.insert(newInstrs.end(), newInstrsSTRImm.begin(), newInstrsSTRImm.end());

    // Finally, restore the general-purpose register.
    newInstrs.push_back(BuildMI(MBB, MI, DL, TII->get(ARM::tPOP))
      .addImm(ARMCC::AL).addReg(0)
      .addReg(interimReg)
      .operator->());
  } else {
    // store a double-precision register (two single-precision registers)
    
    // First, pick two general-purpose reigsters. We just pick R0 and R1 if 
    // baseReg is neither R0 nor R1; otherwise pick R2 and R3. 
    // Weirdly, if we pick R7 and R8 (or larger number registers), and use
    // BuildMI to create push/pop, the generated binary would only have 
    // push/pop {r7}. Is this a bug of LLVM?
    unsigned interimReg0, interimReg1;
    if (baseReg == R0 || baseReg == R1) {
      interimReg0 = ARM::R2, interimReg1 = ARM::R3;
    } else {
      interimReg0 = ARM::R0, interimReg1 = ARM::R1;
    }

    // Second, store the two selected registers onto the stack.
    newInstrs.push_back(BuildMI(MBB, MI, DL, TII->get(ARM::tSUBspi), SP)
      .addReg(SP).addImm(2)
      .operator->());
    newInstrs.push_back(
        buildUnprivStr(MBB, MI, interimReg0, SP, 0, ARM::t2STRT, DL, TII));
    newInstrs.push_back(
        buildUnprivStr(MBB, MI, interimReg1, SP, 4, ARM::t2STRT, DL, TII));

    // Don't forget this.
    if (baseReg == ARM::SP) imm += 8;

    // Third, move the double word to the two general-purpose registers.
    newInstrs.push_back(BuildMI(MBB, MI, DL, TII->get(ARM::VMOVRRD))
      .addReg(interimReg0).addReg(interimReg1)
      .addReg(sourceReg)
      .addImm(ARMCC::AL).addReg(0)  // pred:14, pred:%noreg
      .operator->());

    // Forth, build unprivileged stores.
    std::vector<MachineInstr *> newInstrsSTRImm = 
      convertSTRimm(MBB, MI, interimReg0, baseReg, imm, newOpcode, DL, TII, true);
    newInstrs.insert(newInstrs.end(), newInstrsSTRImm.begin(), newInstrsSTRImm.end());
    newInstrsSTRImm = 
      convertSTRimm(MBB, MI, interimReg1, baseReg, imm + 4, newOpcode, DL, TII, true);
    newInstrs.insert(newInstrs.end(), newInstrsSTRImm.begin(), newInstrsSTRImm.end());

    // Last, restore r0, r1.
    newInstrs.push_back(BuildMI(MBB, MI, DL, TII->get(ARM::tPOP))
      .addImm(ARMCC::AL).addReg(0)  // pred:14, pred:%noreg
      .addReg(interimReg0).addReg(interimReg1)
      .operator->());
  } 

  if (calledByOtherConverter == true) {
    return newInstrs;
  } else {
    insertITInstrIfNeeded(newInstrs, MBB, MI, TII);
    return std::vector<MachineInstr *>();
  }
}


//
// Function: convertSTM()
//
// Description:
//   This function converts a store-multiple-word, i.e., STM or STMDB, to 
//   multiple STRT. It builds multiple STRT for all the registers, and then
//   update the base register as needed.
//
// Inputs:
//   MBB - The MachineBasicBlock in which to insert the new unprivileged store.
//   MI - The MachineInstr before which to insert the the new unprivileged store.
//   baseReg - The register used as the base register to get the memory address.
//   DL - A reference to the DebugLoc structure.
//   TII - A pointer to the TargetInstrInfo structure.
//
// Outputs:
//   One or more STRT and one or two add/sub as needed.
//   
static void convertSTM(MachineBasicBlock &MBB, MachineInstr *MI,
                 unsigned baseReg,
                 DebugLoc &DL, const TargetInstrInfo *TII) {
  std::vector<MachineInstr *> newInstrs;

  unsigned opcode = MI->getOpcode();
  unsigned newOpcode = ARM::t2STRT;

  unsigned numOfReg; 
  std::vector<unsigned> regList;

  // Get the list of reigsters to store.
  if (opcode == ARM::t2STMIA || opcode == ARM::t2STMDB) {
    // The non-write-back and write-back stores have different encodings.
    // For t2STMIA, the MI operands are: baseReg, pred:14, pred:%noreg, and 
    // register list, while for an write-back store, the MI operands are 
    // baseReg (def), baseReg (use), pred:14, pred:%noreg, and register list.
    //
    // Potential hazard: I've never seen t2STMDB in test programs; I'm not sure
    // if t2STMDB has the similar MI encoding as t2STMIA.
    numOfReg = MI->getNumOperands() - 3;
    for (unsigned i = 0; i < numOfReg; i++) {
      regList.push_back(MI->getOperand(i + 3).getReg());
    }
  } else {
    numOfReg = MI->getNumOperands() - 4;
    for (unsigned i = 0; i < numOfReg; i++) {
      regList.push_back(MI->getOperand(i + 4).getReg());
    }
  }
  
  if (opcode == ARM::tSTMIA_UPD ||   // Encoding T1; must write back
      opcode == ARM::t2STMIA ||      // Encoding T2; no write back
      opcode == ARM::t2STMIA_UPD) {  // Encoding T2; with write back
    // STM: Store Multiple
    
    // Store all registers to addresses starting from baseReg
    for (unsigned i = 0; i < numOfReg; i++) {
      newInstrs.push_back(
          buildUnprivStr(MBB, MI, regList[i], baseReg, i * 4, newOpcode, DL, TII));
    }

    // If this is a write-back store, update the baseReg.
    if (opcode != ARM::t2STMIA) {
      if (baseReg == ARM::SP) {
        newInstrs.push_back(BuildMI(MBB, MI, DL, TII->get(ARM::tADDspi), baseReg)
          .addReg(baseReg).addImm(numOfReg)
          .operator->());
      } else {
        buildAddorSub(MBB, MI, baseReg, numOfReg << 2, true, newInstrs, TII);
      }
    }
  } else {
    // STMDB: Store Multiple Decrement Before
    // First, sub the base register to be the starting address of store.
    if (baseReg == ARM::SP) {
      newInstrs.push_back(BuildMI(MBB, MI, DL, TII->get(ARM::tSUBspi), baseReg)
        .addReg(baseReg).addImm(numOfReg)
        .operator->());
    } else {
      buildAddorSub(MBB, MI, baseReg, numOfReg << 2, false, newInstrs, TII);
    }

    // Store all the registers.
    for (unsigned i = 0; i < numOfReg; i++) {
      newInstrs.push_back(
          buildUnprivStr(MBB, MI, regList[i], baseReg, i * 4, newOpcode, DL, TII));
    }

    // If this is not a write-back store, we need restore base register.
    if (opcode == ARM::t2STMDB) {
      if (baseReg == ARM::SP) {
        newInstrs.push_back(BuildMI(MBB, MI, DL, TII->get(ARM::tADDspi), baseReg)
          .addReg(baseReg).addImm(numOfReg)
          .operator->());
      } else {
        buildAddorSub(MBB, MI, baseReg, numOfReg << 2, true, newInstrs, TII);
      }
    }
  }

  insertITInstrIfNeeded(newInstrs, MBB, MI, TII);
}


// Function convertPUSH()
//
// Description:
//   This function converts a PUSH to one or multiple STRT and an addition sub
//   (update the SP). 
//   A push is essentially a STM with update. Maybe we should combine this 
//   function with convertSTM().
//
// Inputs:
//   MBB - The MachineBasicBlock in which to insert the new unprivileged store.
//   MI - The MachineInstr before which to insert the the new unprivileged store.
//   DL - A reference to the DebugLoc structure.
//   TII - A pointer to the TargetInstrInfo structure.
//
// Outputs:
//   One or more STRT and a SUB (SP minus immediate).
//
static void convertPUSH(MachineBasicBlock &MBB, MachineInstr *MI,
                        DebugLoc &DL, const TargetInstrInfo *TII) {
  unsigned SP = ARM::SP;
  unsigned opcode = MI->getOpcode();

  // Get the register list.
  unsigned numOfReg = MI->getNumOperands() - 4;
  std::vector<unsigned> regList;
  if (opcode == ARM::tPUSH) {
    for (unsigned i = 2; i < numOfReg + 2; i++) {
      regList.push_back(MI->getOperand(i).getReg());
    }
  } else {
    // It is a t2STMDB_UPD.
    for (unsigned i = 4; i < numOfReg + 4; i++) {
      regList.push_back(MI->getOperand(i).getReg());
    }
  }

  // Update SP.
  BuildMI(MBB, MI, DL, TII->get(ARM::tSUBspi), SP)
    .addReg(SP)
    .addImm(numOfReg);

  for (unsigned i = 0; i < numOfReg; i++) {
    buildUnprivStr(MBB, MI, regList[i], SP, i * 4, ARM::t2STRT, DL, TII);
  }
}


//
// Function convertVSTM()
//
// Description:
//   This function converts a VSTM to multiple STRT.
//
// Inputs:
//   MBB - The MachineBasicBlock in which to insert the new unprivileged store.
//   MI - The MachineInstr before which to insert the the new unprivileged store.
//   baseReg - The register used as the base register to get the memory address.
//   DL - A reference to the DebugLoc structure.
//   TII - A pointer to the TargetInstrInfo structure.
//
// Outputs:
//   Multiple STRT and a sub to update the SP.
//
static void convertVSTM(MachineBasicBlock &MBB, MachineInstr *MI,
                 DebugLoc &DL, const TargetInstrInfo *TII) {
  std::vector<MachineInstr *> newInstrs;
  std::vector<MachineInstr *> newInstrsVSTR;

  unsigned SP = ARM::SP;  // for the convenience of typing
  unsigned opcode = MI->getOpcode();
  bool isSinglePrecision = (opcode == ARM::VSTMSDB_UPD || 
                            opcode == ARM::VSTMSIA_UPD);
  bool isPush = (opcode == ARM::VSTMSDB_UPD || opcode == ARM::VSTMDDB_UPD);
  unsigned memReg = isPush ? SP : MI->getOperand(0).getReg();

  // Get the register list.
  // The register list of VSTMDIA starts from the fourth operand; all others'
  // start from the fifth.
  unsigned regListLoc = (opcode == ARM::VSTMDIA ? 3 : 4);
  unsigned numOfReg = MI->getNumOperands() - regListLoc;
  std::vector<unsigned> regList;
  for (unsigned i = 0; i < numOfReg; i++) {
    regList.push_back(MI->getOperand(i + regListLoc).getReg());
  }

  // Store multiple floating-point registers.
  if (isSinglePrecision) {
    if (isPush) {
      // Update SP. 
      newInstrs.push_back(BuildMI(MBB, MI, DL, TII->get(ARM::tSUBspi), SP)
          .addReg(SP).addImm(numOfReg)
          .operator->());
    }

    // Store all registers.
    for (unsigned i = 0; i < numOfReg; i++) {
      newInstrsVSTR = convertVSTR(MBB, MI, regList[i], memReg, i * 4, true, DL, TII, true);
      newInstrs.insert(newInstrs.end(), newInstrsVSTR.begin(), newInstrsVSTR.end());
    }

    // If this is VSTMSIA_UPD we need update the base register.
    if (!isPush) {
      buildAddorSub(MBB, MI, memReg, numOfReg << 2, true, newInstrs, TII);
    }
  } else {
    if (isPush) {
      newInstrs.push_back(BuildMI(MBB, MI, DL, TII->get(ARM::tSUBspi), SP)
          .addReg(SP).addImm(numOfReg * 2)
          .operator->());
    }

    // Store all registers.
    for (unsigned i = 0; i < numOfReg; i++) {
      newInstrsVSTR = convertVSTR(MBB, MI, regList[i], memReg, i * 8, false, DL, TII, true);
      newInstrs.insert(newInstrs.end(), newInstrsVSTR.begin(), newInstrsVSTR.end());
    }

    // If this is VSTMDIA_UPD we need update the base register.
    if (opcode == ARM::VSTMDIA_UPD) {
      buildAddorSub(MBB, MI, memReg, numOfReg << 3, true, newInstrs, TII);
    }
  }

  insertITInstrIfNeeded(newInstrs, MBB, MI, TII);

  // TO-DO: there is an optimization we can do here. 
  // When storing multiple FP registers to consecutive memory addresses, for 
  // each store, we store one or two interim registers onto the stack, use them, 
  // and then pop back. Thus, there'd be unnecessary (and consecutive) 
  // <pop, store> pairs generatd. We can remove these <pop, push> pairs to save 
  // both space and running time.
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
// Method: debugHelper()
//
// Description:
//   This method helps to print all kinds of information for the purpose of 
//   debugging. For example, if you want to know the opcode of some instruction,
//   you can add 
//     errs() << "some_instr's opcode = " << ARM::some_instr" << "\n"
//   in this method.
//   Putting all the printing or dumping in this method makes the code cleaner.
//
// Inputs:
//   MF - A reference to the MachineFunction this pass is currently processing.
//
// Outputs:
//   Debugging information.
//
// Return Value:
//   None
void debugHelper(MachineFunction &MF) {
  if (MF.getName() != "main") return;

  errs() << "R0 = " << ARM::R0 << ", R12 = " << ARM::R12 << ".\n";
  errs() << "SP = " << ARM::SP << ", PC = " << ARM::PC << ".\n";
#if 0
  errs() << "S0 = " << ARM::S0 << ", S1 = " << ARM::S1 << ".\n";
  errs() << "D0 = " << ARM::D0 << ", D1 = " << ARM::D1 << ".\n";
#endif
}


// 
// Method: printOperands()
//
// Description:
//   This method dumps a MachineInstr and all its explicit operands.
//
// Inputs:
//   MI - A referecne to the MachineInstr to be examined.
//
// Outputs:
//   The dump of the MachineInstr and its operands.
static void printOperands(MachineInstr &MI) {
  errs() << "(function: " << MI.getParent()->getParent()->getName() << ")\n";
  MI.dump();

  unsigned numOperands = MI.getNumOperands();
  errs() << "Number of operands: " << numOperands << "\n";
  for (unsigned i = 0; i < numOperands; i++) {
    errs() << "Operand type = " << MI.getOperand(i).getType() << "; ";
    MI.getOperand(i).dump();
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
bool ARMSilhouetteSTR2STRT::runOnMachineFunction(MachineFunction &MF) {
  StringRef funcName = MF.getName();
  // skip certain functions
  if (funcBlacklist.find(funcName) != funcBlacklist.end()) return false;

#if 0
  // instrument certain functions
  if (funcWhitelist.find(funcName) == funcWhitelist.end()) return false;
#endif

#if 0
  // for debugging
  errs() << "Silhouette: hello from function: " << funcName << "\n";
#endif

  // Compute the code size of the original machine function.
  unsigned codeSize = getFuncCodeSize(MF); 
  unsigned codeSizeNew = 0;

  // Split an IT block if it has at least one store instruction.
  splitITBlockWithSTR(MF);

  const TargetInstrInfo *TII = MF.getSubtarget<ARMSubtarget>().getInstrInfo();
  DebugLoc DL;

  // iterate over all MachineInstr
  for (MachineBasicBlock &MBB : MF) {
    std::vector<MachineInstr *> originalStores; // Need delete the original stores.
    for (MachineInstr &MI : MBB) {
      if (MI.mayStore() == false) continue;

      unsigned opcode = MI.getOpcode();
      unsigned sourceReg = 0;
      unsigned sourceReg2 = 0; // for STRD
      unsigned baseReg = 0;
      unsigned offsetReg = 0;  // for STR(register) 
      int64_t imm = 0;
      unsigned newOpcode = getNewOpcode(opcode);

      // Don't do anything to Shadow Stack instructions
      if (MI.getFlag(MachineInstr::ShadowStack))
        continue;

      switch (opcode) {
        // Integer stores.
        //
        // store immediate; A7.7.158 STR (immediate)
        case ARM::tSTRi:      // Encoding T1: STR<c> <Rt>, [<Rn>{,#<imm5>}]
        case ARM::tSTRspi:    // Encoding T2: STR<c> <Rt>, [SP, #<imm8>]
        case ARM::t2STRi8:    // Encoding T3: STR<c> <Rt>,[SP,#<imm8>]
        case ARM::t2STRi12:   // Encoding T3: STR<c>.W <Rt>,[<Rn>,#<imm12>]
        // store halfword immediate; A7.7.167 STRH (immiedate)
        case ARM::tSTRHi:     // Encoding T1: STRH<c> <Rt>,[<Rn>{,#<imm5>}]
        case ARM::t2STRHi12:  // Encoding T2: STRH<c>.W <Rt>,[<Rn>{,#<imm12>}]
        case ARM::t2STRHi8:   // Encoding T3: STRH<c> <Rt>,[<Rn>,#-<imm8>]
        // store byte immediate; A7.7.160 STRB(immediate)
        case ARM::tSTRBi:     // A7.7.160 Encoding T1
        case ARM::t2STRBi12:  // Encoding T2: STRB<c>.W <Rt>,[<Rn>,#<imm12>]
        case ARM::t2STRBi8:   // Encoding T3: STRB<c> <Rt>,[<Rn>,#-<imm8>]
#if 1
          sourceReg = MI.getOperand(0).getReg();
          baseReg = MI.getOperand(1).getReg();
          imm = MI.getOperand(2).getImm();
          if (opcode == ARM::tSTRi || opcode == ARM::tSTRspi) {
            // The imm of a these two stores = ZeroExtend(imm: "00", 32); 
            // but the  imm of the unprivileged store is not ZeroExtended. 
            imm <<= 2; 
          } else if (opcode == ARM::tSTRHi) {
            // The imm of this store = ZeroExtend(imm5:'0',32).
            imm <<= 1;
          }
          convertSTRimm(MBB, &MI, sourceReg, baseReg, imm, newOpcode, DL, TII);
          originalStores.push_back(&MI);
#endif
          break;

        // stores with write-back
        // store word; A7.7.158 Encoding T4: STR<c> <Rt>,[<Rn>,#+/-<imm8>]!
        case ARM::t2STR_PRE:  // pre-indexed store
        case ARM::t2STR_POST: // post-indexed store
        // store halfword; A7,7,167 Encoding T3: STRH<c> <Rt>,[<Rn>,#+/-<imm8>]!
        case ARM::t2STRH_PRE:   // pre-indexed store
        case ARM::t2STRH_POST:  // post-index store
        // store byte; A7.7.160 Encoding T3: STRB<c> <Rt>,[<Rn>,#+/-<imm8>]!
        case ARM::t2STRB_PRE:   // pre-indexed store
        case ARM::t2STRB_POST:  // post-index store
#if 1
          sourceReg = MI.getOperand(1).getReg();  
          baseReg = MI.getOperand(0).getReg(); // the reg to be updated
          imm = MI.getOperand(3).getImm();
          // There is no second base register for these three kinds of stores.
          convertSTRimmIndexed(MBB, &MI, sourceReg, 0, baseReg, imm, newOpcode, TII);
          originalStores.push_back(&MI);
#endif
          break;
        
        // store register; A7.7.159 
        case ARM::tSTRr:   // Encoding T1: STR<c> <Rt>,[<Rn>,<Rm>]
        case ARM::t2STRs:  // Encoding T2: STR<c>.W <Rt>,[<Rn>,<Rm>{,LSL #<imm2>}]
        // store register halfword; A7.7.168
        case ARM::tSTRHr:  // Encoding T1: STRH<c> <Rt>,[<Rn>,<Rm>]
        case ARM::t2STRHs: // Encoding T2: STRH<c>.W <Rt>,[<Rn>,<Rm>{,LSL #<imm2>}]
        // store register byte; A7.7.161
        case ARM::tSTRBr:  // Encoding T1: STRB<c> <Rt>,[<Rn>,<Rm>]
        case ARM::t2STRBs: // Encoding T2: STRB<c>.W <Rt>,[<Rn>,<Rm>{,LSL #<imm2>}]
#if 1
          sourceReg = MI.getOperand(0).getReg();
          baseReg = MI.getOperand(1).getReg();
          offsetReg = MI.getOperand(2).getReg();
          convertSTRReg(MBB, &MI, sourceReg, baseReg, offsetReg, newOpcode, DL, TII);
          originalStores.push_back(&MI);
#endif
          break;

        // STRD (immediate); A7.7.163; Encoding T1
        case ARM::t2STRDi8:  // no write back
#if 1
          sourceReg = MI.getOperand(0).getReg();
          sourceReg2 = MI.getOperand(1).getReg();
          baseReg = MI.getOperand(2).getReg();
          imm = MI.getOperand(3).getImm();
          // Although the imm is ZeroExtended by two bits, we don't need do 
          // anthing here because the MIR is already encoded with the real imm.
          convertSTRimm(MBB, &MI, sourceReg, baseReg, imm, newOpcode, DL, TII);
          convertSTRimm(MBB, &MI, sourceReg2, baseReg, imm + 4, newOpcode, DL, TII);
          originalStores.push_back(&MI);
#endif
          break;
        
        // STRD (immediate) with write back
        case ARM::t2STRD_PRE:
        case ARM::t2STRD_POST:
#if 1
          sourceReg = MI.getOperand(1).getReg();
          sourceReg2 = MI.getOperand(2).getReg();
          baseReg = MI.getOperand(0).getReg();
          imm = MI.getOperand(4).getImm();
          convertSTRimmIndexed(MBB, &MI, sourceReg, sourceReg2, baseReg, imm, newOpcode, TII);
          originalStores.push_back(&MI);
#endif
          break;

        // Floating stores.
        case ARM::VSTRS:
        case ARM::VSTRD:
#if 1
          sourceReg = MI.getOperand(0).getReg();
          baseReg = MI.getOperand(1).getReg();
          // For negative imm, LLVM encodes it as a positive here. 
          // We need use 2^8 - imm to get the real immediate.
          imm = MI.getOperand(2).getImm();
          imm = imm >= 256 ? ((256 - imm) << 2) : (imm << 2);
          convertVSTR(MBB, &MI, sourceReg, baseReg, imm, opcode == ARM::VSTRS, DL, TII);
          originalStores.push_back(&MI);
#endif
          break;

        // Store multiple words (integer).
        case ARM::tSTMIA_UPD:   // A7.7.156 Encoding T1: STM<c> <Rn>!,<registers>
        case ARM::t2STMIA:      // A7.7.156 Encoding T2; no write back
        case ARM::t2STMIA_UPD:  // A7.7.156 Encoding T2; with write back
        case ARM::t2STMDB:      // A7.7.157 Encoding T1; no write back
#if 1
          baseReg = MI.getOperand(0).getReg();
          convertSTM(MBB, &MI, baseReg, DL, TII);
          originalStores.push_back(&MI);
#endif
          break;

        // Push one or more registers.
        // Push is a special case of STM.
        case ARM::tPUSH:            // A7.7.99 Encoding T1;
        case ARM::t2STMDB_UPD:      // A7.7.99 Encoding T2; 
#if 1
          convertPUSH(MBB, &MI, DL, TII);
          originalStores.push_back(&MI);
#endif
          break;

        // Store multiple FP registers.
        // VSTMDDB_UPD and VSTMSDB_UPD are aliases for vpush. 
        case ARM::VSTMDDB_UPD:  // VPUSH double-precision registers
        case ARM::VSTMSDB_UPD:  // VPUSH single-precision registers
        case ARM::VSTMDIA_UPD:  // vstmia double-precision then update
        case ARM::VSTMSIA_UPD:  // vstmia single-precision then update
        case ARM::VSTMDIA:      // vstmia double-precision no update
#if 1
          convertVSTM(MBB, &MI, DL, TII);
          originalStores.push_back(&MI);
#endif
          break;

        // inline assembly
        case ARM::INLINEASM:
          // TO-DO?
          break;

        default:
          if (MI.mayStore()) {
#if 1
            errs() << "Silhouette: other stores; dump: ";
            printOperands(MI);
#endif
          }
          break;
      }
    }

    // remove the original stores
    for (MachineInstr *MI : originalStores) {
      MI->eraseFromParent();
    }
  }

  // Compute the code size of the transformed machine function.
  codeSizeNew = getFuncCodeSize(MF);

  // Write the result to a file.
  // The code size string is very small (funcName + original_code_size +
  // new_code_size); raw_fd_ostream's "<<" operator ensures that the write is 
  // atomic because essentially it uses write() to write to a file and according
  // to http://man7.org/linux/man-pages/man7/pipe.7.html, any write with less
  // than PIPE_BUF bytes (at least 4096) is guaranteed to be atomic.
  std::error_code EC;
  StringRef memStatFile("./code_size.stat");
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
  FunctionPass *createARMSilhouetteSTR2STRT(void) {
    return new ARMSilhouetteSTR2STRT();
  }
}
