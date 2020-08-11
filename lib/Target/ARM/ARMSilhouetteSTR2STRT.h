//===-- ARMSilhouetteSTR2STRT - Store to Unprivileged Store convertion-----===//
//
//         Protecting Control Flow of Real-time OS applications
//              Copyright (c) 2019-2020, University of Rochester
//
// Part of the Silhouette Project, under the Apache License v2.0 with
// LLVM Exceptions.
// See LICENSE.txt in the top-level directory for license information.
//
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass converts all regular store instructions to the unprivileged store
// instructions.
//
//===----------------------------------------------------------------------===//
//

#ifndef ARM_SILHOUETTE_STR2STRT
#define ARM_SILHOUETTE_STR2STRT

#include "ARMSilhouetteInstrumentor.h"
#include "llvm/CodeGen/MachineFunctionPass.h"

namespace llvm {

  struct ARMSilhouetteSTR2STRT
      : public MachineFunctionPass, ARMSilhouetteInstrumentor {
    // pass identifier variable
    static char ID;

    ARMSilhouetteSTR2STRT();

    virtual StringRef getPassName() const override;

    virtual bool runOnMachineFunction(MachineFunction & MF) override;
  };

  FunctionPass *createARMSilhouetteSTR2STRT(void);
}

#endif
