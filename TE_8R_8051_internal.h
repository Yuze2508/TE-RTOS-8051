/**
 * @file TE_8R_8051_internal.h
 * @brief Internal macros and declarations for kernel source files / 内部宏与声明，仅供内核源文件使用
 *
 * @warning This header is NOT part of the public API. Users must not include it.
 *          本头文件不是公开API，用户禁止包含。
 */

#ifndef __TE_8R_8051_INTERNAL_H__
#define __TE_8R_8051_INTERNAL_H__

#include "TE_8R_8051.h"

/**
 * @defgroup slot Slot mapping / 槽位映射
 *
 * Physical slots 0..MAX_TASKS-1 = user tasks.
 * Physical slot MAX_TASKS = idle task (user-invisible).
 * 物理槽位0..MAX_TASKS-1为用户任务，槽位MAX_TASKS为空闲任务。
 *
 * @note Idle task has no cold-TCB slot (it never sleeps/blocks/uses mailbox).
 *       空闲任务无冷TCB槽位（永不睡眠/阻塞/使用邮箱）。
 * @{ */

/** Idle task slot index / 空闲任务槽位索引 */
#define TE_8R_8051_SLOT_MAX   TE_8R_8051_MAX_TASKS

/** Total slot count (user tasks + idle) / 总槽位数（用户任务+空闲） */
#define TE_8R_8051_SLOT_COUNT (TE_8R_8051_MAX_TASKS + 1)

/** @} */

/** Yield skip sentinel: no task is skipping / yield跳过标记：无人让位 */
#define TE_8R_8051_SKIP_NONE 0xFF

/**
 * @defgroup state_pred Task state predicates / 任务状态谓词宏
 *
 * tcb_cold[i].delay encodes the full task state:
 * - 0xFFFF  = tombstone (slot unused) / 墓碑（槽位未使用）
 * - 0xFFFE  = blocked (waiting for mutex) / 阻塞（等互斥量）
 * - 0       = ready / 就绪
 * - 1~65533 = sleeping (decremented each tick) / 睡眠（每Tick减1）
 *
 * tcb_cold[i].delay 编码了任务的完整状态。
 *
 * @note IS_SLEEPING reads delay twice (16-bit); must be used with interrupts
 *       disabled to prevent torn reads.
 *       IS_SLEEPING两次读取delay（16位），须在关中断环境下使用。
 * @{ */

#define TE_8R_8051_IS_TOMBSTONE(i)  (tcb_cold[i].delay == 0xFFFF)   /**< Tombstone? / 是否墓碑 */
#define TE_8R_8051_IS_BLOCKED(i)    (tcb_cold[i].delay == TE_8R_8051_TASK_BLOCKED)   /**< Blocked? / 是否阻塞 */
#define TE_8R_8051_IS_READY(i)      (tcb_cold[i].delay == 0)   /**< Ready? / 是否就绪 */
#define TE_8R_8051_IS_SLEEPING(i)   (tcb_cold[i].delay > 0 && tcb_cold[i].delay < TE_8R_8051_TASK_BLOCKED)   /**< Sleeping? / 是否睡眠 */

/** @} */

/**
 * @defgroup asm_contract Assembly-C contract (compile-time assertions) / 汇编-C契约（编译时断言）
 *
 * Keil C51 lacks _Static_assert. The classic trick: declare an extern array
 * in code with size = (condition ? 1 : -1). If condition fails, negative size
 * causes a compile error.
 * Keil C51不支持_Static_assert，用负数组大小技巧实现编译时断言。
 *
 * Contract list / 契约清单:
 * - sizeof(TE_8R_8051_tcb_hot_t) == 5 (assembly uses MUL AB,#05H for TCB offset)
 * - TE_8R_8051_ISR_FRAME_SIZE == 17 (assembly uses #0BH for boundary check)
 * @{ */

extern unsigned char code _assert_tcb_hot_size[sizeof(TE_8R_8051_tcb_hot_t) == 5 ? 1 : -1];
extern unsigned char code _assert_isr_frame[TE_8R_8051_ISR_FRAME_SIZE == 17 ? 1 : -1];

/** @} */

/**
 * @defgroup kernel_data Kernel data (defined in TE_8R_8051_core.c) / 内核数据（定义于core.c）
 * @{ */

/** Currently running task ID / 当前运行任务编号 */
extern unsigned char data current_task;

/** Yield skip marker: task ID to skip this round, or TE_8R_8051_SKIP_NONE / yield一次性落选标记 */
extern unsigned char data TE_8R_8051_yield_skip;

/** Per-task mailbox buffer / 每任务邮箱缓冲区 */
extern unsigned char xdata TE_8R_8051_task_mailbox[][TE_8R_8051_MAILBOX_CAPACITY];

/** Software timer array / 软件定时器数组 */
extern TE_8R_8051_timer_t xdata TE_8R_8051_timers[];

/** @} */

/**
 * @defgroup asm_iface Assembly interface (Kernel.a51) / 汇编接口（Kernel.a51）
 * @{ */

/** Kernel ignition: resume first task from fabricated context, never returns / 内核点火：从伪造现场复活第一个任务，永不返回 */
extern void TE_8R_8051_start(void);

/** Task switch: save current context → schedule → resume next task / 任务切换：保存现场→调度→复活下一任务 */
extern void TE_8R_8051_task_switch(void);

/** @} */

#endif