//===-- ARMSilhouetteSTR2STRT - Store to Unprivileged Store convertion-----===//
//
//         Protecting Control Flow of Real-time OS applications
//
// This file was written by at the University of Rochester.
// All Rights Reserved.
//
//===----------------------------------------------------------------------===//
//
// This pass converts all normal STR instructions to the STRT family 
// instructions.
//
//===----------------------------------------------------------------------===//
//

#ifndef ARM_SILHOUETTE_STR2STRT
#define ARM_SILHOUETTE_STR2STRT

#include "llvm/CodeGen/MachineFunctionPass.h"
#include <set>

// A whitelist of functions on which this pass will run. 
// This is a helper for development. 
// Will take it out after the development is done.
const static std::set<std::string> funcWhitelist = {
#if 0
  "main",
  "HAL_IncTick",
  "HAL_GPIO_Init",
  "HAL_RCC_GetSysClockFreq",
  "HAL_TIM_IRQHandler",
  "HAL_TIM_IC_CaptureCallback",
  "HAL_TIM_OC_DelayElapsedCallback",
  "HAL_TIM_PWM_PulseFinishedCallback",
  "HAL_TIM_TriggerCallback",
  "HAL_TIMEx_CommutationCallback",
  "HAL_TIMEx_BreakCallback",
  "HAL_TIMEx_Break2Callback",
  "HAL_TIM_PeriodElapsedCallback"
  "HAL_UART_Init",
  "HAL_UART_MspInit",
  "UART_SetConfig",
  "UART_AdvFeatureConfig",
  "UART_CheckIdleState",
  "HAL_UART_Transmit",
  "vListInsertEnd",
  "UART_WaitOnFlagUntilTimeout",
  "uxListRemove",
  "prvResetNextTaskUnblockTime",
  "vTaskSwitchContext",
  "xTaskIncrementTick",
  "xTaskGetSchedulerState",
  "PendSV_Handler",
  "xPortSysTickHandler",
  "BSP_COM_Init",
  "vMainUARTPrintString",
  "Console_UART_Init",
  "CopyDataInit", // this one only has a STR(register)
  "LoopCopyDataInit", // no store
  "FillZerobss",  // str.w,
  "__register_exitproc"
#endif
};

// A blacklist of functions on which this pass will ignore. 
// This is a helper for development. 
// Will take it out after the development is done.
const static std::set<std::string> funcBlacklist {
  // JZ: This function (instrumented) causes the program to enter an infinite loop.
  // I guesss the reason is that some store(s) in this function are supposed to 
  // be privileged stores.
  "SystemInit",  
};

namespace llvm {
  struct ARMSilhouetteSTR2STRT : public MachineFunctionPass {
    // pass identifier variable
    static char ID;

    ARMSilhouetteSTR2STRT();

    virtual StringRef getPassName() const override;

    virtual bool runOnMachineFunction(MachineFunction &MF) override;
  };

  FunctionPass *createARMSilhouetteSTR2STRT(void);
}

#endif
