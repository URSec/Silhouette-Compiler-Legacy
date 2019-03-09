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


//
// Method: buildUnprivStore()
//
// Description:
//   This method builds an unprivileged store for a normal store. 
//   Currently it only handles STR -> STRT. We need support all the other stores
//   by expanding this method or adding new method(s).
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
void buildUnprivStore(MachineBasicBlock &MBB,
                     MachineInstr *MI,
                     unsigned sourceReg, unsigned baseReg, int64_t imm,
                     unsigned newInstrOpcode,
                     DebugLoc &DL,
                     const TargetInstrInfo *TII) {
  // insert a new unprivileged store
  BuildMI(MBB, MI, DL, TII->get(newInstrOpcode))
    .addReg(sourceReg)
    .addReg(baseReg)
    .addImm(imm);
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
  MI.dump();

  unsigned numOperands = MI.getNumExplicitOperands();
  errs() << "Number of operands: " << numOperands << "\n";
  for (unsigned i = 0; i < numOperands; i++) {
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
#if 0
  StringRef funcName = MF.getName();
  if (funcWhitelist.find(funcName) == funcWhitelist.end()) return false;
  errs() << "Silhouette: hello from function: " << funcName << "\n";
#endif


  const TargetInstrInfo *TII = MF.getSubtarget<ARMSubtarget>().getInstrInfo();
  DebugLoc DL;

  // iterate over all MachineInstr
  for (MachineBasicBlock &MBB : MF) {
    std::vector<MachineInstr *> originalStores;  // we need delete the original stores
    for (MachineInstr &MI : MBB) {
      unsigned opcode = MI.getOpcode();
      unsigned sourceReg = 0;
      unsigned baseReg = 0;
      int64_t imm = 0;

      switch (opcode) {
        // stores immediate
        case ARM::tSTRi:    // Encoding T1: STR<c> <Rt>, [<Rn>{,#<imm5>}]
        case ARM::tSTRspi:  // Encoding T2: STR<c> <Rt>, [SP, #<imm8>]
        /* case ARM::t2STRi12: // Encoding T3: STR<c>.W <Rt>,[<Rn>,#<imm12>] */
#if 1
          // It's a STR instruction.
          sourceReg = MI.getOperand(0).getReg();
          baseReg = MI.getOperand(1).getReg();
          imm = MI.getOperand(2).getImm();
          // The imm of a STR(immediate) = ZeroExtend(imm: "00", 32); but the 
          // imm of the STRT instruction is not ZeroExtended. 
          imm <<= 2; 
          buildUnprivStore(MBB, &MI, sourceReg, baseReg, imm, ARM::t2STRT, DL, TII);
          
          originalStores.push_back(&MI);
#endif
          /* printOperands(MI); */
          MI.dump();
          break;


        // indexed stores
        case ARM::t2STR_PRE: // pre-index store
        case ARM::t2STR_POST: // post-index store
          /* printOperands(MI); */
          break;
        
        // STR(register); A7.7.159
        case ARM::tSTRr:   
          break;
        
        default:
          if (MI.mayStore()) {
#if 1
              errs() << "Silhouette: other stores; dump: ";
              MI.dump();
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
