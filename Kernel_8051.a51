;======================================================================
; @file Kernel_8051.a51
; @brief Assembly core: context switch, ISR, stack overflow detection
;        汇编核心：上下文切换、中断入口、栈溢出检测
;
; Three pillars / 三大支柱:
;   1. TE_8R_8051_start       : kernel ignition, resume first task / 内核点火
;   2. TE_8R_8051_task_switch : cooperative task switch / 协作式任务切换
;   3. Timer0_Isr             : tick ISR, preemptive switch source / 心跳中断
;
; @note 8051 stack: PUSH increments SP then writes; POP reads then decrements.
;       Stack grows upward. SP always points to the last pushed byte.
;       8051栈：PUSH先SP+1再写入，POP先读出再SP-1，栈向高地址生长。
;======================================================================

; ------------------------------------------------------------------
; Assembly-side constants (synchronized with TE_8R_8051_config.h)
; 汇编侧常量（与config.h保持同步）
; ------------------------------------------------------------------
$INCLUDE (TE_8R_8051_CONFIG.INC)

; ------------------------------------------------------------------
; Interrupt vector: Timer 0 overflow → 0x000B
; 中断向量：定时器0溢出→0x000B
; ------------------------------------------------------------------
CSEG AT 0000BH
LJMP Timer0_Isr

; ------------------------------------------------------------------
; Segment and symbol declarations / 段与符号声明
; ------------------------------------------------------------------
?PR?TE_8R_8051_CORE?TE_8R_8051_CORE SEGMENT CODE
RSEG ?PR?TE_8R_8051_CORE?TE_8R_8051_CORE

EXTRN DATA (TCB_HOT)
EXTRN DATA (CURRENT_TASK)
EXTRN IDATA (TE_8R_8051_WORKSPACE)
EXTRN CODE (TE_8R_8051_SCHEDULE)
EXTRN CODE (TE_8R_8051_TICK_HANDLER)

; Stack overflow diagnostics (data, MOV-writable) / 栈溢出诊断变量
EXTRN DATA (TE_8R_8051_STACK_OVERFLOW_FLAG)
EXTRN DATA (TE_8R_8051_STACK_OVERFLOW_MAGNITUDE)
EXTRN DATA (TE_8R_8051_STACK_OVERFLOW_ID)
EXTRN DATA (TE_8R_8051_STACK_OVERFLOW_SP)
EXTRN DATA (TE_8R_8051_STACK_OVERFLOW_TASK_SP)
EXTRN DATA (TE_8R_8051_STACK_OVERFLOW_GUARD_OFFSET)

PUBLIC TE_8R_8051_start
PUBLIC TE_8R_8051_task_switch
PUBLIC Timer0_Isr

;======================================================================
; TE_8R_8051_start — kernel ignition, never returns
; 内核点火，永不返回
;
; Steps / 步骤:
;   0. Restore task 0's stack from xdata to idata workspace
;      从xdata搬任务0的栈回idata工作区
;   1. Load task 0's SP from tcb_hot[0].sp
;      从tcb_hot[0].sp装载SP
;   2. Pop 13 registers in reverse push order
;      按压栈逆序弹13个寄存器
;   3. Enable global interrupts (EA=1)
;      开总中断
;   4. RETI pops fabricated PC → task 0 begins
;      RETI弹出伪造PC→任务0开始运行
;======================================================================
TE_8R_8051_start:
    ; ---- Step 0: restore task 0 stack from xdata to idata ----
    ; 搬运task 0的栈从xdata到idata工作区
    ; Byte count = stack_guard + 1 (includes sentinel)
    ; 搬运字节数 = stack_guard + 1（含哨兵）
    MOV R0, #TCB_HOT
    INC R0
    INC R0
    INC R0
    INC R0                ; R0 = &tcb_hot[0].stack_guard
    MOV A, @R0
    INC  A                ; A = byte count
    MOV R1, A             ; R1 = loop counter

    ; Load xdata source address from TCB
    ; 从TCB装载xdata栈基址
    MOV R0, #TCB_HOT
    INC R0
    INC R0                ; R0 = &tcb_hot[0].xstack_dpl
    MOV DPL, @R0
    INC R0                ; R0 = &tcb_hot[0].xstack_dph
    MOV DPH, @R0

    MOV R0, #TE_8R_8051_WORKSPACE

TE_8R_8051_start_restore_loop:
    MOVX A, @DPTR
    MOV @R0, A
    INC R0
    INC DPTR
    DJNZ R1, TE_8R_8051_start_restore_loop

    ; ---- Step 1: load task 0 SP ----
    ; 装载任务0的栈顶指针
    MOV R0, #TCB_HOT
    MOV A, @R0            ; A = tcb_hot[0].sp (offset)
    ADD A, #TE_8R_8051_WORKSPACE
    MOV SP, A

    ; ---- Step 2: pop task 0 registers ----
    ; 弹出任务0的寄存器现场（顺序与压栈严格互逆）
    POP 0x07              ; R7
    POP 0x06              ; R6
    POP 0x05              ; R5
    POP 0x04              ; R4
    POP 0x03              ; R3
    POP 0x02              ; R2
    POP 0x01              ; R1
    POP 0x00              ; R0
    POP DPL
    POP DPH
    POP PSW
    POP B
    POP ACC

    ; ---- Step 3: enable interrupts ----
    ; 开总中断，点燃心跳
    SETB EA

    ; ---- Step 4: RETI into task 0 ----
    ; RETI弹出伪造PC，任务0开始运行
    RETI

;======================================================================
; TE_8R_8051_task_switch — cooperative task switch
; 协作式任务切换（Delay/yield触发）
;
; Sequence / 序列:
;   (1) Save all registers to current task stack / 保存现场
;   (2) Record SP as offset in tcb_hot[current].sp / 记录断点
;   (3) Save stack: idata → xdata / 搬栈保存
;   (4) Schedule next task / 调度
;   (5) Restore stack: xdata → idata / 搬栈恢复
;   (6) Load next task SP / 装载SP
;   (7) Pop registers / 弹现场
;   (8) RETI / 复活
;======================================================================
TE_8R_8051_task_switch:
    CLR EA                ; Disable interrupts during save / 保存期间关中断

    ; ════════════════════════════════════════════════════════
    ; L1: pre-switch boundary check / 切换入口边界检查
    ; Condition: (SP_now - WORKSPACE) - (stack_guard - 11) >= 0 → overflow
    ; 11 = ISR_FRAME_SIZE(17) - 4(temp PUSH) = 13(regs) - 4(temp) + 2(LCALL ret)
    ; ════════════════════════════════════════════════════════
    PUSH ACC
    PUSH B
    PUSH 0x00
    PUSH PSW

    MOV  A, CURRENT_TASK
    MOV  B, #05H
    MUL  AB                      ; A = CURRENT_TASK * sizeof(tcb_hot_t)
    ADD  A, #TCB_HOT
    ADD  A, #4                   ; A = &tcb_hot[current_task].stack_guard
    MOV  R0, A
    MOV  A, @R0                  ; A = stack_guard

    CLR  C
; ---- ISR_FRAME_SIZE linkage point / ISR_FRAME_SIZE联动点 ----
; 11 = ISR_FRAME_SIZE(17) - 4. Must sync with TE_8R_8051.h.
; 此常数须与TE_8R_8051.h中的ISR_FRAME_SIZE/MIN_STACK_SIZE同步修改。
    SUBB A, #0BH                 ; A = stack_guard - 11

    MOV  B, A
    MOV  A, SP
    CLR  C
    SUBB A, #TE_8R_8051_WORKSPACE

    CLR  C
    SUBB A, B
    JC   l1_pass_active         ; <0 → no overflow
    LJMP l1_overflow            ; >=0 → overflow!
l1_pass_active:
    POP  PSW
    POP  0x00
    POP  B
    POP  ACC
    ; ════════════════════════════════════════════════════════

    ; ---- Save current task registers / 保存当前任务全部现场 ----
    ; Push order = reverse of pop order in TE_8R_8051_start
    ; 压栈顺序与start弹栈顺序严格互逆
    PUSH ACC
    PUSH B
    PUSH PSW
    PUSH DPH
    PUSH DPL
    PUSH 0x00
    PUSH 0x01
    PUSH 0x02
    PUSH 0x03
    PUSH 0x04
    PUSH 0x05
    PUSH 0x06
    PUSH 0x07

; ---- Merge point: ISR path joins here / 汇合点：中断路径与此处对接 ----
; Both paths (cooperative / preemptive) share the same switch logic.
; 两种触发方式共用同一套切换逻辑，省ROM且行为一致。
TE_8R_8051_task_switch_save:
    ; ---- Record SP as offset / 记录断点SP（偏移量语义）----
    ; tcb_hot[current_task].sp = SP - WORKSPACE_BASE
    ; 存偏移量而非绝对地址：偏移量=栈深度，可与哨兵偏移量比大小
    MOV A, CURRENT_TASK
    MOV B, #05H
    MUL AB                               ; A = current_task * 5
    ADD A, #TCB_HOT
    MOV R0, A                            ; R0 -> &tcb_hot[current_task]
    MOV A, SP
    CLR C
    SUBB A, #TE_8R_8051_WORKSPACE        ; A = SP - workspace base = offset
    MOV @R0, A                           ; tcb_hot[current_task].sp = offset

    ; ---- Save stack: idata → xdata / 搬运当前任务栈（保存方向）----
    ; Byte count = sp + 1 (offset is 0-based)
    ; 搬运字节数 = sp + 1（偏移量从0开始）
    MOV A, CURRENT_TASK
    MOV B, #05H
    MUL AB
    ADD A, #TCB_HOT
    MOV R0, A
    MOV A, @R0            ; A = tcb_hot[current_task].sp
    INC A                 ; A = byte count
    MOV R1, A             ; R1 = loop counter

    ; Load xdata destination from TCB
    ; 从TCB装载xdata目标首地址
    MOV A, CURRENT_TASK
    MOV B, #05H
    MUL AB
    ADD A, #TCB_HOT
    MOV R0, A
    INC R0
    INC R0                ; R0 = &tcb_hot[current_task].xstack_dpl
    MOV DPL, @R0
    INC R0                ; R0 = &tcb_hot[current_task].xstack_dph
    MOV DPH, @R0

    MOV R0, #TE_8R_8051_WORKSPACE

save_loop:
    MOV A, @R0
    MOVX @DPTR, A
    INC R0
    INC DPTR
    DJNZ R1, save_loop

    ; ---- Schedule next task / 调度选下一个任务 ----
    LCALL TE_8R_8051_SCHEDULE

    ; ---- Overflow shutdown check / 栈溢出停机检查 ----
    ; If flag set: TR0=0, ET0=0, EA=0 → system halted
    ; 三道防线全拉下，系统完全静止
    MOV  A, TE_8R_8051_STACK_OVERFLOW_FLAG
    JNZ  overflow_shutdown

    ; ════════════════════════════════════════════════════════
    ; L2: canary check on next task / next任务金丝校验
    ; Read sentinel from next task's xdata stack
    ; ════════════════════════════════════════════════════════
    MOV  A, CURRENT_TASK
    MOV  B, #05H
    MUL  AB
    ADD  A, #TCB_HOT
    MOV  R0, A                   ; R0 -> &tcb_hot[next_task]

    INC  R0                      ; R0 -> &meta
    INC  R0                      ; R0 -> &xstack_dpl
    MOV  DPL, @R0
    INC  R0                      ; R0 -> &xstack_dph
    MOV  DPH, @R0                ; DPTR = next task xdata base

    INC  R0                      ; R0 -> &stack_guard
    MOV  A, @R0                  ; A = stack_guard

    ADD  A, DPL
    MOV  DPL, A
    CLR  A
    ADDC A, DPH
    MOV  DPH, A                  ; DPTR = xdata base + stack_guard

    MOVX A, @DPTR                ; A = sentinel value
    CJNE A, #TE_8R_8051_STACK_MAGIC, l2_overflow  ; != 0xFE → overflow!
    SJMP switch_restore          ; Canary intact, proceed / 金丝完好

l2_overflow:
    ; ════════════════════════════════════════════════════════
    ; L2 overflow entry: record diagnostics → halt → idle
    ; L2事故入口：记录诊断→三道防线→短路空闲任务
    ; ════════════════════════════════════════════════════════
    MOV  A, CURRENT_TASK
    MOV  TE_8R_8051_STACK_OVERFLOW_ID, A
    MOV  TE_8R_8051_STACK_OVERFLOW_MAGNITUDE, #0
    MOV  TE_8R_8051_STACK_OVERFLOW_SP, SP
    MOV  A, CURRENT_TASK
    MOV  B, #05H
    MUL  AB
    ADD  A, #TCB_HOT
    MOV  R0, A
    MOV  A, @R0                  ; tcb_hot[next_task].sp
    MOV  TE_8R_8051_STACK_OVERFLOW_TASK_SP, A
    INC  R0
    INC  R0
    INC  R0
    INC  R0                      ; R0 -> &stack_guard
    MOV  A, @R0
    MOV  TE_8R_8051_STACK_OVERFLOW_GUARD_OFFSET, A
    MOV  TE_8R_8051_STACK_OVERFLOW_FLAG, #1
    CLR  TR0
    CLR  ET0
    CLR  EA
    LJMP l1_switch_to_idle
    ; ════════════════════════════════════════════════════════

overflow_shutdown:
    CLR TR0               ; Stop timer 0 / 停心跳源
    CLR ET0               ; Disable timer 0 interrupt / 关心跳中断使能
    CLR EA                ; Disable all interrupts / 关总中断

switch_restore:
    ; ---- Restore stack: xdata → idata / 搬运下一任务栈（恢复方向）----
    ; Byte count = stack_guard + 1 (includes sentinel)
    ; 搬运字节数 = stack_guard + 1（含哨兵的整个栈区）
    MOV A, CURRENT_TASK
    MOV B, #05H
    MUL AB
    ADD A, #TCB_HOT
    MOV R0, A
    INC R0
    INC R0
    INC R0
    INC R0                ; R0 = &tcb_hot[next_task].stack_guard
    MOV A, @R0            ; A = stack_guard
    INC A                 ; A = byte count
    MOV R1, A             ; R1 = loop counter

    ; Load xdata source from TCB
    ; 从TCB装载xdata源首地址
    MOV A, CURRENT_TASK
    MOV B, #05H
    MUL AB
    ADD A, #TCB_HOT
    MOV R0, A
    INC R0
    INC R0                ; R0 = &tcb_hot[next_task].xstack_dpl
    MOV DPL, @R0
    INC R0                ; R0 = &tcb_hot[next_task].xstack_dph
    MOV DPH, @R0

    MOV R0, #TE_8R_8051_WORKSPACE

restore_loop:
    MOVX A, @DPTR
    MOV @R0, A
    INC R0
    INC DPTR
    DJNZ R1, restore_loop

    ; ---- Load next task SP / 装载新任务SP ----
    ; SP = WORKSPACE_BASE + tcb_hot[next_task].sp
    MOV A, CURRENT_TASK
    MOV B, #05H
    MUL AB
    ADD A, #TCB_HOT
    MOV R0, A
    MOV A, @R0            ; A = tcb_hot[next_task].sp (offset)
    ADD A, #TE_8R_8051_WORKSPACE
    MOV SP, A

    ; ---- Pop next task registers / 弹出新任务现场 ----
    ; Order must be strict reverse of push order
    ; 弹栈顺序必须和压栈顺序严格互逆
    POP 0x07              ; R7
    POP 0x06              ; R6
    POP 0x05              ; R5
    POP 0x04              ; R4
    POP 0x03              ; R3
    POP 0x02              ; R2
    POP 0x01              ; R1
    POP 0x00              ; R0
    POP DPL
    POP DPH
    POP PSW
    POP B
    POP ACC

    ; ---- Enable interrupts (if no overflow) / 开中断（无溢出时）----
    MOV  A, TE_8R_8051_STACK_OVERFLOW_FLAG
    JNZ  overflow_retire
    SETB EA
overflow_retire:
    RETI                  ; Pop PC, task resumes / 弹出PC，任务复活

;======================================================================
; Timer0_Isr — system tick (preemptive switch engine)
; 系统心跳（抢占式调度引擎）
;
; Steps / 步骤:
;   (1) Save interrupted task's registers / 保存被打断任务现场
;   (2) LCALL TE_8R_8051_Tick_Handler: advance time + sentinel check
;       调Tick_Handler：推进时间+哨兵抽检
;   (3) LJMP TE_8R_8051_task_switch_save: merge into switch logic
;       跳入汇合点，进行调度切换
;
; @note No RETI here — borrows the RETI in task_switch_save.
;       本ISR没有自己的RETI，借用汇合点的RETI收尾。
;======================================================================
Timer0_Isr:
    ; ════════════════════════════════════════════════════════
    ; L1: pre-switch boundary check / 切换入口边界检查
    ; Same formula as TE_8R_8051_task_switch L1
    ; ════════════════════════════════════════════════════════
    PUSH ACC
    PUSH B
    PUSH 0x00
    PUSH PSW

    MOV  A, CURRENT_TASK
    MOV  B, #05H
    MUL  AB
    ADD  A, #TCB_HOT
    ADD  A, #4
    MOV  R0, A
    MOV  A, @R0

    CLR  C
; ---- ISR_FRAME_SIZE linkage point / ISR_FRAME_SIZE联动点 ----
; Same as TE_8R_8051_task_switch. Must stay in sync.
; 与task_switch中的联动点保持同步。
    SUBB A, #0BH

    MOV  B, A
    MOV  A, SP
    CLR  C
    SUBB A, #TE_8R_8051_WORKSPACE

    CLR  C
    SUBB A, B
    JC   l1_pass_isr
    LJMP l1_overflow
l1_pass_isr:
    POP  PSW
    POP  0x00
    POP  B
    POP  ACC
    ; ════════════════════════════════════════════════════════

    ; ---- Save interrupted task registers / 保存被打断任务现场 ----
    ; Push order must match TE_8R_8051_task_switch (merge requirement)
    ; 压栈顺序必须与task_switch一致（汇流前提）
    PUSH ACC
    PUSH B
    PUSH PSW
    PUSH DPH
    PUSH DPL
    PUSH 0x00
    PUSH 0x01
    PUSH 0x02
    PUSH 0x03
    PUSH 0x04
    PUSH 0x05
    PUSH 0x06
    PUSH 0x07

    ; ---- Advance time / 推进时间 ----
    LCALL TE_8R_8051_TICK_HANDLER

    ; ---- Merge into switch logic / 汇入切换主航道 ----
    ; LJMP (not LCALL): no return address pushed, clean merge
    ; LJMP而非LCALL：不压返回地址，干净汇合
    LJMP TE_8R_8051_task_switch_save

;======================================================================
; L1 overflow entry: record diagnostics → halt → idle
; L1事故入口：记录诊断→三道防线→短路空闲任务
;======================================================================
l1_overflow:
    MOV  TE_8R_8051_STACK_OVERFLOW_MAGNITUDE, A

    MOV  A, CURRENT_TASK
    MOV  TE_8R_8051_STACK_OVERFLOW_ID, A

    MOV  A, SP
    CLR  C
    SUBB A, #04H
    MOV  TE_8R_8051_STACK_OVERFLOW_SP, A

    MOV  A, SP
    CLR  C
    SUBB A, #TE_8R_8051_WORKSPACE
    SUBB A, #04H
    MOV  TE_8R_8051_STACK_OVERFLOW_TASK_SP, A

    MOV  A, CURRENT_TASK
    MOV  B, #05H
    MUL  AB
    ADD  A, #TCB_HOT
    ADD  A, #4
    MOV  R0, A
    MOV  A, @R0
    MOV  TE_8R_8051_STACK_OVERFLOW_GUARD_OFFSET, A

    MOV  TE_8R_8051_STACK_OVERFLOW_FLAG, #1

    CLR  TR0
    CLR  ET0
    CLR  EA

    LJMP l1_switch_to_idle

;======================================================================
; Short-circuit to idle: set CURRENT_TASK = SLOT_MAX, reuse switch_restore
; 短路至空闲任务：设CURRENT_TASK为空闲索引，复用switch_restore
;======================================================================
l1_switch_to_idle:
    MOV  A, #TE_8R_8051_SLOT_MAX
    MOV  CURRENT_TASK, A
    LJMP switch_restore

END