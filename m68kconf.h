/*
 * Musashi configuration for the SCSI Plug harness.
 * Overrides the default m68kconf.h via -DMUSASHI_CNF="\"m68kconf.h\""
 */
#ifndef PLUG_M68KCONF_H
#define PLUG_M68KCONF_H

#define M68K_OPT_OFF             0
#define M68K_OPT_ON              1
#define M68K_OPT_SPECIFY_HANDLER 2

#define M68K_COMPILE_FOR_MAME    M68K_OPT_OFF

/* Enable only 68040 (Mac OS 9 era) */
#define M68K_EMULATE_010         M68K_OPT_OFF
#define M68K_EMULATE_EC020       M68K_OPT_OFF
#define M68K_EMULATE_020         M68K_OPT_ON
#define M68K_EMULATE_030         M68K_OPT_ON
#define M68K_EMULATE_040         M68K_OPT_ON

#define M68K_SEPARATE_READS      M68K_OPT_OFF
#define M68K_SIMULATE_PD_WRITES  M68K_OPT_OFF
#define M68K_EMULATE_INT_ACK     M68K_OPT_OFF
#define M68K_EMULATE_BKPT_ACK    M68K_OPT_OFF
#define M68K_EMULATE_TRACE       M68K_OPT_OFF
#define M68K_EMULATE_RESET       M68K_OPT_OFF
#define M68K_CMPILD_HAS_CALLBACK M68K_OPT_OFF
#define M68K_RTE_HAS_CALLBACK    M68K_OPT_OFF
#define M68K_TAS_HAS_CALLBACK    M68K_OPT_OFF
#define M68K_EMULATE_FC          M68K_OPT_OFF
#define M68K_MONITOR_PC          M68K_OPT_OFF
#define M68K_EMULATE_PREFETCH    M68K_OPT_OFF
#define M68K_EMULATE_ADDRESS_ERROR M68K_OPT_OFF
#define M68K_EMULATE_PMMU        M68K_OPT_OFF
#define M68K_USE_64_BIT          M68K_OPT_ON

/* Instruction hook: fires before every instruction.
 * We use this to detect and handle A-line traps before the CPU processes them. */
#define M68K_INSTRUCTION_HOOK    M68K_OPT_SPECIFY_HANDLER
extern void plug_instruction_hook(unsigned int pc);
#define M68K_INSTRUCTION_CALLBACK(pc) plug_instruction_hook(pc)

/* Log 1010/1111 opcodes for debugging */
#define M68K_LOG_ENABLE          M68K_OPT_OFF
#define M68K_LOG_1010_1111       M68K_OPT_OFF
#define M68K_LOG_TRAP            M68K_OPT_OFF

/* Illegal instruction callback: catches anything Musashi doesn't handle */
#define M68K_ILLG_HAS_CALLBACK   M68K_OPT_SPECIFY_HANDLER
extern int plug_illg_handler(int opcode);
#define M68K_ILLG_CALLBACK(opcode) plug_illg_handler(opcode)

/* TRAP instruction callback: for any explicit TRAP #n in the code */
#define M68K_TRAP_HAS_CALLBACK   M68K_OPT_OFF

#endif /* PLUG_M68KCONF_H */
