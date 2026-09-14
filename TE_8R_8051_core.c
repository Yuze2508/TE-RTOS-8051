/**
 * @file TE_8R_8051_core.c
 * @brief Kernel core: scheduler, tick, task management / 内核核心：调度器、心跳、任务管理
 */

#include "TE_8R_8051_internal.h"

/**
 * @defgroup prio_guide Priority guide / 优先级档位参考
 *
 * 0        → idle (reserved, auto-clamped to 1 if passed) / 空闲独占
 * 1 ~ 3    → background: stats, watchdog, logging / 后台打杂
 * 4 ~ 9    → normal: sampling, control, comm / 常规业务（主力区）
 * 10 ~ 14  → time-critical: hardware response, protection / 时间敏感
 * 15       → hard real-time: emergency shutdown / 最高实时档
 *
 * @note Starvation aging never pushes effective priority into 10~15.
 *       饥饿老化不会侵入实时档。
 * @{ */

/** @} */

/* ---- Kernel data / 内核数据区 ---- */

/** Hot TCB array (data) / 热TCB数组（data区） */
TE_8R_8051_tcb_hot_t data tcb_hot[TE_8R_8051_SLOT_COUNT];

/** Cold TCB array (xdata, idle excluded) / 冷TCB数组（xdata区，空闲任务不参与） */
TE_8R_8051_tcb_cold_t xdata tcb_cold[TE_8R_8051_MAX_TASKS];

/** Currently running task ID / 当前运行任务编号 */
unsigned char data current_task = 0;

/**
 * Yield skip marker. Single-byte write is atomic on 8051 (one MOV).
 * yield一次性落选标记，单字节赋值在8051上是原子的。
 */
unsigned char data TE_8R_8051_yield_skip = TE_8R_8051_SKIP_NONE;

/** Idle task stack in xdata / 空闲任务栈（xdata区） */
unsigned char xdata TE_8R_8051_idle_stack[36];

/** Shared workspace in idata / idata共享工作区 */
unsigned char idata TE_8R_8051_workspace[TE_8R_8051_MAX_WORKSPACE];

/** Peak workspace usage (diagnostic) / 工作区峰值使用量（诊断用） */
unsigned char data TE_8R_8051_workspace_peak = 0;

/* ---- Stack overflow diagnostics / 栈溢出诊断变量 ---- */

unsigned char data TE_8R_8051_stack_overflow_flag = 0;
unsigned char data TE_8R_8051_stack_overflow_id = 0;
unsigned char data TE_8R_8051_stack_overflow_sp = 0;
unsigned char data TE_8R_8051_stack_overflow_task_sp = 0;
unsigned char data TE_8R_8051_stack_overflow_guard_offset = 0;
unsigned char data TE_8R_8051_stack_overflow_magnitude = 0;

/** Per-task mailbox buffer / 每任务邮箱缓冲区 */
unsigned char xdata TE_8R_8051_task_mailbox[TE_8R_8051_MAX_TASKS][TE_8R_8051_MAILBOX_CAPACITY];

/** Software timer array / 软件定时器数组 */
TE_8R_8051_timer_t xdata TE_8R_8051_timers[TE_8R_8051_MAX_TIMERS];

/**
 * @brief  Idle task — kernel-internal fallback / 空闲任务——内核兜底哨兵
 *
 * Always ready, priority 0, never sleeps/blocks/uses mailbox.
 * 永远就绪，优先级0，不睡眠/阻塞/使用邮箱。
 *
 * @note Keep logic minimal: SFR/bit ops only, no LCALL (limited stack).
 *       保持逻辑极简：仅限SFR赋值/位操作，禁止LCALL。
 */
void idle_task(void)
{
    while(1){
        if(TE_8R_8051_stack_overflow_flag){
            P10 = 0;
        }
    }
}

/**
 * @brief  Raw task creation (no validation) / 内部任务创建（无校验）
 *
 * Fabricates an initial stack frame so the scheduler can start the task
 * the same way it resumes an existing one.
 * 伪造初始栈帧，使调度器能像复活老任务一样启动新任务。
 *
 * @param id         Slot index (may be SLOT_MAX for idle)
 * @param prio       Static priority
 * @param task       Task entry function
 * @param stack      Stack base in xdata
 * @param stack_size Stack size in bytes
 *
 * @note Stack layout (grows upward): PCL, PCH, ACC, B, PSW, DPH, DPL, R0~R7.
 *       Sentinel (0xFE) placed at stack[stack_size-1].
 *       栈布局（向高地址生长）：PCL, PCH, ACC, B, PSW, DPH, DPL, R0~R7。
 *       哨兵(0xFE)埋设在stack[stack_size-1]。
 *
 * @warning No bounds checking — caller must validate.
 *          无边界校验，调用者须自行校验。
 */
static void TE_8R_8051_task_create_raw(unsigned char id, unsigned char prio,
                                   TE_8R_8051_Task_t task, unsigned char xdata *stack,
                                   unsigned char stack_size)
{
    unsigned int addr = (unsigned int)task;
    unsigned int stack_addr = (unsigned int)stack;
    unsigned char i;
    unsigned char idata *sp = TE_8R_8051_workspace;
	
	TE_8R_8051_CRITICAL_VAR();
	TE_8R_8051_ENTER_CRITICAL();
    *sp++ = addr & 0xFF;                    /* PCL */
    *sp++ = (addr >> 8) & 0xFF;            /* PCH */

    for (i = 0; i < 13; i++) {              /* ACC, B, PSW, DPH, DPL, R0~R7 = 0 */
        *sp++ = 0;
    }

    tcb_hot[id].sp = TE_8R_8051_INIT_FRAME_SIZE - 1;

    {
        unsigned char j;
        for (j = 0; j < TE_8R_8051_INIT_FRAME_SIZE; j++) {
            stack[j] = TE_8R_8051_workspace[j];
        }
    }

    stack[stack_size - 1] = TE_8R_8051_STACK_MAGIC;

    tcb_hot[id].xstack_dpl = stack_addr & 0xFF;
    tcb_hot[id].xstack_dph = (stack_addr >> 8) & 0xFF;

    if (id < TE_8R_8051_MAX_TASKS) {
        tcb_hot[id].meta = TE_8R_8051_MAKE_PRIO(prio);
        tcb_cold[id].starve = 0;
        tcb_cold[id].delay = 0;
    }
    tcb_hot[id].stack_guard = stack_size - 1;

    if (stack_size > TE_8R_8051_workspace_peak) {
        TE_8R_8051_workspace_peak = stack_size;
    }

    TE_8R_8051_EXIT_CRITICAL();
}

/**
 * @brief  Create a user task (public API) / 创建用户任务（公开API）
 *
 * Validates parameters, then delegates to TE_8R_8051_task_create_raw.
 * 校验参数后委托给task_create_raw。
 *
 * - id out of range → silently rejected / 越界静默拒绝
 * - prio 0 → clamped to 1; prio >15 → clamped to 15 / 0升为1，>15压到15
 * - stack_size+ISR_FRAME_SIZE > MAX_WORKSPACE → rejected / 栈深超限拒绝
 * - stack_size < MIN_STACK_SIZE → rejected / 栈深不足拒绝
 */
void TE_8R_8051_Task_Create(unsigned char id, unsigned char prio, TE_8R_8051_Task_t task, unsigned char xdata *stack, unsigned char stack_size)
{
    if (id >= TE_8R_8051_MAX_TASKS) { return; }
    if (prio == 0) { prio = 1; }
    if (prio > 15) { prio = 15; }
    if (stack_size + 17 > TE_8R_8051_MAX_WORKSPACE) { return; }
    if (stack_size < TE_8R_8051_MIN_STACK_SIZE) { return; }
    TE_8R_8051_task_create_raw(id, prio, task, stack, stack_size);
}

/**
 * @brief  Yield one tick to other tasks / 主动让出一拍CPU
 *
 * Writes own ID to yield_skip (atomic single-byte write), then triggers
 * task_switch. The scheduler skips the caller this round.
 * 写入yield_skip（单字节原子写），然后触发切换，调度器本轮跳过调用者。
 *
 * @warning Task context only, never from ISR. / 仅限任务上下文，禁止ISR。
 */
void TE_8R_8051_yield(void)
{
    TE_8R_8051_yield_skip = current_task;
    TE_8R_8051_task_switch();
}

/**
 * @brief  Initialize the kernel / 初始化内核
 *
 * 1. Mark all user slots as tombstone / 全部用户槽位插墓碑
 * 2. Create idle task at the last slot / 在末槽创建空闲任务
 * 3. Configure Timer 0 as tick source / 配置定时器0为心跳源
 *
 * @note EA is deliberately NOT enabled here — TE_8R_8051_start() enables it
 *       after loading task 0's context.
 *       故意不开EA，等start()装好任务0现场后再开。
 */
void TE_8R_8051_Init(void)
{
    unsigned char i;

    for (i = 0; i < TE_8R_8051_MAX_TASKS; i++) {
        tcb_cold[i].delay = 0xFFFF;
        tcb_cold[i].starve = 0;
    }

    TE_8R_8051_task_create_raw(TE_8R_8051_SLOT_MAX, 0, idle_task, TE_8R_8051_idle_stack, 36);

    AUXR |= 0x80;
    TMOD &= 0xF0;
    TL0 = TIMER0_RELOAD & 0xFF;
    TH0 = (TIMER0_RELOAD >> 8) & 0xFF;
    TF0 = 0;
    TR0 = 1;
    ET0 = 1;
}

/**
 * @brief  Scheduler — decide which task runs next / 调度器——决定下一拍CPU归属
 *
 * Rules / 规则:
 * 1. Higher effective priority wins (preemptive) / 高有效优先级胜出
 * 2. Same priority: round-robin (first-scanned wins) / 同级轮转
 * 3. All sleeping → idle task / 全员睡眠→空闲任务
 * 4. Yield-skip target is excluded this round / yield让位者本轮落榜
 *
 * Effective priority = static priority + starve / AGING_STEP.
 * Aging caps at 1+7=8, never intruding into real-time range (10~15).
 * 有效优先级 = 静态优先级 + starve / AGING_STEP，
 * 老化最多抬到1+7=8，不侵入实时档。
 *
 * @note On stack overflow, shorts directly to idle task (system halted).
 *       栈溢出时短路至空闲任务（系统停机）。
 */
void TE_8R_8051_Schedule(void)
{
    unsigned char i;
    unsigned char next_task = current_task;
    unsigned char best_task = TE_8R_8051_SLOT_MAX;
    unsigned char best_prio = 0;
    bit found = 0;

    if (TE_8R_8051_stack_overflow_flag) {
        current_task = TE_8R_8051_SLOT_MAX;
        return;
    }

    for (i = 0; i < TE_8R_8051_SLOT_COUNT; i++)
    {
        static unsigned char p;
        next_task++;
        if (next_task > TE_8R_8051_SLOT_MAX) {
            next_task = 0;
        }

        /* Rule 4: yield skip / yield让位→本轮落榜 */
        if (next_task == TE_8R_8051_yield_skip) {
            continue;
        }

        /* Idle: always ready, priority 0 / 空闲任务：定义上永远就绪 */
        if (next_task == TE_8R_8051_SLOT_MAX) {
            if (!found) {
                best_prio = 0;
                best_task = TE_8R_8051_SLOT_MAX;
                found = 1;
            }
            continue;
        }

        /* Eligibility: tombstone/blocked/sleeping cannot run / 墓碑/阻塞/睡眠不可候选 */
        if (TE_8R_8051_IS_TOMBSTONE(next_task)) continue;
        if (TE_8R_8051_IS_BLOCKED(next_task))   continue;
        if (TE_8R_8051_IS_SLEEPING(next_task))  continue;

        /* Rule 1&2: effective priority / 有效优先级 */
        p = TE_8R_8051_PRIO_OF(tcb_hot[next_task].meta) + tcb_cold[next_task].starve / TE_8R_8051_AGING_STEP;

        if (!found || p > best_prio)
        {
            best_prio = p;
            best_task = next_task;
            found = 1;
        }
    }

    current_task = best_task;

    /* Starvation accounting / 饥饿记账 */
    for (i = 0; i < TE_8R_8051_MAX_TASKS; i++) {
        if (TE_8R_8051_IS_READY(i) && i != TE_8R_8051_yield_skip) {
            if (i == current_task) {
                tcb_cold[i].starve = 0;
            } else if (tcb_cold[i].starve < 250) {
                tcb_cold[i].starve++;
            }
        }
    }

    /* Consume yield marker (one-shot) / 消费yield标记（一次性） */
    TE_8R_8051_yield_skip = TE_8R_8051_SKIP_NONE;
}

/**
 * @brief  Tick advance: decrement sleeping delays & timer countdowns / 时间推进：睡眠延时减1 & 定时器倒计时
 *
 * @note 16-bit delay decrement is protected by critical section to prevent
 *       torn writes if a higher-priority ISR preempts.
 *       16位减法用临界区保护，防止高优先级中断打断导致撕裂写入。
 */
static void TE_8R_8051_tick_advance(void)
{
    unsigned char i;
    TE_8R_8051_CRITICAL_VAR();

    for (i = 0; i < TE_8R_8051_MAX_TASKS; i++) {
        if (TE_8R_8051_IS_SLEEPING(i)) {
            TE_8R_8051_ENTER_CRITICAL();
            tcb_cold[i].delay--;
            TE_8R_8051_EXIT_CRITICAL();
        }
    }

#if TE_8R_8051_MAX_TIMERS > 0
    for (i = 0; i < TE_8R_8051_MAX_TIMERS; i++) {
        if (!(TE_8R_8051_timers[i].flags & TE_8R_8051_TIMER_ENABLED)) continue;
        if (TE_8R_8051_timers[i].flags & TE_8R_8051_TIMER_EXPIRED) continue;

        TE_8R_8051_ENTER_CRITICAL();
        TE_8R_8051_timers[i].remaining--;
        TE_8R_8051_EXIT_CRITICAL();

        if (TE_8R_8051_timers[i].remaining == 0) {
            TE_8R_8051_timers[i].flags |= TE_8R_8051_TIMER_EXPIRED;
            if (TE_8R_8051_timers[i].flags & TE_8R_8051_TIMER_MODE) {
                TE_8R_8051_timers[i].remaining = TE_8R_8051_timers[i].period;
            } else {
                TE_8R_8051_timers[i].flags &= ~TE_8R_8051_TIMER_ENABLED;
            }
        }
    }
#endif
}

/**
 * @brief  Tick guard: check stack sentinel of current task / 哨兵抽检：检查当前任务栈哨兵
 *
 * Only the running task can overflow its stack (others are frozen).
 * Only idle task is excluded from checks.
 * 只有当前运行任务可能溢出（其他任务冻着），空闲任务不参与抽检。
 *
 * @note On detection: EA=0, TR0=0, ET0=0 → system halted, idle loop runs.
 *       检测到后：关中断、停心跳→系统停机，空闲死循环运行。
 */
static void TE_8R_8051_tick_guard(void)
{
    if (current_task >= TE_8R_8051_MAX_TASKS) return;

    if (TE_8R_8051_workspace[tcb_hot[current_task].stack_guard] != TE_8R_8051_STACK_MAGIC) {
        EA = 0;
        TR0 = 0;
        ET0 = 0;

        TE_8R_8051_stack_overflow_id = current_task;
        TE_8R_8051_stack_overflow_magnitude = 0;
        TE_8R_8051_stack_overflow_task_sp = tcb_hot[current_task].sp;
        TE_8R_8051_stack_overflow_sp = SP;
        TE_8R_8051_stack_overflow_guard_offset = tcb_hot[current_task].stack_guard;
        TE_8R_8051_stack_overflow_flag = 1;
    }
}

/**
 * @brief  Tick handler entry (called from Timer0 ISR via LCALL) / 心跳入口（汇编LCALL目标）
 *
 * Splits into tick_advance (time) and tick_guard (sentinel check).
 * 拆成tick_advance（时间推进）和tick_guard（哨兵抽检）。
 */
void TE_8R_8051_Tick_Handler(void)
{
    TE_8R_8051_tick_advance();
    TE_8R_8051_tick_guard();
}

/**
 * @brief  Delay current task by ticks / 当前任务睡眠指定Tick数
 *
 * @param ticks Sleep duration. Values >= TE_8R_8051_TASK_BLOCKED are
 *              auto-clamped to the maximum valid sleep value.
 *              睡眠Tick数，保留值自动兜底。
 *
 * @note 16-bit write to tcb_cold[].delay is protected by critical section.
 *       16位写入delay用临界区保护。
 */
void TE_8R_8051_Delay(unsigned int ticks)
{
    TE_8R_8051_CRITICAL_VAR();
    if(ticks >= TE_8R_8051_TASK_BLOCKED){ticks = TE_8R_8051_TASK_BLOCKED - 1;}

    TE_8R_8051_ENTER_CRITICAL();
    tcb_cold[current_task].delay = ticks;
    TE_8R_8051_EXIT_CRITICAL();

    TE_8R_8051_task_switch();
}