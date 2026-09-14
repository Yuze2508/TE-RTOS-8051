/**
 * @file TE_8R_8051_mutex.c
 * @brief Mutex with priority inheritance / 带优先级继承的互斥量
 *
 * Waiter relationship stored in per-mutex bitmap (waiters[]).
 * Eliminates the NULL-vs-xdata-0x0000 collision defect of the old
 * pointer-array approach.
 * 等待关系存于互斥量侧的位图，消除了旧版指针数组的NULL碰撞缺陷。
 */

#include "TE_8R_8051_internal.h"

/**
 * @brief  Find highest-priority waiter in mutex / 找出互斥量中最高优先级等待者
 *
 * @param m Mutex pointer
 * @return  Task ID, or TE_8R_8051_MUTEX_NONE if no waiters
 *
 * @note Compares static priority only (waiters' starve is zero while blocked).
 *       仅比较静态优先级（等待者阻塞时starve已清零）。
 */
static unsigned char TE_8R_8051_mutex_best_waiter(TE_8R_8051_mutex_t xdata *m)
{
    unsigned char i;
    unsigned char best = TE_8R_8051_MUTEX_NONE;
    unsigned char best_prio = 0;

    for (i = 0; i < TE_8R_8051_MAX_TASKS; i++) {
        if (m->waiters[i / 8] & (unsigned char)(1 << (i % 8))) {
            unsigned char p = TE_8R_8051_PRIO_OF(tcb_hot[i].meta);
            if (best == TE_8R_8051_MUTEX_NONE || p > best_prio) {
                best_prio = p;
                best = i;
            }
        }
    }
    return best;
}

/**
 * @brief  Get highest static priority among waiters / 获取等待者中最高静态优先级
 *
 * Used for priority inheritance calculation.
 * 用于优先级继承计算。
 */
static unsigned char TE_8R_8051_mutex_highest_wait_prio(TE_8R_8051_mutex_t xdata *m)
{
    unsigned char i;
    unsigned char highest = 0;

    for (i = 0; i < TE_8R_8051_MAX_TASKS; i++) {
        if (m->waiters[i / 8] & (unsigned char)(1 << (i % 8))) {
            unsigned char p = TE_8R_8051_PRIO_OF(tcb_hot[i].meta);
            if (p > highest) {
                highest = p;
            }
        }
    }
    return highest;
}

/**
 * @brief  Initialize mutex as unowned / 初始化互斥量为无人持有
 *
 * @param m Mutex pointer in xdata
 *
 * @pre Must be called after TE_8R_8051_Init().
 */
void TE_8R_8051_mutex_init(TE_8R_8051_mutex_t xdata *m)
{
    unsigned char i;
    m->owner = TE_8R_8051_MUTEX_NONE;
    m->orig_prio = 0;
    for (i = 0; i < TE_8R_8051_WAITER_WORDS; i++) {
        m->waiters[i] = 0;
    }
}

/**
 * @brief  Acquire mutex (may block) / 获取互斥量（可能阻塞）
 *
 * @param m Mutex pointer in xdata
 *
 * Three cases / 三种情况:
 * 1. Lock free → acquired immediately / 锁空闲→直接拿到
 * 2. Self-lock → detected, returns without blocking / 自锁→直接返回
 * 3. Held by other → priority inheritance + block / 被他人持有→继承+阻塞
 *
 * Priority inheritance: if the caller's static priority exceeds the
 * holder's, the holder is temporarily boosted to the caller's level.
 * This prevents medium-priority tasks from causing unbounded inversion.
 * 优先级继承：调用者优先级高于持有者时，持有者临时提升到调用者级别，
 * 防止中等优先级任务导致无界反转。
 *
 * @note Inheritance uses static priority only, not effective (aging).
 *       继承仅看静态优先级，不看饥饿老化。
 *
 * @warning Task context only, never from ISR.
 */
void TE_8R_8051_mutex_acquire(TE_8R_8051_mutex_t xdata *m)
{
    TE_8R_8051_CRITICAL_VAR();

    TE_8R_8051_ENTER_CRITICAL();

    /* Case 1: lock free / 锁空闲 */
    if (m->owner == TE_8R_8051_MUTEX_NONE) {
        m->owner = current_task;
        m->orig_prio = TE_8R_8051_PRIO_OF(tcb_hot[current_task].meta);
        TE_8R_8051_EXIT_CRITICAL();
        return;
    }

    /* Case 2: self-lock / 自锁检测 */
    if (m->owner == current_task) {
        TE_8R_8051_EXIT_CRITICAL();
        return;
    }

    /* Case 3: held by other — inheritance + block / 被他人持有 */

    /* Priority inheritance / 优先级继承 */
    {
        unsigned char my_prio = TE_8R_8051_PRIO_OF(tcb_hot[current_task].meta);
        unsigned char owner_prio = TE_8R_8051_PRIO_OF(tcb_hot[m->owner].meta);

        if (my_prio > owner_prio) {
            TE_8R_8051_SET_PRIO(tcb_hot[m->owner].meta, my_prio);
        }
    }

    /* Register in waiter bitmap / 登记等待位图 */
    m->waiters[current_task / 8] |= (unsigned char)(1 << (current_task % 8));

    /* Block: set BLOCKED state, clear starve / 阻塞：设阻塞态，清饥饿 */
    tcb_cold[current_task].starve = 0;
    tcb_cold[current_task].delay = TE_8R_8051_TASK_BLOCKED;

    TE_8R_8051_EXIT_CRITICAL();
    TE_8R_8051_task_switch();
}

/**
 * @brief  Release mutex / 释放互斥量
 *
 * @param m Mutex pointer in xdata
 * @return 1=success, 0=caller is not the owner
 *
 * Steps / 步骤:
 * 1. Restore owner's original priority / 恢复持有者原始优先级
 * 2. If waiters exist: transfer lock to highest-priority waiter, wake it
 *    / 有等待者：转锁给最高优先级者并唤醒
 * 3. If remaining waiters: apply inheritance to new owner
 *    / 仍有等待者：对新持有者做继承
 * 4. If woken task priority > current: reschedule immediately
 *    / 唤醒任务优先级更高：立刻让出CPU
 *
 * @warning Only the owner may release.
 */
unsigned char TE_8R_8051_mutex_release(TE_8R_8051_mutex_t xdata *m)
{
    TE_8R_8051_CRITICAL_VAR();
    unsigned char do_resched = 0;

    TE_8R_8051_ENTER_CRITICAL();

    if (m->owner != current_task) {
        TE_8R_8051_EXIT_CRITICAL();
        return 0;
    }

    /* Step 1: restore original priority / 恢复原始优先级 */
    TE_8R_8051_SET_PRIO(tcb_hot[current_task].meta, m->orig_prio);

    /* Step 2: select highest-priority waiter / 选最高优先级等待者 */
    {
        unsigned char new_owner = TE_8R_8051_mutex_best_waiter(m);
        unsigned char new_prio;

        if (new_owner == TE_8R_8051_MUTEX_NONE) {
            m->owner = TE_8R_8051_MUTEX_NONE;
            TE_8R_8051_EXIT_CRITICAL();
            return 1;
        }

        /* Transfer lock / 转锁 */
        m->waiters[new_owner / 8] &= (unsigned char)~(1 << (new_owner % 8));
        m->owner = new_owner;
        m->orig_prio = TE_8R_8051_PRIO_OF(tcb_hot[new_owner].meta);

        /* Wake new owner / 唤醒新持有者 */
        tcb_cold[new_owner].starve = 0;
        tcb_cold[new_owner].delay = 0;

        /* Step 3: inheritance for remaining waiters / 对剩余等待者做继承 */
        {
            unsigned char w, has_waiters = 0;
            for (w = 0; w < TE_8R_8051_WAITER_WORDS; w++) {
                if (m->waiters[w] != 0) { has_waiters = 1; break; }
            }
            if (has_waiters) {
                new_prio = TE_8R_8051_mutex_highest_wait_prio(m);
                if (new_prio > m->orig_prio) {
                    TE_8R_8051_SET_PRIO(tcb_hot[new_owner].meta, new_prio);
                }
            }
        }

        /* Step 4: reschedule if woken task has higher priority / 重调度判定 */
        {
            unsigned char cur_prio = TE_8R_8051_PRIO_OF(tcb_hot[current_task].meta);
            unsigned char wake_prio = TE_8R_8051_PRIO_OF(tcb_hot[new_owner].meta);
            if (wake_prio > cur_prio) {
                do_resched = 1;
            }
        }
    }

    TE_8R_8051_EXIT_CRITICAL();

    if (do_resched) {
        TE_8R_8051_task_switch();
    }

    return 1;
}

/**
 * @brief  Try to acquire mutex without blocking / 尝试获取互斥量（不阻塞）
 *
 * @param m Mutex pointer in xdata
 * @return 1=acquired, 0=lock held by another task
 *
 * Non-blocking variant of TE_8R_8051_mutex_acquire.
 * acquire的非阻塞变体。
 */
unsigned char TE_8R_8051_mutex_try_acquire(TE_8R_8051_mutex_t xdata *m)
{
    TE_8R_8051_CRITICAL_VAR();

    TE_8R_8051_ENTER_CRITICAL();

    if (m->owner == TE_8R_8051_MUTEX_NONE) {
        m->owner = current_task;
        m->orig_prio = TE_8R_8051_PRIO_OF(tcb_hot[current_task].meta);
        TE_8R_8051_EXIT_CRITICAL();
        return 1;
    }

    TE_8R_8051_EXIT_CRITICAL();
    return 0;
}