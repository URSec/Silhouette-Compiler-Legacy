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
#include "ARMSilhouetteSTR2STRT.h"
#include "ARMSilhouetteConvertFuncList.h"

#include <vector>

using namespace llvm;

// number of general-purpose registers R0 - R12, excluding SP, PC, and LR.
#define GP_REGULAR_REG_NUM 13  

char ARMSilhouetteSTR2STRT::ID = 0;

ARMSilhouetteSTR2STRT::ARMSilhouetteSTR2STRT() : MachineFunctionPass(ID) {
  return;
}

StringRef ARMSilhouetteSTR2STRT::getPassName() const {
  return "ARM Silhouette store to store unprivileged convertion Pass";
}


// function declration
static void printOperands(MachineInstr &MI);


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
    case ARM::VSTMDDB_UPD: // A7.7.249 Encoding T1; double-precision registers
    case ARM::VSTMSDB_UPD: // A7.7.249 Encoding T1; single-precision registers
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
// Method: buildUnprivStr
//
// Description:
//   This method builds an unprivileged store instruction.
//   Unprivileged load/store instructions only support 8-bit immediate, ranging
//   from 0 to 255; but a normal store can have an immediate as large as 1023.
//   For an immediate greater than 255, we need to use extra add and sub 
//   instructions. See the code for details.
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
static void buildUnprivStr(MachineBasicBlock &MBB,
                      MachineInstr *MI,
                      unsigned sourceReg, unsigned baseReg, uint64_t imm,
                      unsigned newOpcode,
                      DebugLoc &DL,
                      const TargetInstrInfo *TII) {
  if (imm <= 255) {
    // If the imm is less than 256, then just create an unprivileged store.
    BuildMI(MBB, MI, DL, TII->get(newOpcode))
      .addReg(sourceReg)
      .addReg(baseReg)
      .addImm(imm);
  } else {
      // If the imm is greater than 255, we need to add the imm to the base 
      // register first, create an unprivileged store, and then restore the 
      // base register.
      BuildMI(MBB, MI, DL, TII->get(ARM::t2ADDri12), baseReg)
        .addReg(baseReg)
        .addImm(imm)
        .addImm(ARMCC::AL).addReg(0);

      buildUnprivStr(MBB, MI, sourceReg, baseReg, 0, newOpcode, DL, TII);

      BuildMI(MBB, MI, DL, TII->get(ARM::t2SUBri12), baseReg)
        .addReg(baseReg)
        .addImm(imm)
        .addImm(ARMCC::AL).addReg(0);
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
//
// Outputs:
//   A new unprivileged store.
//
// Return:
//   None.
//
void convertSTRimm(MachineBasicBlock &MBB,
                     MachineInstr *MI,
                     unsigned sourceReg, unsigned baseReg, int64_t imm,
                     unsigned newOpcode,
                     DebugLoc &DL,
                     const TargetInstrInfo *TII) {
  // Unprivileged stores only support positive imm. If imm is a negative, then 
  // we need to sub this imm to the base register, give 0 to the imm field of 
  // the new str, and restore the base registr.
  if (imm < 0) {
    BuildMI(MBB, MI, DL, TII->get(ARM::tSUBi8), baseReg)
      .addReg(baseReg)
      .addReg(baseReg)
      .addImm(-imm);
    
    // insert a new unprivileged store
    buildUnprivStr(MBB, MI, sourceReg, baseReg, 0, newOpcode, DL, TII);

    // restore the base register
    BuildMI(MBB, MI, DL, TII->get(ARM::tADDi8), baseReg)
      .addReg(baseReg)
      .addReg(baseReg)
      .addImm(-imm);
  } else {
    buildUnprivStr(MBB, MI, sourceReg, baseReg, imm, newOpcode, DL, TII);
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
//   sourceReg - The register whose contents will be stored to some memory address.
//   baseReg - The register used as the base register to compute the memory address.
//   imm - The immediate that is added to the baseReg to compute the memory address.
//   newOpcode - The opcode of the unprivileged store.
//   preIndex - Indicate the original instruction is pre-indexed or post-indexed.
//   DL - A reference to the DebugLoc structure.
//   TII - A pointer to the TargetInstrInfo structure.
//
// Outputs:
//   A new unprivileged store.
//
// Return:
//   None.
//
void convertSTRimmIndexed(MachineBasicBlock &MBB,
                          MachineInstr *MI,
                          unsigned sourceReg, unsigned baseReg, int64_t imm,
                          bool preIndex,
                          unsigned newOpcode,
                          DebugLoc &DL,
                          const TargetInstrInfo *TII) {
  if (preIndex == true) {
    // This is a pre-indexed store.
    // First, update the base register.
    if (imm < 0) {
      if (baseReg == ARM::SP) {
        // If the baseReg is SP, then we need to use the special sub mentioned
        // in the comment of this function.

        // For SUB (SP minus imm), imm = ZeroExtend(imm7:'00', 32)
        assert((-imm) % 4 == 0 && "IMM is not a multiple of 4");

        imm = (-imm) >> 2;
        BuildMI(MBB, MI, DL, TII->get(ARM::tSUBspi), baseReg)
          .addReg(baseReg)
          .addImm(imm);
      } else {
        BuildMI(MBB, MI, DL, TII->get(ARM::tSUBi8))
          .addReg(baseReg)
          .addReg(baseReg)
          .addImm(-imm);
      }
    } else {
      // imm >= 0.
      if (baseReg == ARM::SP) {
        // For ADD (SP plus imm), imm = ZeroExtend(imm7:'00', 32)
        assert(imm % 4 == 0 && "IMM is not a multiple of 4");
        imm >>= 2;
        BuildMI(MBB, MI, DL, TII->get(ARM::tADDspi), baseReg)
          .addReg(baseReg)
          .addImm(imm);
      } else {
        BuildMI(MBB, MI, DL, TII->get(ARM::tADDi8), baseReg)
          .addReg(baseReg)
          .addReg(baseReg)
          .addImm(imm);
      }
    }

    // Second, build a new store.
    buildUnprivStr(MBB, MI, sourceReg, baseReg, 0, newOpcode, DL, TII);

  } else {
    // This is a post-indexed store.
    // First, build a new store.
    buildUnprivStr(MBB, MI, sourceReg, baseReg, 0, newOpcode, DL, TII);

    // Second, update the imm to the base register
    if (imm < 0) {
      if (baseReg == ARM::SP) {
        assert((-imm) % 4 == 0 && "IMM is not a multiple of 4");
        
        imm = (-imm) >> 2;
        BuildMI(MBB, MI, DL, TII->get(ARM::tSUBspi), baseReg)
          .addReg(baseReg)
          .addImm(imm);
      } else {
        BuildMI(MBB, MI, DL, TII->get(ARM::tSUBi8), baseReg)
          .addReg(baseReg)
          .addReg(baseReg)
          .addImm(-imm);
      }
    } else {
      if (baseReg == ARM::SP) {
        assert(imm % 4 == 0 && "IMM is not a multiple of 4");
        BuildMI(MBB, MI, DL, TII->get(ARM::tADDspi), baseReg)
          .addReg(baseReg)
          .addImm(imm);
      } else {
        BuildMI(MBB, MI, DL, TII->get(ARM::tADDi8), baseReg)
          .addReg(baseReg)
          .addReg(baseReg)
          .addImm(imm);
      }
    }
  }
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
void convertSTRReg(MachineBasicBlock &MBB, MachineInstr *MI,
                   unsigned sourceReg, unsigned baseReg, unsigned offsetReg,
                   unsigned newOpcode,
                   DebugLoc &DL,
                   const TargetInstrInfo *TII) {
  if (MI->getNumExplicitOperands() == 5) {
    // STR(register) Encoding T1; no lsl
    // Add up the base and offset registers.
    BuildMI(MBB, MI, DL, TII->get(ARM::tADDrr), baseReg)
      .addReg(baseReg)
      .addReg(baseReg)
      .addReg(offsetReg);

    // Build an unprivileged store.
    buildUnprivStr(MBB, MI, sourceReg, baseReg, 0, newOpcode, DL, TII);

    // Restore the base register.
    BuildMI(MBB, MI, DL, TII->get(ARM::tSUBrr), baseReg)
      .addReg(baseReg)
      .addReg(baseReg)
      .addReg(offsetReg);
  } else {

    // STR(registr) Encoding T2; with lsl
    uint8_t imm = MI->getOperand(3).getImm();
    // Add up the base and offset registers (add with lsl).
    BuildMI(MBB, MI, DL, TII->get(ARM::t2ADDrs), baseReg)
      .addReg(baseReg).addReg(offsetReg)
      .addImm(ARM_AM::getSORegOpc(ARM_AM::lsl, imm))
      .addImm(ARMCC::AL).addReg(0)  // pred:14, pred:%noreg
      .addReg(0);  // opt:%noreg

    // Build an unprivileged store.
    buildUnprivStr(MBB, MI, sourceReg, baseReg, 0, newOpcode, DL, TII);

    // Restore the base register.
    BuildMI(MBB, MI, DL, TII->get(ARM::t2SUBrs), baseReg)
      .addReg(baseReg).addReg(offsetReg)
      .addImm(ARM_AM::getSORegOpc(ARM_AM::lsl, imm)) 
      .addImm(ARMCC::AL).addReg(0)  // pred:14, pred:%noreg
      .addReg(0);  // opt:%noreg
  }
}


//
// Function: convertVSTR()
//
// Description:
//   This function convert a floating-point store (VSTR) to STRT. Here are the
//   algorithm:
//   1. pick general-purpose register(s) and push it/them onto the stack.
//   2. move FP register to general-purpose register(s)
//   3. create unprivileged store(s).
//   4. restore (pop) general-purpose register(s).
//
//   ARMv7-M has nice push/pop instructions that can push/pop multiple registers
//   with one single instruction. It also has instructions (VMOV) that can move
//   a double-precision register to two general-purpose registers.
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
//
// Outputs:
//   One or more unprivileged stores, one extra push, and one extra pop.
//
void convertVSTR(MachineBasicBlock &MBB, MachineInstr *MI,
                 unsigned sourceReg, unsigned baseReg, uint16_t imm,
                 bool isSinglePrecision,
                 DebugLoc &DL, const TargetInstrInfo *TII) {
  unsigned newOpcode = ARM::t2STRT;
  unsigned R0 = ARM::R0, R1 = ARM::R1, SP = ARM::SP;
  unsigned baseRegNum = baseReg - R0; 

  if (isSinglePrecision) {
    // store a single-precision register 
    
    // First, pick a general-purpose reigster.
    // There is a potential pitfall here: we cannot pick the base register 
    // otherwise it'd destroy the destination address to store.
    //
    // Here we use a O(1) algorithm rather than a O(n) one. 
    // (n == GP_REGULAR_REG_NUM) This may not be faster than the O(n) one 
    // in practice, but it's definitely cooler. :-)    -Jie
    unsigned interimReg = baseReg == SP ? R0 : (baseRegNum  + 1) % GP_REGULAR_REG_NUM + R0;
    
    // Second, push the selected register onto the stack.
    BuildMI(MBB, MI, DL, TII->get(ARM::tPUSH))
      .addImm(ARMCC::AL).addReg(0)  // pred:14, pred:%noreg
      .addReg(interimReg);

    // The push increased SP by 4 bytes; so imm needs to be updated.
    if (baseReg == ARM::SP) imm += 4;

    // Third, move from FP register to the general-purpose register.
    BuildMI(MBB, MI, DL, TII->get(ARM::VMOVRS), interimReg)
      .addReg(interimReg)
      .addReg(sourceReg);

    // Forth, create an unprivileged store.
    convertSTRimm(MBB, MI, interimReg, baseReg, imm, newOpcode, DL, TII);

    // Finally, restore the general-purpose register.
    BuildMI(MBB, MI, DL, TII->get(ARM::tPOP))
      .addImm(ARMCC::AL).addReg(0)
      .addReg(interimReg);
  } else {
    // store a double-precision register (two single-precision registers)
    
    // First, pick two general-purpose reigsters. 
    unsigned interimReg0, interimReg1;
    if (baseReg == SP) {
      interimReg0 = R0, interimReg1 = R1;
    } else {
      interimReg0 = (baseRegNum + 1) % GP_REGULAR_REG_NUM + R0;
      interimReg1 = (baseRegNum + 2) % GP_REGULAR_REG_NUM + R0;
      if (interimReg1 < interimReg0) {
        // The Code Generator only allows to push/pop registers in an 
        // increasingly sorted order.
        unsigned tmp = interimReg0;
        interimReg0 = interimReg1, interimReg1 = tmp;
      }
    }

    // Second, push the two selected registers onto the stack.
    BuildMI(MBB, MI, DL, TII->get(ARM::tPUSH))
      .addImm(ARMCC::AL).addReg(0)  // pred:14, pred:%noreg
      .addReg(interimReg0).addReg(interimReg1);
  
    // The push increased SP by 8 bytes.
    if (baseReg == ARM::SP) imm += 8;

    // Third, move the double word to the two general-purpose registers.
    BuildMI(MBB, MI, DL, TII->get(ARM::VMOVRRD))
      .addReg(interimReg0).addReg(interimReg1)
      .addReg(sourceReg)
      .addImm(ARMCC::AL).addReg(0);  // pred:14, pred:%noreg

    // Forth, build unprivileged stores.
    convertSTRimm(MBB, MI, interimReg0, baseReg, imm, newOpcode, DL, TII);
    convertSTRimm(MBB, MI, interimReg1, baseReg, imm + 4, newOpcode, DL, TII);

    // Last, restore r0, r1.
    BuildMI(MBB, MI, DL, TII->get(ARM::tPOP))
      .addImm(ARMCC::AL).addReg(0)  // pred:14, pred:%noreg
      .addReg(interimReg0).addReg(interimReg1);
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
      buildUnprivStr(MBB, MI, regList[i], baseReg, i * 4, newOpcode, DL, TII);
    }

    // If this is a write-back store, update the baseReg.
    if (opcode != ARM::t2STMIA) {
      if (baseReg == ARM::SP) {
        BuildMI(MBB, MI, DL, TII->get(ARM::tADDspi), baseReg)
          .addReg(baseReg)
          .addImm(numOfReg);
      } else {
        BuildMI(MBB, MI, DL, TII->get(ARM::tADDi8), baseReg)
          .addDef(ARM::CPSR, RegState::Dead)
          .addUse(baseReg)
          .addImm(numOfReg << 2);
      }
    }
  } else {
    // STMDB: Store Multiple Decrement Before
    // First, sub the base register to be the starting address of store.
    if (baseReg == ARM::SP) {
      BuildMI(MBB, MI, DL, TII->get(ARM::tSUBspi), baseReg)
        .addReg(baseReg)
        .addImm(numOfReg);
    } else {
      BuildMI(MBB, MI, DL, TII->get(ARM::tSUBi8), baseReg)
        .addDef(ARM::CPSR, RegState::Dead)
        .addUse(baseReg)
        .addImm(numOfReg << 2);
    }

    // Store all the registers.
    for (unsigned i = 0; i < numOfReg; i++) {
      buildUnprivStr(MBB, MI, regList[i], baseReg, i * 4, newOpcode, DL, TII);
    }

    // If this is not a write-back store, we need restore base register.
    if (opcode == ARM::t2STMDB) {
      if (baseReg == ARM::SP) {
        BuildMI(MBB, MI, DL, TII->get(ARM::tADDspi), baseReg)
          .addReg(baseReg)
          .addImm(numOfReg);
      } else {
        BuildMI(MBB, MI, DL, TII->get(ARM::tADDi8), baseReg)
          .addDef(ARM::CPSR, RegState::Dead)
          .addUse(baseReg)
          .addImm(numOfReg << 2);
      }
    }
  }
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
// Function convertVPUSH()
//
// Description:
//   This function converts a VPUSH to multiple STRT.
//
// Inputs:
//   MBB - The MachineBasicBlock in which to insert the new unprivileged store.
//   MI - The MachineInstr before which to insert the the new unprivileged store.
//   baseReg - The register used as the base register to get the memory address.
//   isSinglePrecision - Indicate if it is a single-precision or double-precision store.
//   DL - A reference to the DebugLoc structure.
//   TII - A pointer to the TargetInstrInfo structure.
//
// Outputs:
//   Multiple STRT and a sub to update the SP.
//
static void convertVPUSH(MachineBasicBlock &MBB, MachineInstr *MI,
                 bool isSinglePrecision,
                 DebugLoc &DL, const TargetInstrInfo *TII) {

  unsigned SP = ARM::SP;
  // Get the register list.
  unsigned numOfReg = MI->getNumOperands() - 4;
  std::vector<unsigned> regList;
  for (unsigned i = 0; i < numOfReg; i++) {
    regList.push_back(MI->getOperand(i + 4).getReg());
  }
  
  // Store multiple floating-point registers.
  if (isSinglePrecision) {
    // Update SP. 
    // A VPUSH stores registersfrom the lower address to higher address.
    BuildMI(MBB, MI, DL, TII->get(ARM::tSUBspi), SP)
      .addReg(SP)
      .addImm(numOfReg);

    // Store all registers.
    for (unsigned i = 0; i < numOfReg; i++) {
      convertVSTR(MBB, MI, regList[i], SP, i * 4, true, DL, TII);
    }
  } else {
    BuildMI(MBB, MI, DL, TII->get(ARM::tSUBspi), SP)
      .addReg(SP)
      .addImm(numOfReg * 2);
    for (unsigned i = 0; i < numOfReg; i++) {
      convertVSTR(MBB, MI, regList[i], SP, i * 8, false, DL, TII);
    }
  }

  // TO-DO: there is an optimization we can do here. 
  // When storing multiple FP registers to consecutive memory addresses, for 
  // each store, we push one or two interim registers onto the stack, use them, 
  // and then pop back. Thus, there'd be unnecessary <pop, push> pairs generatd.
  // We can remove these <pop, push> pairs to save both space and time.
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

  // for debugging
  errs() << "Silhouette: hello from function: " << funcName << "\n";

  const TargetInstrInfo *TII = MF.getSubtarget<ARMSubtarget>().getInstrInfo();
  DebugLoc DL;

  // iterate over all MachineInstr
  for (MachineBasicBlock &MBB : MF) {
    std::vector<MachineInstr *> originalStores; // Need delete the original stores.
    for (MachineInstr &MI : MBB) {
      unsigned opcode = MI.getOpcode();
      unsigned sourceReg = 0;
      unsigned sourceReg2 = 0; // for STRD
      unsigned baseReg = 0;
      unsigned offsetReg = 0;  // for STR(register) 
      int64_t imm = 0;
      unsigned newOpcode = getNewOpcode(opcode);

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

        // indexed stores
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
          convertSTRimmIndexed(MBB, &MI, sourceReg, baseReg, imm, 
              opcode == ARM::t2STR_PRE, newOpcode, DL, TII);
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
          // TO-DO
          break;

        // Floating stores.
        case ARM::VSTRS:
        case ARM::VSTRD:
#if 1
          sourceReg = MI.getOperand(0).getReg();
          baseReg = MI.getOperand(1).getReg();
          imm = (MI.getOperand(2).getImm()) << 2;
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
          convertPUSH(MBB, &MI, DL, TII);
          originalStores.push_back(&MI);
          break;

        // Store multiple FP registers.
        // According to ARMInstrVFP.td, it looks like that LLVM 4.0 didn't
        // generate VSTM. VSTMDDB_UPD and VSTMSDB_UPD are aliases for vpush. 
        case ARM::VSTMDDB_UPD:  // VPUSH double-precision registers
        case ARM::VSTMSDB_UPD:  // VPUSH single-precision registers
#if 1
          convertVPUSH(MBB, &MI, opcode == ARM::VSTMSDB_UPD, DL, TII);
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
