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

#include "ARMSilhouetteSTR2STRT.h"
#include "ARM.h"
#include "ARMTargetMachine.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

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
    case ARM::tSTRi:
    case ARM::tSTRspi:
    case ARM::t2STRi12:
    case ARM::t2STR_PRE:
    case ARM::t2STR_POST:
    case ARM::tSTRr:
    case ARM::t2STRs:  
      return ARM::t2STRT;

    case ARM::tSTRBi:
      return ARM::t2STRBT;
  }

  // unknown opcode
  return -1;
}


//
// Method: buildSTRT
//
// Description:
//   This method builds a unprivileged store instructions. It serves as a
//   space-saver - this pass builds unprivileged stores a lot.
//
// Inputs:
//   MBB - The MachineBasicBlock in which to insert the new unprivileged store.
//   MI - The MachineInstr before which to insert the the new unprivileged store.
//   sourceReg - The register whose contents will be stored to some memory address.
//   baseReg - The register used as the base register to compute the memory address.
//   imm - The immediate that is added to the baseReg to compute the memory address.
//   newInstrOpcode - The opcode of the unprivileged store.
//   DL - A reference to the DebugLoc structure.
//   TII - A pointer to the TargetInstrInfo structure.
//
// Outputs:
//   A new unprivileged store.
//
// Return:
//   None.

static void buildSTRT(MachineBasicBlock &MBB,
                      MachineInstr *MI,
                      unsigned sourceReg, unsigned baseReg, uint8_t imm,
                      unsigned newInstrOpcode,
                      DebugLoc &DL,
                      const TargetInstrInfo *TII) {
  BuildMI(MBB, MI, DL, TII->get(newInstrOpcode))
    .addReg(sourceReg)
    .addReg(baseReg)
    .addImm(imm);
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
//   newInstrOpcode - The opcode of the unprivileged store.
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
                     unsigned newInstrOpcode,
                     DebugLoc &DL,
                     const TargetInstrInfo *TII) {
  
  // TO-DO: baseReg could be SP, which should be handled individually.
  
  // Unprivileged stores only support positive imm. If imm is a negative, then 
  // we need to sub this imm to the base register, give 0 to the imm field of 
  // the new str, and restore the base registr.
  if (imm < 0) {
    printOperands(*MI);
    BuildMI(MBB, MI, DL, TII->get(ARM::tSUBi8), baseReg)
      .addReg(baseReg)
      .addReg(baseReg)
      .addImm(-imm);
    
    // insert a new unprivileged store
    buildSTRT(MBB, MI, sourceReg, baseReg, 0, newInstrOpcode, DL, TII);

    // restore the base register
    BuildMI(MBB, MI, DL, TII->get(ARM::tADDi8), baseReg)
      .addReg(baseReg)
      .addReg(baseReg)
      .addImm(-imm);
  } else {
    buildSTRT(MBB, MI, sourceReg, baseReg, imm, newInstrOpcode, DL, TII);
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
//   newInstrOpcode - The opcode of the unprivileged store.
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
                          unsigned newInstrOpcode,
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
    buildSTRT(MBB, MI, sourceReg, baseReg, 0, newInstrOpcode, DL, TII);
  } else {
    // This is a post-indexed store.
    // First, build a new store.
    buildSTRT(MBB, MI, sourceReg, baseReg, 0, newInstrOpcode, DL, TII);

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
//   newInstrOpcode - The opcode of the unprivileged store.
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
                   unsigned newInstrOpcode,
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
    buildSTRT(MBB, MI, sourceReg, baseReg, 0, newInstrOpcode, DL, TII);

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
    buildSTRT(MBB, MI, sourceReg, baseReg, 0, newInstrOpcode, DL, TII);

    // Restore the base register.
    BuildMI(MBB, MI, DL, TII->get(ARM::t2SUBrs), baseReg)
      .addReg(baseReg).addReg(offsetReg)
      .addImm(ARM_AM::getSORegOpc(ARM_AM::lsl, imm)) 
      .addImm(ARMCC::AL).addReg(0)  // pred:14, pred:%noreg
      .addReg(0);  // opt:%noreg
  }
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
  errs() << "tSUBi3 = " << ARM::tSUBi3 << "\n";
  errs() << "tSTRspi = " << ARM::tSTRspi << "\n";
  errs() << "t2STR_PRE = " << ARM::t2STR_PRE << "\n";
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
  
#if 1
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
      unsigned baseReg = 0;
      unsigned offsetReg = 0; // for STR(register) 
      int64_t imm = 0;
      unsigned newOpcode = getNewOpcode(opcode);
      
      switch (opcode) {
#if 1
        // store immediate; A7.7.158 STR(immediate)
        case ARM::tSTRi:    // Encoding T1: STR<c> <Rt>, [<Rn>{,#<imm5>}]
        case ARM::tSTRspi:  // Encoding T2: STR<c> <Rt>, [SP, #<imm8>]
        case ARM::t2STRi12: // Encoding T3: STR<c>.W <Rt>,[<Rn>,#<imm12>]
#endif 

#if 1
        // store byte immediate; A7.7.160 STRB(immediate)
        case ARM::tSTRBi:   // 
          if (newOpcode == ARM::t2STRBT)
            printOperands(MI);
          sourceReg = MI.getOperand(0).getReg();
          baseReg = MI.getOperand(1).getReg();
          imm = MI.getOperand(2).getImm();
          if (opcode == ARM::tSTRi || opcode == ARM::tSTRspi) {
            // The imm of a these two stores = ZeroExtend(imm: "00", 32); 
            // but the  imm of the STRT instruction is not ZeroExtended. 
            imm <<= 2; 
          }

          convertSTRimm(MBB, &MI, sourceReg, baseReg, imm, newOpcode, DL, TII);
          originalStores.push_back(&MI);
#endif
          break;

        // indexed stores
        case ARM::t2STR_PRE: // pre-index store
        case ARM::t2STR_POST: // post-index store
#if 0
          sourceReg = MI.getOperand(1).getReg();
          baseReg = MI.getOperand(0).getReg(); // the reg to be updated
          imm = MI.getOperand(3).getImm();
          convertSTRimmIndexed(MBB, &MI, sourceReg, baseReg, imm, opcode == ARM::t2STR_PRE,
              ARM::t2STRT, DL, TII);
          originalStores.push_back(&MI);
#endif
          break;
        
        // STR(register); A7.7.159 
        case ARM::tSTRr: // Encoding T1: STR<c> <Rt>,[<Rn>,<Rm>]
        case ARM::t2STRs:  // Encoding T2: STR<c>.W <Rt>,[<Rn>,<Rm>{,LSL #<imm2>}]
#if 1
          printOperands(MI);
          sourceReg = MI.getOperand(0).getReg();
          baseReg = MI.getOperand(1).getReg();
          offsetReg = MI.getOperand(2).getReg();
          convertSTRReg(MBB, &MI, sourceReg, baseReg, offsetReg, newOpcode, DL, TII);
          originalStores.push_back(&MI);
#endif
          break;

        default:
          if (MI.mayStore()) {
#if 0
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
