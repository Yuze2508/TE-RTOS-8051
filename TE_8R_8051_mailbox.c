/**
 * @file TE_8R_8051_mailbox.c
 * @brief Mailbox: inter-task message passing / 邮箱：任务间消息传递
 *
 * Mailbox semantics: latest value wins. Unread mail is overwritten.
 * Not a queue — 8051 RAM is too scarce for queuing.
 * 邮箱语义：最新值有效，旧信未读则被覆盖，非队列。
 */

#include "TE_8R_8051_internal.h"
#include <string.h>

/**
 * @brief  Send mail and wake target task / 发送邮件并唤醒目标任务
 *
 * @param id    Target task ID
 * @param dat_p Data pointer in xdata
 * @param len   Data length (1 ~ MAILBOX_CAPACITY)
 * @return      1=success, 0=invalid params
 *
 * @note Data is written first, then mail length (ring the bell).
 *       If length is set before data completes, the reader sees a half-written message.
 *       先写数据，最后写信长（摇铃），防止读到半成品。
 *
 * @note Blocked tasks (waiting for mutex) are not woken by mail —
 *       they are waiting for a lock, not a message.
 *       阻塞态任务不被邮件唤醒——它等的是锁，不是信。
 *
 * @warning Must not be called from ISR. Use TE_8R_8051_Post_Mailbox instead.
 *          禁止在ISR中调用，请用Post_Mailbox。
 */
unsigned char TE_8R_8051_Send_Mailbox(unsigned char id, unsigned char xdata *dat_p, unsigned char len)
{
	TE_8R_8051_CRITICAL_VAR();
	
    if (id >= TE_8R_8051_MAX_TASKS || len == 0 || len > TE_8R_8051_MAILBOX_CAPACITY)
        return 0;

    TE_8R_8051_ENTER_CRITICAL();
    memcpy(TE_8R_8051_task_mailbox[id], dat_p, len);
    TE_8R_8051_SET_LEN(tcb_hot[id].meta, len);
    if (!TE_8R_8051_IS_TOMBSTONE(id) && !TE_8R_8051_IS_BLOCKED(id)) {
        tcb_cold[id].delay = 0;
    }
    TE_8R_8051_EXIT_CRITICAL();
    return 1;
}

/**
 * @brief  Post mail silently (no wake) / 静默投递邮件（不唤醒）
 *
 * @param id    Target task ID
 * @param dat_p Data pointer in xdata
 * @param len   Data length
 * @return      1=success, 0=invalid params
 *
 * Use in ISR (cannot trigger scheduling) or for pre-filling a mailbox
 * before the target task starts.
 * 适用于ISR投递或开机预填邮箱。
 */
unsigned char TE_8R_8051_Post_Mailbox(unsigned char id, unsigned char xdata *dat_p, unsigned char len)
{
	TE_8R_8051_CRITICAL_VAR();
	
    if (id >= TE_8R_8051_MAX_TASKS || len == 0 || len > TE_8R_8051_MAILBOX_CAPACITY)
        return 0;

    TE_8R_8051_ENTER_CRITICAL();
    memcpy(TE_8R_8051_task_mailbox[id], dat_p, len);
    TE_8R_8051_SET_LEN(tcb_hot[id].meta, len);
    TE_8R_8051_EXIT_CRITICAL();
    return 1;
}

/**
 * @brief  Read own mailbox / 读取本任务邮箱
 *
 * @param dat_p  Receive buffer in xdata
 * @param buflen Buffer capacity
 * @return       Bytes read; 0 = no mail or buffer too small
 *
 * @note If buflen < mail length, the mail is not consumed.
 *       缓冲区太小则不消费邮件，换大缓冲区重读即可。
 */
unsigned char TE_8R_8051_Read_Mailbox(unsigned char xdata *dat_p, unsigned char buflen)
{
    unsigned char n;
	TE_8R_8051_CRITICAL_VAR();

    if (TE_8R_8051_LEN_OF(tcb_hot[current_task].meta) == 0)
        return 0;

    TE_8R_8051_ENTER_CRITICAL();
    n = TE_8R_8051_LEN_OF(tcb_hot[current_task].meta);
    if (n > buflen) {
        TE_8R_8051_EXIT_CRITICAL();
        return 0;
    }
    memcpy(dat_p, TE_8R_8051_task_mailbox[current_task], n);
    TE_8R_8051_SET_LEN(tcb_hot[current_task].meta, 0);
    TE_8R_8051_EXIT_CRITICAL();
    return n;
}