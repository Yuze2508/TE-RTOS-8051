/**
 * @file TE_8R_8051_timer.c
 * @brief Software timer API / 软件定时器 API
 *
 * All functions are O(1) — direct index access, no traversal.
 * 所有函数O(1)——直接索引访问，不遍历。
 *
 * @note Concurrency with tick_advance(): timer_expired uses critical
 *       sections for flags. start/stop/reset may have up to 1-tick
 *       jitter on remaining (acceptable for software timers).
 *       与tick_advance的并发：expired用临界区保护flags；
 *       start/stop/reset对remaining最多1个Tick抖动，可接受。
 */

#include "TE_8R_8051_internal.h"

#if TE_8R_8051_MAX_TIMERS > 0

/**
 * @brief  Start a software timer / 启动软件定时器
 *
 * @param id      Timer ID (0 ~ MAX_TIMERS-1)
 * @param period  Period in ticks (1~65535; 0 clamped to 1)
 * @param mode    ONE_SHOT or PERIODIC
 */
void TE_8R_8051_timer_start(unsigned char id, unsigned int period, unsigned char mode)
{
    if (id >= TE_8R_8051_MAX_TIMERS) return;
    if (period == 0) period = 1;

    TE_8R_8051_timers[id].period = period;
    TE_8R_8051_timers[id].remaining = period;
    TE_8R_8051_timers[id].flags = TE_8R_8051_TIMER_ENABLED
                                | (mode ? TE_8R_8051_TIMER_MODE : 0);
}

/**
 * @brief  Stop a software timer / 停止软件定时器
 *
 * @param id Timer ID
 *
 * Remaining and flags are preserved (cleared by reset).
 * remaining和flags保留（由reset清零）。
 */
void TE_8R_8051_timer_stop(unsigned char id)
{
    if (id >= TE_8R_8051_MAX_TIMERS) return;
    TE_8R_8051_timers[id].flags &= ~TE_8R_8051_TIMER_ENABLED;
}

/**
 * @brief  Check if timer expired (read-clear) / 查询定时器是否到期（读后自动清标志）
 *
 * @param id Timer ID
 * @return   1=expired (flag cleared), 0=not expired
 */
unsigned char TE_8R_8051_timer_expired(unsigned char id)
{
    TE_8R_8051_CRITICAL_VAR();
    if (id >= TE_8R_8051_MAX_TIMERS) return 0;
    TE_8R_8051_ENTER_CRITICAL();
    if (TE_8R_8051_timers[id].flags & TE_8R_8051_TIMER_EXPIRED) {
        TE_8R_8051_timers[id].flags &= ~TE_8R_8051_TIMER_EXPIRED;
        TE_8R_8051_EXIT_CRITICAL();
        return 1;
    }
    TE_8R_8051_EXIT_CRITICAL();
    return 0;
}

/**
 * @brief  Reset timer (reload remaining=period, clear expired flag) / 重置定时器
 *
 * @param id Timer ID
 *
 * Equivalent to stop + start with the same period and mode.
 * 等效于先stop再start（保持原period和mode）。
 */
void TE_8R_8051_timer_reset(unsigned char id)
{
    if (id >= TE_8R_8051_MAX_TIMERS) return;
    TE_8R_8051_timers[id].remaining = TE_8R_8051_timers[id].period;
    TE_8R_8051_timers[id].flags &= ~TE_8R_8051_TIMER_EXPIRED;
}

/**
 * @brief  Check if timer is running / 查询定时器是否正在运行
 *
 * @param id Timer ID
 * @return   1=running, 0=stopped
 */
unsigned char TE_8R_8051_timer_is_running(unsigned char id)
{
    if (id >= TE_8R_8051_MAX_TIMERS) return 0;
    return (TE_8R_8051_timers[id].flags & TE_8R_8051_TIMER_ENABLED) ? 1 : 0;
}

#endif