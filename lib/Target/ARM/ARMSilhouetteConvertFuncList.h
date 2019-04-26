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
  "benchmark",
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
    // Shadow stack experimental blacklist
    "main",
		"prvmiscinitialization",
		"hal_init",
		"hal_nvic_setprioritygrouping",
		"assert_param",
		"nvic_setprioritygrouping",
		"hal_systick_config",
		"systick_config",
		"hal_nvic_setpriority",
		"is_nvic_sub_priority",
		"is_nvic_preemption_priority",
		"nvic_getprioritygrouping",
		"nvic_encodepriority",
		"hal_mspinit",
		"nvic_setpriority",
		"systemclock_config",
		"hal_rcc_oscconfig",
		"rcc_setflashlatencyfrommsirange",
		"hal_pwrex_getvoltagerange",
		"hal_rcc_getsysclockfreq",
		"hal_inittick",
		"hal_gettick",
		"prvinitializeheap",
		"vportdefineheapregions",
		"configassert",
		"bsp_led_init",
		"hal_nvic_enableirq",
		"nvic_enableirq",
		"hal_rng_init",
		"hal_rng_mspinit",
		"rtc_init",
		"hal_rtc_init",
		"hal_rtc_mspinit",
		"rtc_enterinitmode",
		"hal_rtc_settime",
		"hal_rtc_setdate",
		"console_uart_init",
		"bsp_com_init",
		"hal_gpio_init",
		"hal_uart_init",
		"xloggingtaskinitialize",
		"xtaskcreate",
		"vapplicationgetidletaskmemory",
		"xtaskcreatestatic",
    "hal_rcc_clockconfig",
    "hal_rccex_periphclkconfig",
    "bsp_pb_init",
    "hal_rtc_waitforsynchro",
    "uart_setconfig",
    "hal_rcc_getpclk2freq",
    "uart_checkidlestate",
    "uart_waitonflaguntiltimeout",
    "xqueuegenericcreate",
    "pvportmalloc",
    "xtaskresumeall",
    "prvinitialisenewqueue",
    "xqueuegenericreset",
    "prvinitialisenewtask",
    "pxcurrenttcb",
    "pxportinitialisestack",
    "prvaddnewtasktoreadylist",
    "prvportinitialisetasklists",
    "prvinitialisetasklists",
    "vtaskstartscheduler",
    "xtimercreatetimertask",
    "prvcheckforvalidlistandqueue",
    "xqueuegenericcreatestatic",
    "vapplicationgettimertaskmemory",
    "xportstartscheduler",
    "strdup",
    "malloc",
    "_sbrk",
    "_sbrk_r",
    "_exit",
    "_kill",
    "xqueuegenericsend",
    "prvcopydatatoqueue",
    "mqtt_agent_publish",
    "prvsendcommandtomqtttask",
    "systick_handler",
    "hal_systick_irqhandler",
    "hal_systick_callback",
    "vportraisebasepri",
    "vtaskswitchcontext",
    "hal_flash_program",
    "flash_waitforlastoperation",
    "flash_program_doubleword"
};
