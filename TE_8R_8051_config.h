#ifndef __TE_8R_8051_CONFIG_H__
#define __TE_8R_8051_CONFIG_H__

/**
 * @file TE_8R_8051_config.h
 * @brief User-configurable parameters / 用户可配置参数
 *
 * All tunable parameters are centralized here.
 * Modify only this file; never edit kernel internals.
 * 所有可调参数集中于此，只改本文件，不碰内核。
 *
 * @note Assembly-side constants must be synchronized in TE_8R_8051_config.inc.
 *       汇编侧常量须在 TE_8R_8051_config.inc 中手动同步。
 */

/* ---- System clock / 系统时钟 ---- */
#define FOSC 24000000UL                        /**< MCU frequency / 主频 */
#define TICK_MS 1                              /**< Tick period in ms / 每Tick毫秒数 */

/* ---- Task count / 任务规模 ---- */
#define TE_8R_8051_MAX_TASKS 4

#if TE_8R_8051_MAX_TASKS < 1
#error "TE_8R_8051_MAX_TASKS must be >= 1"
#endif
#if TE_8R_8051_MAX_TASKS > 25
#error "TE_8R_8051_MAX_TASKS > 25 exceeds data RAM capacity"
#endif

/* ---- Mailbox capacity / 邮箱容量 ---- */
#define TE_8R_8051_MAILBOX_CAPACITY 4

#if TE_8R_8051_MAILBOX_CAPACITY > 15
#error "TE_8R_8051_MAILBOX_CAPACITY exceeds 15-byte hard limit (4-bit len field)"
#error "Need >=16-byte mailbox? Use a 32-bit MCU instead. That simple."
#endif
#if TE_8R_8051_MAILBOX_CAPACITY < 1
#error "TE_8R_8051_MAILBOX_CAPACITY must be >= 1"
#endif

/* ---- Starvation aging step / 饥饿老化步长 ---- */
#define TE_8R_8051_AGING_STEP 32

#if TE_8R_8051_AGING_STEP == 0
#error "TE_8R_8051_AGING_STEP must not be 0 (division by zero in scheduler)"
#endif

/* ---- Workspace size / 工作区大小 ---- */
#define TE_8R_8051_MAX_WORKSPACE  64

/* ---- Software timers / 软件定时器 ---- */
#define TE_8R_8051_MAX_TIMERS 4

/* ---- Stack sentinel magic / 栈哨兵魔数 ---- */
#define TE_8R_8051_STACK_MAGIC  0xFE

/* ---- Compile-time consistency checks / 编译期一致性检查 ---- */
#if (FOSC * TICK_MS / 1000) > 65535
#error "FOSC * TICK_MS / 1000 exceeds 16-bit timer range"
#endif

#endif