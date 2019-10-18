//===- ARMSilhouetteInstrumentor - A helper class for instrumentation -----===//
//
//         Protecting Control Flow of Real-time OS applications
//
// This file was written by at the University of Rochester.
// All Rights Reserved.
//
//===----------------------------------------------------------------------===//
//
// This class implements methods that can help passes of its subclass easily
// instrument machine IR without concerns of breaking IT blocks.
//
//===----------------------------------------------------------------------===//
//

#include "ARM.h"
#include "ARMBaseInstrInfo.h"
#include "ARMSilhouetteInstrumentor.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/Support/raw_ostream.h"

#include <deque>

using namespace llvm;

static DebugLoc DL;

//
// Method: getITBlockSize()
//
// Description:
//   This method computes how many predicated instructions an IT instruction
//   covers.
//
// Input:
//   IT - A reference to an IT instruction.
//
// Return value:
//   The number of predicated instructions IT covers.
//
unsigned
ARMSilhouetteInstrumentor::getITBlockSize(const MachineInstr & IT) {
  assert(IT.getOpcode() == ARM::t2IT && "Not an IT instruction!");

  unsigned Mask = IT.getOperand(1).getImm() & 0xf;
  assert(Mask != 0 && "Invalid IT mask!");

  if (Mask & 0x1) {
    return 4;
  } else if (Mask & 0x2) {
    return 3;
  } else if (Mask & 0x4) {
    return 2;
  } else {
    return 1;
  }
}

//
// Method: findIT()
//
// Description:
//   This method finds the IT instruction that forms an IT block containing a
//   given instruction MI.  It also computes the distance (from 1 to 4) between
//   the IT and MI.  If there is no such IT, a null pointer is returned.
//
// Inputs:
//   MI       - A reference to an instruction from which to find IT.
//   distance - A reference to an unsigned to store the distance.
//
// Return value:
//   A pointer to IT if found, nullptr otherwise.
//
MachineInstr *
ARMSilhouetteInstrumentor::findIT(MachineInstr & MI, unsigned & distance) {
  assert(MI.getOpcode() != ARM::t2IT && "MI cannot be IT!");

  MachineInstr * Prev = &MI;
  unsigned dist = 0;
  while (Prev != nullptr && dist < 5 && Prev->getOpcode() != ARM::t2IT) {
    Prev = Prev->getPrevNode();
    ++dist;
  }
  if (Prev != nullptr && dist < 5 && Prev->getOpcode() == ARM::t2IT) {
    if (getITBlockSize(*Prev) >= dist) {
      distance = dist;
      return Prev;
    }
  }
  return nullptr;
}

//
// Method: findIT()
//
// Description:
//   This method finds the IT instruction that forms an IT block containing a
//   given instruction MI.  It also computes the distance (from 1 to 4) between
//   the IT and MI.  If there is no such IT, a null pointer is returned.
//
// Inputs:
//   MI       - A const reference to an instruction from which to find IT.
//   distance - A reference to an unsigned to store the distance.
//
// Return value:
//   A const pointer to IT if found, nullptr otherwise.
//
const MachineInstr *
ARMSilhouetteInstrumentor::findIT(const MachineInstr & MI, unsigned & distance) {
  return findIT(const_cast<MachineInstr &>(MI), distance);
}

//
// Method: insertInstBefore()
//
// Description:
//   This method inserts an instruction Inst before a given instruction MI.  If
//   MI is a predicated instruction within an IT block, then Inst will have the
//   same predicate as MI and also end up in an IT block.  Note that MI cannot
//   be an IT instruction itself.
//
// Inputs:
//   MI   - A reference to an instruction before which to insert Inst.
//   Inst - A pointer to an instruction to insert.
//
void
ARMSilhouetteInstrumentor::insertInstBefore(MachineInstr & MI,
                                            MachineInstr * Inst) {
  std::deque<MachineInstr *> Insts { Inst };
  insertInstsBefore(MI, Insts);
}

//
// Method: insertInstAfter()
//
// Description:
//   This method inserts an instruction Inst after a given instruction MI.  If
//   MI is a predicated instruction within an IT block, then Inst will have the
//   same predicate as MI and also end up in an IT block.  Note that MI cannot
//   be an IT instruction itself.
//
// Inputs:
//   MI   - A reference to an instruction after which to insert Inst.
//   Inst - A pointer to an instruction to insert.
//
void
ARMSilhouetteInstrumentor::insertInstAfter(MachineInstr & MI,
                                           MachineInstr * Inst) {
  std::deque<MachineInstr *> Insts { Inst };
  insertInstsAfter(MI, Insts);
}

//
// Method: insertInstsBefore()
//
// Description:
//   This method inserts a group of instructions contained in a deque before a
//   given instruction MI.  If MI is a predicated instruction within an IT
//   block, then the new instructions will have the same predicate as MI and
//   also end up in one or more IT blocks.  Note that MI cannot be an IT
//   instruction itself.
//
// Inputs:
//   MI    - A reference to an instruction before which to insert instructions.
//   Insts - A reference to a deque containing the instructions.
//
void
ARMSilhouetteInstrumentor::insertInstsBefore(MachineInstr & MI,
                                             std::deque<MachineInstr *> & Insts) {
  MachineFunction & MF = *MI.getMF();
  MachineBasicBlock & MBB = *MI.getParent();
  const TargetInstrInfo * TII = MF.getSubtarget().getInstrInfo();

  unsigned distance;
  MachineInstr * IT = findIT(MI, distance);

  // Do insert new instructions before MI
  for (MachineInstr * Inst : Insts) {
    MBB.insert(MI, Inst);
  }

  // If MI is inside an IT block, we should make sure to cover all new
  // instructions with IT(s)
  if (IT != nullptr) {
    unsigned ITBlockSize = getITBlockSize(*IT);
    unsigned Mask = IT->getOperand(1).getImm() & 0xf;
    ARMCC::CondCodes firstCond = (ARMCC::CondCodes)IT->getOperand(0).getImm();
    std::deque<bool> DQMask = decodeITMask(Mask);
    bool sameAsFirstCond = DQMask[distance - 1];

    // Find the range of instructions that are supposed to be in IT block(s)
    MachineBasicBlock::iterator firstMI(IT->getNextNode()); // Inclusive
    MachineBasicBlock::iterator lastMI(MI);                 // Non-inclusive
    for (unsigned i = distance; i <= ITBlockSize; ++i) {
      ++lastMI;
    }

    // Track new instructions in DQMask
    auto it = DQMask.begin();
    for (unsigned i = 0; i < distance - 1; ++i) {
      it++;
    }
    DQMask.insert(it, Insts.size(), sameAsFirstCond);

    // Insert ITs to cover instructions from [firstMI, lastMI)
    for (MachineBasicBlock::iterator i(firstMI); i != lastMI; ) {
      std::deque<bool> NewDQMask;
      MachineBasicBlock::iterator j(i);
      for (unsigned k = 0; k < 4 && j != lastMI; ++j, ++k) {
        NewDQMask.push_back(DQMask.front());
        DQMask.pop_front();
      }
      bool flip = false;
      if (!NewDQMask[0]) {
        for (unsigned k = 0; k < NewDQMask.size(); ++k) {
          NewDQMask[k] = !NewDQMask[k];
        }
        flip = true;
      }
      BuildMI(MBB, i, DL, TII->get(ARM::t2IT))
      .addImm(flip ? ARMCC::getOppositeCondition(firstCond) : firstCond)
      .addImm(encodeITMask(NewDQMask));
      i = j; // Update i here
    }

    // Remove the original IT
    IT->eraseFromParent();
  }
}

//
// Method: insertInstsAfter()
//
// Description:
//   This method inserts a group of instructions contained in a deque after a
//   given instruction MI.  If MI is a predicated instruction within an IT
//   block, then the new instructions will have the same predicate as MI and
//   also end up in one or more IT blocks.  Note that MI cannot be an IT
//   instruction itself.
//
// Inputs:
//   MI    - A reference to an instruction after which to insert instructions.
//   Insts - A reference to a deque containing the instructions.
//
void
ARMSilhouetteInstrumentor::insertInstsAfter(MachineInstr & MI,
                                            std::deque<MachineInstr *> & Insts) {
  MachineFunction & MF = *MI.getMF();
  MachineBasicBlock & MBB = *MI.getParent();
  const TargetInstrInfo * TII = MF.getSubtarget().getInstrInfo();
  MachineBasicBlock::iterator NextMI(MI); ++NextMI;

  unsigned distance;
  MachineInstr * IT = findIT(MI, distance);

  // Do insert new instructions after MI
  for (MachineInstr * Inst : Insts) {
    MBB.insert(NextMI, Inst);
  }

  // If MI is inside an IT block, we should make sure to cover all new
  // instructions with IT(s)
  if (IT != nullptr) {
    unsigned ITBlockSize = getITBlockSize(*IT);
    unsigned Mask = IT->getOperand(1).getImm() & 0xf;
    ARMCC::CondCodes firstCond = (ARMCC::CondCodes)IT->getOperand(0).getImm();
    std::deque<bool> DQMask = decodeITMask(Mask);
    bool sameAsFirstCond = DQMask[distance - 1];

    // Find the range of instructions that are supposed to be in IT block(s)
    MachineBasicBlock::iterator firstMI(IT->getNextNode()); // Inclusive
    MachineBasicBlock::iterator lastMI(Insts.back());       // Non-inclusive
    for (unsigned i = distance; i <= ITBlockSize; ++i) {
      ++lastMI;
    }

    // Track new instructions in DQMask
    auto it = DQMask.begin();
    for (unsigned i = 0; i <= distance - 1; ++i) {
      it++;
    }
    DQMask.insert(it, Insts.size(), sameAsFirstCond);

    // Insert ITs to cover instructions from [firstMI, lastMI)
    for (MachineBasicBlock::iterator i(firstMI); i != lastMI; ) {
      std::deque<bool> NewDQMask;
      MachineBasicBlock::iterator j(i);
      for (unsigned k = 0; k < 4 && j != lastMI; ++j, ++k) {
        NewDQMask.push_back(DQMask.front());
        DQMask.pop_front();
      }
      bool flip = false;
      if (!NewDQMask[0]) {
        for (unsigned k = 0; k < NewDQMask.size(); ++k) {
          NewDQMask[k] = !NewDQMask[k];
        }
        flip = true;
      }
      BuildMI(MBB, i, DL, TII->get(ARM::t2IT))
      .addImm(flip ? ARMCC::getOppositeCondition(firstCond) : firstCond)
      .addImm(encodeITMask(NewDQMask));
      i = j; // Update i here
    }

    // Remove the original IT
    IT->eraseFromParent();
  }
}

//
// Method: removeInst()
//
// Description:
//   This method removes a given instruction MI from machine IR.  If MI is a
//   predicated instruction within an IT block, then its corresponding IT
//   instruction will be updated or removed as well.  Note that MI cannot be an
//   IT instruction itself.
//
// Input:
//   MI - A reference to the instruction to remove.
//
void
ARMSilhouetteInstrumentor::removeInst(MachineInstr & MI) {
  unsigned distance;
  MachineInstr * IT = findIT(MI, distance);

  // If MI was inside an IT block, we should make sure to update/remove the IT
  // instruction
  if (IT != nullptr) {
    unsigned Mask = IT->getOperand(1).getImm() & 0xf;
    ARMCC::CondCodes firstCond = (ARMCC::CondCodes)IT->getOperand(0).getImm();
    std::deque<bool> DQMask = decodeITMask(Mask);

    // Remove MI's entry from DQMask
    auto it = DQMask.begin();
    for (unsigned i = 0; i < distance - 1; ++i) {
      it++;
    }
    DQMask.erase(it);

    // Remove IT as well if MI was the only instruction in the IT block
    if (DQMask.empty()) {
      IT->eraseFromParent();
    } else {
      // If MI was the first instruction in the IT block, removing MI might
      // change the first condition, in which case we need to flip it
      if (!DQMask[0]) {
        for (unsigned i = 0; i < DQMask.size(); ++i) {
          DQMask[i] = !DQMask[i];
        }
        IT->getOperand(0).setImm(ARMCC::getOppositeCondition(firstCond));
      }
      // Update the IT mask
      IT->getOperand(1).setImm(encodeITMask(DQMask));
    }
  }

  // Now do remove MI
  MI.eraseFromParent();
}

//
// Method: decodeITMask()
//
// Description:
//   This method decodes an IT mask in LLVM's representation and puts a list of
//   boolean values in a deque to return.  The boolean values represent whether
//   their corresponding instructions in an IT block have the same predicate as
//   the first one (which indicates that the first boolean value is always
//   true).
//
// Input:
//   Mask - The IT mask in LLVM's representation (immediate value of the second
//          operand of a t2IT instruction).
//
// Return value:
//   A deque of boolean values (see the above description).
//
std::deque<bool>
ARMSilhouetteInstrumentor::decodeITMask(unsigned Mask) {
  Mask &= 0xf;
  assert(Mask != 0 && "Invalid IT mask!");

  std::deque<bool> DQMask { true };
  unsigned size = 4;
  for (unsigned i = 0x1; i < 0x10; i <<= 1) {
    if (Mask & i) {
      break;
    }
    --size;
  }
  for (unsigned i = 3; i > 4 - size; --i) {
    DQMask.push_back((Mask & (1 << i)) == 0);
  }

  return DQMask;
}

//
// Method: encodeITMask()
//
// Description:
//   This method takes an IT mask in the form of a list of boolean values and
//   encodes it into LLVM's representation.  The boolean values represent
//   whether their corresponding instructions in an IT block have the same
//   predicate as the first one (which requires that the first boolean value
//   be always true).
//
// Input:
//   DQMask - An IT mask in the form of a list of boolean values.
//
// Return value:
//   The IT mask in LLVM's representation (immediate value of the second
//   operand of a t2IT instruction).
//
unsigned
ARMSilhouetteInstrumentor::encodeITMask(std::deque<bool> DQMask) {
  assert(!DQMask.empty() && "Invalid deque representation of an IT mask!");
  assert(DQMask.size() <= 4 && "Invalid deque representation of an IT mask!");
  assert(DQMask[0] && "Invalid deque representation of an IT mask!");

  unsigned Mask = 0;
  for (unsigned i = 1; i < DQMask.size(); ++i) {
    Mask |= DQMask[i] ? 0 : 1;
    Mask <<= 1;
  }
  Mask |= 1;
  Mask <<= (4 - DQMask.size());

  return Mask;
}
