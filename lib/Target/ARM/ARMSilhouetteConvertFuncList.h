//===-- ARMSilhouetteSTR2STRT - Store to Unprivileged Store convertion-----===//
//
//         Protecting Control Flow of Real-time OS applications
//
// This file was written by at the University of Rochester.
// All Rights Reserved.
//
//===----------------------------------------------------------------------===//
// This file contains a list of functions on which our pass runs, and 
// a list of functions on which our pass does not run. It servers two purposes.
// One is to help debugging. The other is to really ignore certain functions:
// some function are supposed to be running in privileged mode. We need to find 
// out which those functions are.
// 
//===----------------------------------------------------------------------===//
//

#include <set>

// A whitelist of functions on which this pass will run. This is a helper for
// development. Will take it out after the development is done.
const static std::set<std::string> funcWhitelist = {
  // main and HAL lib functions without using timer
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

  // BEEBS benchmark
#if 1
  "main",
  "initialise_benchmark",
  "benchmark",
  "verify_benchmark",
  // aha-compress
  "compress3",
  "compress4",
  // cnt
  "Test",
  "Initialize",
  "InitSeed",
  "Sum",
  "RandomInteger",

  // dtoi
  "strtod",
  // dijkstra
  "enqueue",
  "dequeue",
  "qcount",
  "dijkstra",
#endif
};

// A blacklist of functions on which this pass will ignore. 
const static std::set<std::string> funcBlacklist {
  // This function (instrumented) causes the program to enter an infinite loop.
  // I guesss the reason is that some store(s) in this function are supposed to 
  // be privileged stores.
  "SystemInit",  

  // timer related functions
  // Instrumenting them would also break programs.
  "HAL_Init",
    "HAL_NVIC_SetPriorityGrouping",  // called by HAL_Init
    "NVIC_SetPriorityGrouping",      // called by HAL_NVIC_SetPriorityGrouping
    "HAL_InitTick",                  // called by HAL_Init
      "HAL_NVIC_SetPriority",        // called by HAL_InitTick
        "NVIC_GetPriorityGrouping",  // called by HAL_NVIC_SetPriority
        "NVIC_EncodePriority",       // called by HAL_NVIC_SetPriority
        "NVIC_SetPriority",          // called by HAL_NVIC_SetPriority
      "HAL_NVIC_EnableIRQ",          // called by HAL_InitTick
        "NVIC_EnableIRQ",            // called by HAL_NVIC_EnableIRQ
      "HAL_RCC_GetClockConfig",      // called by HAL_InitTick
      "HAL_RCC_GetPCLK1Freq",        // called by HAL_InitTick
        "HAL_RCC_GetHCLKFreq",       // called by HAL_RCC_GetPCLK1Freq
      "HAL_TIM_Base_Init",           // called by HAL_InitTick
        "HAL_TIM_Base_MspInit",      // called by HAL_TIM_Base_Init
        "TIM_Base_SetConfig",        // called by HAL_TIM_Base_Init
      "HAL_TIM_Base_Start_IT",       // called by HAL_InitTick
    "HAL_MspInit",                   // called by HAL_Init
    "TIM_SlaveTimer_SetConfig",      // called by HAL_TIM_SlaveConfigSynchronization
    "RCCEx_GetSAIxPeriphCLKFreq",
    "initMPU",
    // "HAL_GPIO_Init",
    // "BSP_COM_Init",
    // "HAL_UART_Transmit",
    //     "HAL_UART_Init",
    "HAL_TIM_IRQHandler",            // Timer interrupt handler
    "SysTick_Handler",               // Timer related handler
    "UART_CheckIdleState",           // Timer related function
    "UART_AdvFeatureConfig",         // Timer related function
    "UART_SetConfig",                // Timer related function
};
