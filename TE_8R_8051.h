/**
 * @file TE_8R_8051.h
 * @brief RTOS public API and type definitions / RTOS 公开 API 与类型定义
 *
 * All user-tunable parameters are in TE_8R_8051_config.h.
 * 所有可调参数集中在 TE_8R_8051_config.h。
 *
 * @note Assembly-side constants must be synchronized in TE_8R_8051_config.inc.
 *       汇编侧常量须在 TE_8R_8051_config.inc 中手动同步。
 */

#ifndef __TE_8R_8051_H__
#define __TE_8R_8051_H__

#include <STC15F2K60S2.H>
#include "TE_8R_8051_config.h"

/** @defgroup system System constants / 系统常量
 *  @{ */

/** Timer 0 reload value / 定时器0重装载值 */
#define TIMER0_RELOAD (65536UL - (FOSC * TICK_MS / 1000))

/** @} */

/* ---- Task delay special values / 任务延时特殊值 ---- */

/** Task delay: blocked (waiting for mutex) / 任务延时：阻塞（等互斥量） */
#define TE_8R_8051_TASK_BLOCKED 0xFFFE

/** Mutex: no owner / 互斥量：无人持有 */
#define TE_8R_8051_MUTEX_NONE   0xFF

/* ---- Stack frame sizes / 栈帧大小 ---- */

/** Initial frame size for a new task / 新任务初始帧大小 */
#define TE_8R_8051_INIT_FRAME_SIZE 15

/** ISR frame size / 中断帧大小 */
#define TE_8R_8051_ISR_FRAME_SIZE  17

/** Minimum stack size: init + ISR + sentinel / 最小栈需求：初始帧+中断帧+哨兵 */
#define TE_8R_8051_MIN_STACK_SIZE  (TE_8R_8051_INIT_FRAME_SIZE + TE_8R_8051_ISR_FRAME_SIZE + 1)

#if TE_8R_8051_MAX_WORKSPACE < TE_8R_8051_INIT_FRAME_SIZE
#error "TE_8R_8051_MAX_WORKSPACE must be >= TE_8R_8051_INIT_FRAME_SIZE / 工作区至少为初始帧大小"
#endif

/**
 * @defgroup tcb Task Control Block / 任务控制块
 *
 * Hot TCB resides in data (fast access by scheduler/assembly).
 * Cold TCB resides in xdata (less frequent access).
 * 热TCB位于data区（调度器/汇编热路径高频访问）。
 * 冷TCB位于xdata（低频访问）。
 *
 * @note 8051 C compiler requires a struct to reside entirely in one
 *       storage area. Splitting into two structs allows hot fields in
 *       data and cold fields in xdata.
 *       8051 C编译器要求结构体整体在同一存储区，
 *       分成两个结构体才能将热字段放data、冷字段放xdata。
 * @{ */

/** Hot TCB: scheduler/assembly hot-path fields / 热TCB：调度器/汇编热路径字段 */
typedef struct {
    unsigned char sp;           /**< Stack pointer offset / 栈顶偏移量 */
    unsigned char meta;         /**< Priority + mail-length bitmap / 优先级+信长位图 */
    unsigned char xstack_dpl;   /**< xdata stack base low byte / xdata栈基址低字节 */
    unsigned char xstack_dph;   /**< xdata stack base high byte / xdata栈基址高字节 */
    unsigned char stack_guard;  /**< Sentinel offset / 哨兵偏移量 */
} TE_8R_8051_tcb_hot_t;

/** Cold TCB: tick/API-accessed fields / 冷TCB：Tick/API访问的字段 */
typedef struct {
    unsigned int  delay;        /**< Sleep/block/tombstone / 睡眠/阻塞/墓碑 */
    unsigned char starve;       /**< Starvation counter / 饥饿计数 */
} TE_8R_8051_tcb_cold_t;

/** @} */

/**
 * @defgroup meta_bitmap meta field layout / meta 字段位图
 *
 * High nibble (bit7~4): static priority (0~15).
 * Low nibble  (bit3~0): mail length   (0=no mail, 1~15=bytes).
 * 高4位(bit7~4)：静态优先级(0~15)。
 * 低4位(bit3~0)：邮箱信长(0=无信, 1~15=有信)。
 *
 * @note Scheduler reads/writes high nibble only; mailbox reads/writes
 *       low nibble only. Read-modify-write on the whole byte requires
 *       interrupts disabled.
 *       调度器只读写高4位，邮箱只读写低4位。
 *       读-改-写整个字节时必须关中断。
 * @{ */

/** Extract priority from meta / 从meta提取优先级 */
#define TE_8R_8051_PRIO_OF(v)     ((unsigned char)(v) >> 4)

/** Extract mail length from meta / 从meta提取信长 */
#define TE_8R_8051_LEN_OF(v)     ((unsigned char)(v) & 0x0F)

/** Encode priority into high nibble / 将优先级编码到高4位 */
#define TE_8R_8051_MAKE_PRIO(p)  ((unsigned char)(p) << 4)

/** Set mail length (low nibble only; interrupts must be off!) / 设置信长（仅改低4位，须关中断！） */
#define TE_8R_8051_SET_LEN(v, l) ((v) = ((v) & 0xF0) | (l))

/** Set priority (high nibble only; interrupts must be off!) / 设置优先级（仅改高4位，须关中断！） */
#define TE_8R_8051_SET_PRIO(v, p) ((v) = ((v) & 0x0F) | TE_8R_8051_MAKE_PRIO(p))

/** @} */

/** Hot TCB array in data / 热TCB数组（data区） */
extern TE_8R_8051_tcb_hot_t data tcb_hot[];

/** Cold TCB array in xdata / 冷TCB数组（xdata区） */
extern TE_8R_8051_tcb_cold_t xdata tcb_cold[];

/**
 * @defgroup critical Critical section / 临界区
 *
 * Protect code segments that must not be interrupted.
 * 保护不能被中断打断的代码段。
 *
 * Usage / 用法:
 * @code
 *   TE_8R_8051_CRITICAL_VAR();
 *   TE_8R_8051_ENTER_CRITICAL();
 *   ... protected code ...
 *   TE_8R_8051_EXIT_CRITICAL();
 * @endcode
 *
 * @warning Each function at most one critical pair (variable name _ea_bak_ is fixed).
 *          每个函数最多一对临界区（变量名_ea_bak_固定，写两对会重名）。
 * @warning Early return inside critical section must call TE_8R_8051_EXIT_CRITICAL() first.
 *          临界区内提前return须先调TE_8R_8051_EXIT_CRITICAL()。
 * @warning Never call Delay/yield/mutex_acquire inside critical section.
 *          临界区内禁止调用Delay/yield/mutex_acquire。
 * @{ */

#define TE_8R_8051_CRITICAL_VAR()    unsigned char _ea_bak_          /**< Declare saved EA / 声明中断保存变量 */
#define TE_8R_8051_ENTER_CRITICAL()  do { _ea_bak_ = EA; EA = 0; } while(0)   /**< Save EA, disable interrupts / 保存EA，关中断 */
#define TE_8R_8051_EXIT_CRITICAL()   do { EA = _ea_bak_; } while(0)  /**< Restore EA / 还原EA */

/** @} */

/** Task function type: void func(void) / 任务函数类型 */
typedef void (*TE_8R_8051_Task_t)(void);

/**
 * @brief  Initialize the kernel / 初始化内核
 *
 * Marks all task slots as unused, creates the idle task, and configures
 * Timer 0 as the tick source.
 * 标记所有槽位为未使用，创建空闲任务，配置定时器0为心跳源。
 *
 * After this, create tasks with TE_8R_8051_Task_Create(), then call
 * TE_8R_8051_Start().
 * 此后创建任务，最后调TE_8R_8051_Start()启动调度。
 */
void TE_8R_8051_Init(void);

/**
 * @brief  Create a user task / 创建用户任务
 *
 * @param id         Task ID (0 ~ TE_8R_8051_MAX_TASKS-1). Out of range is silently rejected.
 *                   任务编号，越界静默拒绝。
 * @param prio       Static priority (1~15, higher = more urgent).
 *                   0 is reserved for idle (auto-clamped to 1); >15 clamped to 15.
 *                   静态优先级(1~15)，0自动升为1，>15压到15。
 * @param task       Task entry function (must be an infinite loop, never return).
 *                   任务入口函数（必须是死循环，不能返回）。
 * @param stack      Stack base address in xdata (stack grows upward on 8051).
 *                   xdata区栈首地址（栈向高地址生长）。
 * @param stack_size Stack size in bytes. Must be >= TE_8R_8051_MIN_STACK_SIZE
 *                   plus call-nesting overhead, and <= TE_8R_8051_MAX_WORKSPACE.
 *                   栈区字节数，须 >= TE_8R_8051_MIN_STACK_SIZE + 调用嵌套开销，
 *                   且 <= TE_8R_8051_MAX_WORKSPACE。
 *
 * @warning Must be called before TE_8R_8051_Start(). After Start the kernel
 *          is running and Create causes a data race.
 *          必须在Start之前调用！Start后Create会导致数据竞争。
 */
void TE_8R_8051_Task_Create(unsigned char id, unsigned char prio,
                        TE_8R_8051_Task_t task, unsigned char xdata *stack,
                        unsigned char stack_size);

/**
 * @brief  Start the kernel scheduler (never returns) / 启动内核调度（永不返回）
 *
 * Begins executing from task 0 and enables global interrupts.
 * 从任务0开始执行，开总中断，心跳开始跳动。
 *
 * @pre TE_8R_8051_Init() and all TE_8R_8051_Task_Create() must have been called.
 *      必须已完成Init和所有Task_Create。
 */
void TE_8R_8051_Start(void);

/**
 * @brief  Sleep for the given number of ticks, then yield CPU / 睡眠指定Tick数后让出CPU
 *
 * @param ticks Sleep duration in ticks.
 *   - 0     = yield without penalty (scheduler may re-select immediately)
 *   - 1~65533 = sleep, auto-wake when counter reaches 0
 *   - 65534/65535 are reserved; passed values are auto-clamped
 *   睡眠Tick数：0=无惩罚让出；1~65533=睡眠后自动醒来；保留值自动兜底
 *
 * @note If a mail arrives while sleeping, the task is woken early.
 *       睡眠期间收到邮件会提前唤醒。
 *
 * @note TE_8R_8051_Delay(0) may re-select the same task;
 *       TE_8R_8051_yield() guarantees the scheduler skips the caller this round.
 *       Delay(0)调度器可能又选回你；yield()保证本轮一定不选你。
 */
void TE_8R_8051_Delay(unsigned int ticks);

/**
 * @brief  Yield one tick to other tasks / 主动让出一拍CPU
 *
 * The scheduler is guaranteed to skip the caller this round, even if it
 * has the highest priority.
 * 调度器本轮一定不选回调用者，即使其优先级最高。
 *
 * @warning Must not be called from an ISR. ISR has no task context.
 *          禁止在ISR中调用！ISR不是任务上下文。
 */
void TE_8R_8051_yield(void);

/**
 * @brief  Send mail to a task's mailbox (wake on delivery) / 发送邮件并唤醒目标任务
 *
 * @param id    Target task ID (0 ~ TE_8R_8051_MAX_TASKS-1)
 * @param dat_p Data pointer in xdata
 * @param len   Data length (1 ~ TE_8R_8051_MAILBOX_CAPACITY); 0 or overflow returns failure
 * @return      1=success, 0=invalid parameters
 *
 * Mailbox semantics: latest value wins. Unread mail is overwritten.
 * 邮箱语义：最新值有效，旧信未读则被覆盖。
 *
 * On success, if the target task is sleeping it is woken immediately.
 * 投递成功后，若目标任务正在睡眠则立刻唤醒。
 *
 * @warning Must not be called from an ISR. Use TE_8R_8051_Post_Mailbox instead.
 *          禁止在ISR中调用！ISR中请用TE_8R_8051_Post_Mailbox。
 */
unsigned char TE_8R_8051_Send_Mailbox(unsigned char id, unsigned char xdata *dat_p, unsigned char len);

/**
 * @brief  Post mail silently (no wake) / 静默投递邮件（不唤醒）
 *
 * @param id    Target task ID
 * @param dat_p Data pointer in xdata
 * @param len   Data length (1 ~ TE_8R_8051_MAILBOX_CAPACITY)
 * @return      1=success, 0=invalid parameters
 *
 * Same as TE_8R_8051_Send_Mailbox but does not wake the target task.
 * 与Send_Mailbox唯一区别：投递后不唤醒目标任务。
 *
 * Use in ISR or for pre-filling a mailbox before the target starts.
 * 适用于ISR投递或开机预填邮箱。
 */
unsigned char TE_8R_8051_Post_Mailbox(unsigned char id, unsigned char xdata *dat_p, unsigned char len);

/**
 * @brief  Read own mailbox / 读取本任务邮箱
 *
 * @param dat_p  Receive buffer in xdata
 * @param buflen Buffer capacity in bytes
 * @return       Bytes actually read; 0 = no mail or buffer too small
 *
 * Only the owner task can read its mailbox.
 * 只能读自己的邮箱。
 *
 * @note If buflen < mail length, the mail is not consumed (read again with a larger buffer).
 *       缓冲区太小则不消费邮件，换大缓冲区重读即可。
 */
unsigned char TE_8R_8051_Read_Mailbox(unsigned char xdata *dat_p, unsigned char buflen);

/**
 * @defgroup mutex Mutex with priority inheritance / 带优先级继承的互斥量
 *
 * When a high-priority task waits for a lock held by a low-priority task,
 * the holder's priority is temporarily raised to the waiter's level
 * (priority inheritance), preventing medium-priority tasks from causing
 * unbounded priority inversion.
 * 高优先级任务等锁时，持有者优先级临时提升到等锁者级别（优先级继承），
 * 防止中等优先级任务导致无界优先级反转。
 *
 * Usage / 用法:
 * @code
 *   TE_8R_8051_mutex_t xdata my_mutex;
 *   TE_8R_8051_mutex_init(&my_mutex);
 *   TE_8R_8051_mutex_acquire(&my_mutex);
 *   ... critical resource ...
 *   TE_8R_8051_mutex_release(&my_mutex);
 * @endcode
 *
 * @warning Rules / 纪律:
 *   - Same task that acquires must release / 谁拿谁放
 *   - No recursive acquire (self-lock detected, returns without blocking) / 禁止递归拿锁
 *   - Consistent lock ordering to avoid deadlock / 多锁获取顺序须一致
 *   - No nested holding (holding A while acquiring B is forbidden) / 禁止嵌套持有
 *   - Must not be called from ISR / 禁止在ISR中调用
 * @{ */

/** Waiter bitmap width: ceil(MAX_TASKS / 8) / 等待位图宽度 */
#define TE_8R_8051_WAITER_WORDS  ((TE_8R_8051_MAX_TASKS + 7) / 8)

/** Mutex structure / 互斥量结构体 */
typedef struct {
    unsigned char owner;        /**< Owning task ID, or TE_8R_8051_MUTEX_NONE / 持有者编号 */
    unsigned char orig_prio;    /**< Original priority before inheritance / 继承前的原始优先级 */
    unsigned char waiters[TE_8R_8051_WAITER_WORDS];
                                /**< Waiter bitmap: bit[i]=1 means task i is waiting / 等待位图 */
} TE_8R_8051_mutex_t;

/**
 * @brief  Initialize mutex as unowned / 初始化互斥量为无人持有
 *
 * @param m Mutex pointer in xdata
 *
 * @pre Must be called after TE_8R_8051_Init().
 *      必须在Init之后调用。
 */
void TE_8R_8051_mutex_init(TE_8R_8051_mutex_t xdata *m);

/**
 * @brief  Acquire mutex (may block) / 获取互斥量（可能阻塞）
 *
 * @param m Mutex pointer in xdata
 *
 * - Lock free: acquired immediately / 锁空闲：直接拿到
 * - Self-lock: detected, returns without blocking / 自锁：检测到，直接返回
 * - Held by other: priority inheritance + block / 被他人持有：优先级继承+阻塞
 *
 * @warning Task context only, never from ISR. / 仅限任务上下文，禁止ISR。
 */
void TE_8R_8051_mutex_acquire(TE_8R_8051_mutex_t xdata *m);

/**
 * @brief  Release mutex / 释放互斥量
 *
 * @param m Mutex pointer in xdata
 * @return 1=success, 0=caller is not the owner
 *
 * Restores the owner's original priority. If waiters exist, transfers
 * the lock to the highest-priority waiter and wakes it.
 * 恢复持有者原始优先级。若有等待者，转锁给最高优先级者并唤醒。
 *
 * @warning Only the owner may release. / 只有持有者才能释放。
 */
unsigned char TE_8R_8051_mutex_release(TE_8R_8051_mutex_t xdata *m);

/**
 * @brief  Try to acquire mutex without blocking / 尝试获取互斥量（不阻塞）
 *
 * @param m Mutex pointer in xdata
 * @return 1=acquired, 0=lock held by another task
 *
 * Use for polling-style design / 适用于轮询式设计:
 * @code
 *   if (TE_8R_8051_mutex_try_acquire(&m)) {
 *       ... critical resource ...
 *       TE_8R_8051_mutex_release(&m);
 *   }
 * @endcode
 */
unsigned char TE_8R_8051_mutex_try_acquire(TE_8R_8051_mutex_t xdata *m);

/** @} */

/**
 * @defgroup timer Software timers / 软件定时器
 *
 * Flag-based (not callback): on expiry a flag is set; the task polls it.
 * This matches bare-metal 8051 practice (check TF0, RI, etc.).
 * 标志位语义（非回调）：到期设标志，任务自行查询，与裸机查TF0/RI习惯一致。
 *
 * Periodic timers auto-reload on expiry with zero drift.
 * 周期定时器到期后自动重载，零漂移。
 * @{ */

/** One-shot mode: stop after expiry / 单次模式：到期后停止 */
#define TE_8R_8051_TIMER_ONE_SHOT  0x00

/** Periodic mode: reload after expiry / 周期模式：到期后重载 */
#define TE_8R_8051_TIMER_PERIODIC  0x01

/** Timer flag: enabled / 定时器标志：使能 */
#define TE_8R_8051_TIMER_ENABLED   0x01

/** Timer flag: mode bit / 定时器标志：模式位 */
#define TE_8R_8051_TIMER_MODE      0x02

/** Timer flag: expired (unread) / 定时器标志：已到期（未读） */
#define TE_8R_8051_TIMER_EXPIRED   0x04

/** Timer structure / 定时器结构体 */
typedef struct {
    unsigned int  period;     /**< Period in ticks / 周期（Tick数） */
    unsigned int  remaining;  /**< Remaining ticks / 剩余Tick数 */
    unsigned char flags;     /**< bit0:enable bit1:mode bit2:expired / bit0:使能 bit1:模式 bit2:已到期 */
} TE_8R_8051_timer_t;

#if TE_8R_8051_MAX_TIMERS > 0

/**
 * @brief  Start a software timer / 启动软件定时器
 *
 * @param id      Timer ID (0 ~ TE_8R_8051_MAX_TIMERS-1)
 * @param period  Period in ticks (1~65535; 0 clamped to 1)
 * @param mode    TE_8R_8051_TIMER_ONE_SHOT or TE_8R_8051_TIMER_PERIODIC
 */
void TE_8R_8051_timer_start(unsigned char id, unsigned int period, unsigned char mode);

/**
 * @brief  Stop a software timer / 停止软件定时器
 *
 * @param id Timer ID
 *
 * Remaining and flags are preserved (cleared by reset).
 * remaining和flags保留（由reset清零）。
 */
void TE_8R_8051_timer_stop(unsigned char id);

/**
 * @brief  Check if timer expired (read-clear) / 查询定时器是否到期（读后自动清标志）
 *
 * @param id Timer ID
 * @return   1=expired (flag cleared), 0=not expired
 *
 * Usage / 用法:
 * @code
 *   if (TE_8R_8051_timer_expired(0)) { ... }
 *   TE_8R_8051_Delay(1);
 * @endcode
 */
unsigned char TE_8R_8051_timer_expired(unsigned char id);

/**
 * @brief  Reset timer (reload remaining=period, clear expired flag) / 重置定时器
 *
 * @param id Timer ID
 *
 * Equivalent to stop + start with the same period and mode.
 * 等效于先stop再start（保持原period和mode）。
 */
void TE_8R_8051_timer_reset(unsigned char id);

/**
 * @brief  Check if timer is running / 查询定时器是否正在运行
 *
 * @param id Timer ID
 * @return   1=running, 0=stopped
 */
unsigned char TE_8R_8051_timer_is_running(unsigned char id);

#endif

/** @} */

/* ---- Stack overflow diagnostics / 栈溢出诊断变量 ---- */

/** Stack overflow detected flag / 栈溢出检测标志 */
extern unsigned char data TE_8R_8051_stack_overflow_flag;

/** Task ID that caused overflow / 溢出任务编号 */
extern unsigned char data TE_8R_8051_stack_overflow_id;

/** SP at overflow / 溢出时SP值 */
extern unsigned char data TE_8R_8051_stack_overflow_sp;

/** Task SP offset at overflow / 溢出任务栈顶偏移量 */
extern unsigned char data TE_8R_8051_stack_overflow_task_sp;

/** Sentinel offset at overflow / 溢出时哨兵偏移量 */
extern unsigned char data TE_8R_8051_stack_overflow_guard_offset;

/** Overflow magnitude / 溢出程度 */
extern unsigned char data TE_8R_8051_stack_overflow_magnitude;

/** Shared workspace in idata: the single stack area for the running task / idata共享工作区：当前运行任务独占的栈区 */
extern unsigned char idata TE_8R_8051_workspace[];

/** Peak workspace usage (diagnostic) / 工作区峰值使用量（诊断用） */
extern unsigned char data TE_8R_8051_workspace_peak;

/**
 * @defgroup constraints Hard constraints / 硬约束
 *
 * Violating these will not cause compile errors but leads to runtime issues.
 * 违反以下约束不会编译报错，但会导致运行时问题。
 *
 * -# All task functions must use register bank 0 (no Keil `using` keyword).
 *    所有任务函数必须使用第0组寄存器，禁止用Keil的using关键字。
 * -# Stack budget: stack_size >= TE_8R_8051_MIN_STACK_SIZE + 2 * call-nesting-depth
 *    (large model); add local-variable total for small model.
 *    栈预算：large模型下 stack_size >= TE_8R_8051_MIN_STACK_SIZE + 2×调用嵌套深度；
 *    small模型还需加局部变量总和。
 * -# idle_task must not overflow: keep its logic minimal (SFR/bit ops only, no LCALL).
 *    idle_task不能溢出：仅限SFR赋值/位操作，禁止LCALL。
 * -# Storage model: tasks = large (recommended), kernel = small, ISR = small.
 *    Mailbox API pointers are xdata-exclusive.
 *    存储模型：任务建议large，内核必须small，ISR必须small。
 *    邮箱API指针为xdata专属。
 * -# TE_8R_8051_Task_Create must precede TE_8R_8051_Start.
 *    Task_Create必须在Start之前调用。
 * @{ */

/** @} */

#endif