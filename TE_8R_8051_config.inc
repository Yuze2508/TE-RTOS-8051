;======================================================================
; @file TE_8R_8051_config.inc
; @brief Assembly-side constants, synchronized with TE_8R_8051_config.h
;        汇编侧常量，与 TE_8R_8051_config.h 保持同步
;
; @warning When changing TE_8R_8051_config.h, you MUST update this file!
;          改 config.h 时必须同步改本文件！
;======================================================================

TE_8R_8051_STACK_MAGIC  EQU 0FEH              ; Must match config.h / 须与config.h一致
TE_8R_8051_MAX_TASKS    EQU 4                 ; Must match config.h / 须与config.h一致
TE_8R_8051_SLOT_MAX     EQU TE_8R_8051_MAX_TASKS   ; Idle task index / 空闲任务索引