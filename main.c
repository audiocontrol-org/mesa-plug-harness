/*
 * plug-harness: Standalone 68k emulator for running the MESA II SCSI Plug
 *
 * Loads the SCSI Plug binary into a minimal Mac OS environment and observes
 * what SCSI commands it issues, without booting Mac OS 9.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "musashi/m68k.h"
#include "scsi_bridge.h"

/* ======================================================================== */
/* Memory layout                                                             */
/* ======================================================================== */
#define MEM_SIZE       0x2000000  /* 32MB flat address space */
#define PLUG_CODE_BASE 0x010000  /* Plug code loaded here */
#define PLUG_CODE_SIZE 0x008000  /* 32KB from plug_full.bin */
#define PLUG_DATA_BASE 0x020000  /* Plug data section (A4 points here) */
#define PLUG_DATA_SIZE 0x004000  /* 16KB zeroed */
#define UNIT_TABLE     0x030000  /* Mock unit table */
#define DEVICE_RECORD  0x040000  /* Mock device record */
#define SCSI_BUFFER    0x050000  /* SCSI data buffers */
#define STACK_TOP      0x100000  /* Stack grows down from here */
#define TRAP_STUBS     0x100000  /* Trap handler stubs (RTE) */
#define ALINE_HANDLER  0x100010  /* Line-A exception handler: just RTE */

/* Mac OS low memory globals */
#define LM_UTABLEBASE  0x011C    /* Pointer to unit table */
#define LM_UNITNTRYCNT 0x01D2    /* Number of unit table entries */
#define LM_OS_TRAPS    0x0400    /* OS trap table (256 × 4 bytes) */
#define LM_TB_TRAPS    0x0E00    /* Toolbox trap table (1024 × 4 bytes) */

/* Exception vectors */
#define VEC_LINEA      0x0028    /* Vector 10: Line-A exception */
#define VEC_LINEF      0x002C    /* Vector 11: Line-F exception */
#define VEC_RESET_SP   0x0000    /* Vector 0: Initial SP */
#define VEC_RESET_PC   0x0004    /* Vector 1: Initial PC */

/* Plug entry points (file offsets in mesa_scsi_plug.bin) */
#define PLUG_CONSTRUCTOR 0x06CC  /* MOVEM.L D3/A2, -(SP) — real constructor with A4-relative globals */
#define PLUG_ORCHESTRATOR 0x140A

static unsigned char *g_mem = NULL;
static int g_aline_active = 0;  /* Set when we're handling an A-line trap */
static int g_running = 1;
static int g_trace = 1;         /* Trace all instructions */
static int g_trap_count = 0;

/* ======================================================================== */
/* Memory read/write (Musashi callbacks)                                     */
/* ======================================================================== */
static int g_oob_count = 0;
static inline void oob_check(unsigned int addr, const char *op) {
    g_oob_count++;
    if (g_oob_count <= 5)
        fprintf(stderr, "  %s OOB: 0x%08x (count=%d)\n", op, addr, g_oob_count);
    if (g_oob_count > 20) {
        fprintf(stderr, "  HALT: too many OOB accesses (%d) — garbage data\n", g_oob_count);
        g_running = 0;
        m68k_end_timeslice();
    }
}
static inline unsigned int mem_read_byte(unsigned int addr) {
    if (addr >= MEM_SIZE) { oob_check(addr, "RD8"); return 0; }
    return g_mem[addr];
}

#define READ_BE16(base, addr) (((base)[addr] << 8) | (base)[(addr)+1])
#define READ_BE32(base, addr) (((base)[addr] << 24) | ((base)[(addr)+1] << 16) | \
                               ((base)[(addr)+2] << 8) | (base)[(addr)+3])
#define WRITE_BE16(base, addr, val) do { \
    (base)[addr] = ((val) >> 8) & 0xFF; \
    (base)[(addr)+1] = (val) & 0xFF; \
} while(0)
#define WRITE_BE32(base, addr, val) do { \
    (base)[addr] = ((val) >> 24) & 0xFF; \
    (base)[(addr)+1] = ((val) >> 16) & 0xFF; \
    (base)[(addr)+2] = ((val) >> 8) & 0xFF; \
    (base)[(addr)+3] = (val) & 0xFF; \
} while(0)

unsigned int m68k_read_memory_8(unsigned int addr)  { return mem_read_byte(addr); }
unsigned int m68k_read_memory_16(unsigned int addr) {
    if (addr + 1 >= MEM_SIZE) { oob_check(addr, "RD16"); return 0; }
    return READ_BE16(g_mem, addr);
}
unsigned int m68k_read_memory_32(unsigned int addr) {
    if (addr + 3 >= MEM_SIZE) { oob_check(addr, "RD32"); return 0; }
    return READ_BE32(g_mem, addr);
}

void m68k_write_memory_8(unsigned int addr, unsigned int val) {
    if (addr >= MEM_SIZE) { oob_check(addr, "WR8"); return; }
    g_mem[addr] = val & 0xFF;
}
void m68k_write_memory_16(unsigned int addr, unsigned int val) {
    if (addr + 1 >= MEM_SIZE) { oob_check(addr, "WR16"); return; }
    WRITE_BE16(g_mem, addr, val);
}
void m68k_write_memory_32(unsigned int addr, unsigned int val) {
    if (addr + 3 >= MEM_SIZE) { oob_check(addr, "WR32"); return; }
    WRITE_BE32(g_mem, addr, val);
}

/* Disassembler memory access */
unsigned int m68k_read_disassembler_8(unsigned int addr)  { return m68k_read_memory_8(addr); }
unsigned int m68k_read_disassembler_16(unsigned int addr) { return m68k_read_memory_16(addr); }
unsigned int m68k_read_disassembler_32(unsigned int addr) { return m68k_read_memory_32(addr); }

/* ======================================================================== */
/* A-line trap handler                                                       */
/* ======================================================================== */

/* Mac OS trap number decoding:
 * OS traps:      $A0xx (bit 11 = 0) → table at 0x0400, index = bits 0-7
 * Toolbox traps: $A8xx (bit 11 = 1) → table at 0x0E00, index = bits 0-9
 */
static const char* trap_name(unsigned int opcode) {
    switch (opcode) {
        case 0xA055: return "StripAddress";
        case 0xA089: return "SCSIDispatch";
        case 0xA198: return "_Unimplemented";
        case 0xA346: return "GetOSTrapAddress";
        case 0xA746: return "GetToolTrapAddress";
        case 0xA89F: return "SCSIAtomic";
        case 0xA9A0: return "GetResource";
        case 0xA002: return "_Read";
        case 0xA003: return "_Write";
        case 0xA004: return "_Control";
        case 0xA005: return "_Status";
        case 0xA01E: return "NewPtr";
        case 0xA023: return "DisposeHandle";
        case 0xA025: return "GetHandleSize";
        case 0xA029: return "HLock";
        case 0xA02A: return "HUnlock";
        case 0xA040: return "ResrvMem";
        case 0xA122: return "NewHandle";
        case 0xA11E: return "NewPtrClear";
        case 0xA162: return "PurgeSpace";
        case 0xA871: return "NewDialog";
        case 0xA975: return "ModalDialog";
        case 0xA976: return "GetNewDialog";
        case 0xA820: return "Get1NamedResource";
        case 0xA821: return "Get1Resource";
        case 0xA9A1: return "GetNamedResource";
        default: return NULL;
    }
}

static void handle_aline_trap(unsigned int opcode) {
    const char *name = trap_name(opcode);
    unsigned int d0 = m68k_get_reg(NULL, M68K_REG_D0);
    unsigned int a0 = m68k_get_reg(NULL, M68K_REG_A0);
    unsigned int a4 = m68k_get_reg(NULL, M68K_REG_A4);

    g_trap_count++;

    if (name)
        printf("TRAP #%d: $%04X (%s) D0=0x%08x A0=0x%08x\n", g_trap_count, opcode, name, d0, a0);
    else
        printf("TRAP #%d: $%04X D0=0x%08x A0=0x%08x\n", g_trap_count, opcode, d0, a0);

    switch (opcode) {
        case 0xA055:  /* StripAddress: return A0 unchanged (32-bit clean) */
            /* A0 already has the address, just return it */
            m68k_set_reg(M68K_REG_D0, 0);  /* noErr */
            break;

        case 0xA746:  /* GetToolTrapAddress: D0.w = trap number, returns address in A0 */
        {
            unsigned int trap_num = d0 & 0x03FF;
            unsigned int addr = LM_TB_TRAPS + trap_num * 4;
            unsigned int handler = m68k_read_memory_32(addr);
            printf("  GetToolTrapAddress($%04X) → 0x%08x\n", 0xA800 | trap_num, handler);
            m68k_set_reg(M68K_REG_A0, handler);
            break;
        }

        case 0xA346:  /* GetOSTrapAddress: D0.w = trap number, returns address in A0 */
        {
            unsigned int trap_num = d0 & 0x00FF;
            unsigned int addr = LM_OS_TRAPS + trap_num * 4;
            unsigned int handler = m68k_read_memory_32(addr);
            printf("  GetOSTrapAddress($%04X) → 0x%08x\n", 0xA000 | trap_num, handler);
            m68k_set_reg(M68K_REG_A0, handler);
            break;
        }

        case 0xA089:  /* SCSIDispatch */
        {
            printf("  *** SCSI DISPATCH *** A0=PB@0x%08x D0=%d\n", a0, d0);
            if (a0 && a0 + 0x50 < MEM_SIZE) {
                /* Parse SCSI Manager 4.3 SCSIExecIO PB */
                unsigned int func_code = g_mem[a0 + 8];
                unsigned int bus = g_mem[a0 + 12] & 0xFF;  /* scsiDevice.bus at PB+12 */
                unsigned int target = g_mem[a0 + 13] & 0xFF;
                unsigned int lun = g_mem[a0 + 14] & 0xFF;
                unsigned int flags = READ_BE32(g_mem, a0 + 20);
                unsigned int data_ptr = READ_BE32(g_mem, a0 + 28);
                unsigned int data_len = READ_BE32(g_mem, a0 + 32);
                int cdb_len = g_mem[a0 + 58] ? g_mem[a0 + 58] : 6;
                unsigned char cdb[16];
                for (int i = 0; i < cdb_len && i < 16; i++)
                    cdb[i] = g_mem[a0 + 42 + i];

                printf("  func=%d bus=%d target=%d lun=%d flags=0x%08x\n",
                    func_code, bus, target, lun, flags);
                printf("  CDB[%d]:", cdb_len);
                for (int i = 0; i < cdb_len; i++) printf(" %02x", cdb[i]);
                printf("\n  data_ptr=0x%08x data_len=%d\n", data_ptr, data_len);

                if (func_code == 1) {  /* SCSIExecIO — forward to scsi2pi */
                    int is_write = (flags & 0x80000000) != 0;
                    int is_read  = (flags & 0x40000000) != 0;

                    uint8_t *out_buf = NULL;
                    size_t out_len = 0;
                    uint8_t *in_buf = NULL;
                    size_t in_len = 0;

                    if (is_write && data_ptr && data_len > 0 && data_ptr + data_len <= MEM_SIZE) {
                        out_buf = &g_mem[data_ptr];
                        out_len = data_len;
                    }
                    if (is_read && data_ptr && data_len > 0) {
                        in_buf = (data_ptr + data_len <= MEM_SIZE) ? &g_mem[data_ptr] : NULL;
                        in_len = data_len;
                    }

                    printf("  → Forwarding to scsi2pi (target %d, %s, %d bytes)\n",
                        target, is_write ? "WRITE" : is_read ? "READ" : "NONE",
                        is_write ? (int)out_len : (int)in_len);

                    int status = scsi_bridge_exec(target, lun, cdb, cdb_len,
                        out_buf, out_len, in_buf, &in_len, 10);

                    printf("  ← SCSI status=%d, data_in=%zu bytes\n", status, in_len);
                    if (is_read && in_len > 0) {
                        printf("  ← Data:");
                        for (size_t i = 0; i < in_len && i < 64; i++)
                            printf(" %02x", in_buf ? in_buf[i] : 0);
                        if (in_len > 64) printf(" ...");
                        printf("\n");
                    }

                    /* Write result back to PB */
                    WRITE_BE16(g_mem, a0 + 10, (status >= 0) ? 0 : 0xFFEC); /* scsiResult */
                    WRITE_BE32(g_mem, a0 + 32, in_len); /* actual data length */
                } else {
                    printf("  (non-ExecIO func %d — returning noErr)\n", func_code);
                }
            }
            m68k_set_reg(M68K_REG_D0, 0);
            break;
        }

        case 0xA89F:  /* SCSIAtomic: just return its address (different from _Unimplemented) */
        {
            /* The Plug checks if GetToolTrapAddress($A89F) != GetOSTrapAddress($A198).
             * We just need the trap table entries to be different.
             * The actual call is just a check, not a real SCSI operation. */
            unsigned int trap_addr = LM_TB_TRAPS + 0x09F * 4;
            unsigned int handler = m68k_read_memory_32(trap_addr);
            printf("  SCSIAtomic → 0x%08x\n", handler);
            m68k_set_reg(M68K_REG_D0, 0);
            break;
        }

        case 0xA198:  /* _Unimplemented */
            printf("  _Unimplemented called (conditional init path)\n");
            m68k_set_reg(M68K_REG_D0, 0);
            break;

        case 0xA9A0:  /* GetResource(resType, resID) → Handle */
        {
            unsigned int sp = m68k_get_reg(NULL, M68K_REG_A7);
            unsigned int res_id   = m68k_read_memory_16(sp);
            unsigned int res_type = m68k_read_memory_32(sp + 2);
            char type_str[5] = {(res_type>>24)&0xFF, (res_type>>16)&0xFF, (res_type>>8)&0xFF, res_type&0xFF, 0};
            printf("  GetResource('%s', %d)\n", type_str, (int16_t)res_id);
            sp += 6;  /* Pop resType(4) + resID(2) */
            m68k_set_reg(M68K_REG_A7, sp);
            m68k_write_memory_32(sp, 0);  /* Result: NULL handle */
            break;
        }

        case 0xA9A1:  /* GetNamedResource(resType, name) → Handle */
        {
            unsigned int sp = m68k_get_reg(NULL, M68K_REG_A7);
            unsigned int name_ptr = m68k_read_memory_32(sp);
            unsigned int res_type = m68k_read_memory_32(sp + 4);
            char type_str[5] = {(res_type>>24)&0xFF, (res_type>>16)&0xFF, (res_type>>8)&0xFF, res_type&0xFF, 0};
            /* Read Pascal string name */
            char name[256] = {0};
            if (name_ptr && name_ptr < MEM_SIZE) {
                int len = g_mem[name_ptr];
                if (len > 0 && len < 256 && name_ptr + len < MEM_SIZE)
                    memcpy(name, &g_mem[name_ptr + 1], len);
            }
            printf("  GetNamedResource('%s', \"%s\")\n", type_str, name);
            sp += 8;  /* Pop name(4) + resType(4) */
            m68k_set_reg(M68K_REG_A7, sp);
            m68k_write_memory_32(sp, 0);  /* Result: NULL handle */
            break;
        }

        case 0xA01E:  /* NewPtr */
        case 0xA11E:  /* NewPtrClear */
        {
            unsigned int size = d0;
            static unsigned int heap_ptr = SCSI_BUFFER;
            unsigned int ptr = heap_ptr;
            heap_ptr += (size + 15) & ~15;  /* Align to 16 */
            if (opcode == 0xA11E)
                memset(&g_mem[ptr], 0, size);
            printf("  NewPtr(%d) → 0x%08x\n", size, ptr);
            m68k_set_reg(M68K_REG_A0, ptr);
            m68k_set_reg(M68K_REG_D0, 0);
            break;
        }

        case 0xA122:  /* NewHandle */
        {
            unsigned int size = d0;
            static unsigned int handle_area = 0x060000;
            static unsigned int handle_data = 0x070000;
            unsigned int handle = handle_area;
            unsigned int data = handle_data;
            handle_area += 4;
            handle_data += (size + 15) & ~15;
            /* Handle is a pointer to a pointer */
            WRITE_BE32(g_mem, handle, data);
            printf("  NewHandle(%d) → handle=0x%08x data=0x%08x\n", size, handle, data);
            m68k_set_reg(M68K_REG_A0, handle);
            m68k_set_reg(M68K_REG_D0, 0);
            break;
        }

        case 0xA002:  /* _Read */
        case 0xA003:  /* _Write */
        case 0xA004:  /* _Control */
        case 0xA005:  /* _Status */
            printf("  Device Manager trap (will log vector patching)\n");
            m68k_set_reg(M68K_REG_D0, 0);
            break;

        case 0xA871:  /* NewDialog → DialogPtr on stack */
        {
            /* NewDialog pushes many params and returns a DialogPtr on stack.
             * Just pop all params and return a mock dialog pointer. */
            unsigned int sp = m68k_get_reg(NULL, M68K_REG_A7);
            /* NewDialog has 11 params totaling ~30 bytes, plus 4-byte result space.
             * The exact stack layout varies. Just pop everything and return NULL.
             * The Plug checks the result — NULL means "skip dialog". */
            sp += 26;  /* Pop params (approximate) */
            m68k_set_reg(M68K_REG_A7, sp);
            m68k_write_memory_32(sp, 0);  /* Result: NULL (no dialog) */
            printf("  NewDialog → returning NULL (no GUI)\n");
            break;
        }

        case 0xA820:  /* Get1NamedResource(resType, name) → Handle */
        case 0xA821:  /* Get1Resource(resType, resID) → Handle */
        {
            unsigned int sp = m68k_get_reg(NULL, M68K_REG_A7);
            unsigned int name_ptr = m68k_read_memory_32(sp);
            unsigned int res_type = m68k_read_memory_32(sp + 4);
            char type_str[5] = {(res_type>>24)&0xFF, (res_type>>16)&0xFF, (res_type>>8)&0xFF, res_type&0xFF, 0};
            char name[256] = {0};
            if (name_ptr && name_ptr < MEM_SIZE) {
                int len = g_mem[name_ptr];
                if (len > 0 && len < 256 && name_ptr + len < MEM_SIZE)
                    memcpy(name, &g_mem[name_ptr + 1], len);
            }
            printf("  Get1NamedResource('%s', \"%s\")\n", type_str, name);
            sp += 8;  /* Pop name(4) + resType(4) */
            m68k_set_reg(M68K_REG_A7, sp);

            /* For 'DRVR' ".EDisk", return a mock handle so INIT proceeds */
            if (res_type == 0x44525652 && strncmp(name, ".EDisk", 6) == 0) {
                /* Create a mock handle: handle → master pointer → mock DRVR data */
                static unsigned int mock_drvr_data = 0x080000;
                static unsigned int mock_handle = 0x060010;
                /* Write a minimal DRVR header at the data area */
                WRITE_BE16(g_mem, mock_drvr_data, 0x0000);  /* flags */
                WRITE_BE16(g_mem, mock_drvr_data + 2, 0x0000);  /* delay */
                WRITE_BE16(g_mem, mock_drvr_data + 4, 0x0000);  /* EMask */
                WRITE_BE16(g_mem, mock_drvr_data + 6, 0x0000);  /* menu */
                /* Handle points to master pointer, master pointer points to data */
                WRITE_BE32(g_mem, mock_handle, mock_drvr_data);
                printf("  → Returning mock handle 0x%08x → 0x%08x\n", mock_handle, mock_drvr_data);
                m68k_write_memory_32(sp, mock_handle);
            } else {
                m68k_write_memory_32(sp, 0);  /* NULL handle */
            }
            break;
        }

        default:
            printf("  UNHANDLED TRAP $%04X — returning 0\n", opcode);
            m68k_set_reg(M68K_REG_D0, 0);
            break;
    }
}

/* ======================================================================== */
/* Instruction hook (called before every instruction)                        */
/* ======================================================================== */
void plug_instruction_hook(unsigned int pc) {
    if (pc + 1 >= MEM_SIZE) {
        fprintf(stderr, "PC out of bounds: 0x%08x — halting\n", pc);
        g_running = 0;
        m68k_end_timeslice();
        return;
    }

    unsigned int opcode = READ_BE16(g_mem, pc);

    /* Detect A-line traps BEFORE the CPU takes the line-A exception.
     * We handle the trap in C, then rewrite the A-line word in memory to a NOP
     * so the CPU doesn't take the exception. After execution, we restore it. */
    if ((opcode & 0xF000) == 0xA000) {
        handle_aline_trap(opcode);
        /* Replace the A-line word with NOP (0x4E71) so the CPU just skips it */
        WRITE_BE16(g_mem, pc, 0x4E71);
        /* We need to restore it after execution — use a simple flag */
        /* For now, we won't restore (the plug code doesn't self-modify) */
    }

    /* Trace instructions in Inquiry + SCSICommand range */
    if ((pc >= PLUG_CODE_BASE + 0x06CC && pc <= PLUG_CODE_BASE + 0x0730) ||
        (pc >= PLUG_CODE_BASE + 0x1C00 && pc <= PLUG_CODE_BASE + 0x1E30)) {
        char buf[256];
        m68k_disassemble(buf, pc, M68K_CPU_TYPE_68040);
        unsigned int sp = m68k_get_reg(NULL, M68K_REG_A7);
        printf("  [%08x] SP=%08x  %s\n", pc, sp, buf);
    }

    if (pc == PLUG_CODE_BASE + 0x06C8) {
        printf("\n=== CONSTRUCTOR ENTRY (0x%08x) ===\n", pc);
    }
}

/* Illegal instruction callback */
int plug_illg_handler(int opcode) {
    unsigned int pc = m68k_get_reg(NULL, M68K_REG_PC);
    printf("ILLEGAL INSTRUCTION: $%04X at PC=0x%08x\n", opcode & 0xFFFF, pc);

    /* If it's an A-line trap that somehow got here, handle it */
    if ((opcode & 0xF000) == 0xA000) {
        handle_aline_trap(opcode);
        return 1;  /* Handled */
    }

    /* Dump registers and stack for debugging */
    printf("  Registers:\n");
    for (int i = 0; i <= 7; i++)
        printf("    D%d=0x%08x  A%d=0x%08x\n", i,
            m68k_get_reg(NULL, M68K_REG_D0 + i),
            m68k_get_reg(NULL, M68K_REG_A0 + i));
    unsigned int sp = m68k_get_reg(NULL, M68K_REG_A7);
    printf("  Stack at SP=0x%08x:", sp);
    for (int i = 0; i < 8 && sp + i*4 < MEM_SIZE; i++)
        printf(" %08x", READ_BE32(g_mem, sp + i*4));
    printf("\n");

    /* Stop execution on truly illegal instructions */
    g_running = 0;
    m68k_end_timeslice();
    return 0;
}

/* ======================================================================== */
/* Memory environment setup                                                  */
/* ======================================================================== */
static void setup_mac_env(void) {
    /* Memory already zeroed by calloc */

    /* Exception vectors */
    WRITE_BE32(g_mem, VEC_RESET_SP, STACK_TOP);
    WRITE_BE32(g_mem, VEC_RESET_PC, PLUG_CODE_BASE);  /* Not used directly */

    /* Line-A handler: just RTE */
    WRITE_BE16(g_mem, ALINE_HANDLER, 0x4E73);  /* RTE */
    WRITE_BE32(g_mem, VEC_LINEA, ALINE_HANDLER);

    /* Line-F handler: just RTE */
    WRITE_BE16(g_mem, ALINE_HANDLER + 2, 0x4E73);  /* RTE */
    WRITE_BE32(g_mem, VEC_LINEF, ALINE_HANDLER + 2);

    /* OS trap table: each entry is a unique address so GetOSTrapAddress returns something */
    for (int i = 0; i < 256; i++) {
        WRITE_BE32(g_mem, LM_OS_TRAPS + i * 4, 0x00F00000 + i * 4);
    }

    /* Toolbox trap table: same, distinct addresses */
    for (int i = 0; i < 1024; i++) {
        WRITE_BE32(g_mem, LM_TB_TRAPS + i * 4, 0x00F10000 + i * 4);
    }

    /* Set _Unimplemented ($A198) OS trap entry to a known address */
    WRITE_BE32(g_mem, LM_OS_TRAPS + 0x98 * 4, 0x00F00000 + 0x98 * 4);

    /* Set SCSIAtomic ($A89F) toolbox entry to a DIFFERENT address (so scsi43_flag = 1) */
    WRITE_BE32(g_mem, LM_TB_TRAPS + 0x09F * 4, 0x00F20000);

    /* Unit table: mock 48 entries */
    WRITE_BE32(g_mem, LM_UTABLEBASE, UNIT_TABLE);
    WRITE_BE16(g_mem, LM_UNITNTRYCNT, 48);

    /* Put some mock DCE entries in the unit table */
    for (int i = 0; i < 48; i++) {
        unsigned int dce_addr = UNIT_TABLE + 0x100 + i * 0x40;
        WRITE_BE32(g_mem, UNIT_TABLE + i * 4, dce_addr);
        /* DCE header: minimal valid structure */
        WRITE_BE16(g_mem, dce_addr + 4, 0x0020);  /* bit 5 set (capability flag) */
    }

    printf("Mac environment initialized:\n");
    printf("  Plug code:  0x%06x - 0x%06x\n", PLUG_CODE_BASE, PLUG_CODE_BASE + PLUG_CODE_SIZE);
    printf("  Plug data:  0x%06x - 0x%06x (A4)\n", PLUG_DATA_BASE, PLUG_DATA_BASE + PLUG_DATA_SIZE);
    printf("  Stack:      0x%06x (top)\n", STACK_TOP);
    printf("  Trap stubs: 0x%06x\n", TRAP_STUBS);
    printf("  Unit table: 0x%06x (%d entries)\n", UNIT_TABLE, 48);
}

/* ======================================================================== */
/* Load plug binary                                                          */
/* ======================================================================== */
static int load_plug(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open %s\n", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size > PLUG_CODE_SIZE) {
        fprintf(stderr, "Warning: plug binary %ld bytes, truncating to %d\n", size, PLUG_CODE_SIZE);
        size = PLUG_CODE_SIZE;
    }

    size_t read = fread(&g_mem[PLUG_CODE_BASE], 1, size, f);
    fclose(f);

    printf("Loaded %zu bytes from %s at 0x%06x\n", read, path, PLUG_CODE_BASE);

    /* Fix up absolute JSR/JMP references (PEF relocation).
     * The binary has absolute addresses relative to file offset 0.
     * We need to add PLUG_CODE_BASE to each target address. */
    int fixup_count = 0;
    for (unsigned int i = 0; i < (unsigned int)size - 5; i += 2) {
        unsigned int word = READ_BE16(g_mem, PLUG_CODE_BASE + i);
        if (word == 0x4EB9 || word == 0x4EF9) {  /* JSR abs.l or JMP abs.l */
            unsigned int addr = READ_BE32(g_mem, PLUG_CODE_BASE + i + 2);
            if (addr > 0x0400 && addr < (unsigned int)size) {
                unsigned int relocated = addr + PLUG_CODE_BASE;
                WRITE_BE32(g_mem, PLUG_CODE_BASE + i + 2, relocated);
                fixup_count++;
            }
        }
    }
    printf("  Relocated %d absolute JSR/JMP references (+0x%x)\n", fixup_count, PLUG_CODE_BASE);

    return 0;
}

/* ======================================================================== */
/* Run a subroutine and return when it hits RTS                              */
/* ======================================================================== */

/* We place a "return sentinel" on the stack. When the subroutine does RTS,
 * it pops this address and jumps to it. At that address we place a STOP
 * instruction that halts execution. */
#define RETURN_SENTINEL 0x100020
#define STOP_OPCODE     0x4E72    /* STOP #imm */

static void run_subroutine(unsigned int entry, const char *name) {
    /* Place STOP at the return sentinel address */
    WRITE_BE16(g_mem, RETURN_SENTINEL, STOP_OPCODE);
    WRITE_BE16(g_mem, RETURN_SENTINEL + 2, 0x2700);  /* STOP #$2700 (supervisor, all ints masked) */

    /* Set up CPU state */
    m68k_set_reg(M68K_REG_A4, PLUG_DATA_BASE);  /* Data section */
    m68k_set_reg(M68K_REG_A7, STACK_TOP - 4);   /* Stack with room for return addr */
    m68k_set_reg(M68K_REG_A6, STACK_TOP - 4);   /* Frame pointer = SP (caller convention) */
    m68k_set_reg(M68K_REG_A5, PLUG_DATA_BASE);  /* Alternate globals pointer */
    m68k_set_reg(M68K_REG_PC, entry);
    /* Set supervisor mode so RTE from line-A handler works */
    m68k_set_reg(M68K_REG_SR, 0x2700);  /* Supervisor, all interrupts masked */

    /* Push return sentinel on stack */
    unsigned int sp = m68k_get_reg(NULL, M68K_REG_A7);
    WRITE_BE32(g_mem, sp, RETURN_SENTINEL);

    printf("\n========================================\n");
    printf("Running %s at 0x%08x\n", name, entry);
    printf("  A4 (data) = 0x%08x\n", PLUG_DATA_BASE);
    printf("  A7 (stack) = 0x%08x\n", sp);
    printf("  Return sentinel = 0x%08x\n", RETURN_SENTINEL);
    printf("========================================\n\n");

    g_running = 1;
    g_trap_count = 0;
    int total_cycles = 0;
    int max_cycles = 10000000;  /* Safety limit */

    while (g_running && total_cycles < max_cycles) {
        int cycles = m68k_execute(100);
        total_cycles += cycles;
        unsigned int pc = m68k_get_reg(NULL, M68K_REG_PC);

        /* Check if we hit the return sentinel (STOP instruction) */
        if (pc == RETURN_SENTINEL || pc == RETURN_SENTINEL + 2 ||
            pc == RETURN_SENTINEL + 4) {
            printf("\n=== %s RETURNED (hit sentinel) ===\n", name);
            break;
        }

        /* Safety: limit total traps */
        if (g_trap_count > 1000) {
            printf("\n=== SAFETY LIMIT: too many traps ===\n");
            break;
        }
    }

    if (total_cycles >= max_cycles) {
        unsigned int pc = m68k_get_reg(NULL, M68K_REG_PC);
        printf("\n=== CYCLE LIMIT reached at PC=0x%08x ===\n", pc);
        char buf[256];
        m68k_disassemble(buf, pc, M68K_CPU_TYPE_68040);
        printf("  Current instruction: %s\n", buf);
    }

    printf("  D0 = 0x%08x (result)\n", m68k_get_reg(NULL, M68K_REG_D0));
    printf("  Total traps: %d\n", g_trap_count);
}

/* ======================================================================== */
/* Main                                                                      */
/* ======================================================================== */
int main(int argc, char *argv[]) {
    const char *plug_path = NULL;

    setbuf(stdout, NULL);  /* Unbuffered output */
    setbuf(stderr, NULL);

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <plug_full.bin>\n", argv[0]);
        fprintf(stderr, "  Loads the SCSI Plug binary and runs its entry points.\n");
        return 1;
    }
    plug_path = argv[1];

    printf("=== SCSI Plug Harness ===\n\n");

    /* Allocate emulated memory */
    g_mem = calloc(1, MEM_SIZE);
    if (!g_mem) { fprintf(stderr, "Failed to allocate %u bytes\n", MEM_SIZE); return 1; }
    printf("Allocated %u bytes (%u MB) for emulated memory\n", MEM_SIZE, MEM_SIZE / (1024*1024));

    /* Initialize memory and Mac OS environment */
    setup_mac_env();

    /* Load plug binary */
    if (load_plug(plug_path) < 0)
        return 1;

    /* Initialize SCSI bridge to scsi2pi */
    scsi_bridge_init("10.0.0.57", 6868);

    /* Initialize Musashi */
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68040);
    m68k_pulse_reset();

    /* Skip the constructor (it requires PEF relocation we can't provide).
     * Instead, manually set up the critical Plug state. */
    printf("=== Skipping constructor — setting up state manually ===\n");

    /* Set scsi43_flag = 1 (SCSI Manager 4.3 available) */
    g_mem[PLUG_DATA_BASE + 0x26A] = 1;
    printf("  scsi43_flag (A4+0x26A) = 1\n");

    /* Set init_marker to non-zero (mark as initialized) */
    WRITE_BE32(g_mem, PLUG_DATA_BASE + 0x266, PLUG_DATA_BASE);
    printf("  init_marker (A4+0x266) = 0x%08x\n", PLUG_DATA_BASE);

    /* Phase 2: Write a tiny 68k stub that builds a SCSI PB and calls $A089.
     * This proves the harness can intercept SCSI commands.
     * SCSI Manager 4.3 SCSIExecIO PB layout:
     *   +0: qLink (4), +4: reserved (2), +6: PBLength (2), +8: functionCode (1),
     *   +9: reserved (1), +10: result (2), +12: device (bus:1, target:1, lun:1, pad:1, +4 reserved),
     *   +20: flags (4), +24: timeout (2), +26: pad (2),
     *   +28: dataPtr (4), +32: dataLength (4), +36: sensePtr (4), +40: senseLength (2),
     *   +42: CDB (16 bytes) */
    printf("\n=== Phase 2: SCSI Command Test ===\n");
    g_oob_count = 0;

    /* Build SCSI PB for INQUIRY at SCSI_BUFFER */
    unsigned int pb = SCSI_BUFFER;
    unsigned int inq_buf = SCSI_BUFFER + 0x100;
    memset(&g_mem[pb], 0, 64);  /* Zero the PB */
    WRITE_BE16(g_mem, pb + 6, 0x0058);   /* PBLength = 88 */
    g_mem[pb + 8] = 0x01;                 /* functionCode = SCSIExecIO */
    g_mem[pb + 12] = 0;                   /* bus = 0 */
    g_mem[pb + 13] = 6;                   /* target = 6 */
    g_mem[pb + 14] = 0;                   /* lun = 0 */
    WRITE_BE32(g_mem, pb + 20, 0x40040000); /* flags: read (from SCSI-PROTOCOL.md) */
    WRITE_BE16(g_mem, pb + 24, 1000);     /* timeout = 1000 */
    WRITE_BE32(g_mem, pb + 28, inq_buf);  /* dataPtr */
    WRITE_BE32(g_mem, pb + 32, 96);       /* dataLength = 96 */
    /* CDB at pb+42: INQUIRY command */
    g_mem[pb + 42] = 0x12;               /* INQUIRY opcode */
    g_mem[pb + 43] = 0x00;
    g_mem[pb + 44] = 0x00;
    g_mem[pb + 45] = 0x00;
    g_mem[pb + 46] = 96;                 /* allocation length */
    g_mem[pb + 47] = 0x00;
    WRITE_BE16(g_mem, pb + 58, 6);       /* CDB length = 6 */

    /* Write a tiny 68k program at 0x0F0000 that calls $A089 then RTS */
    unsigned int stub = 0x0F0000;
    int so = 0;
    /* LEA pb, A0 */
    g_mem[stub + so++] = 0x41; g_mem[stub + so++] = 0xF9;  /* LEA abs.l, A0 */
    WRITE_BE32(g_mem, stub + so, pb); so += 4;
    /* MOVEQ #1, D0 (SCSIAction selector) */
    g_mem[stub + so++] = 0x70; g_mem[stub + so++] = 0x01;
    /* $A089 (SCSIDispatch) */
    g_mem[stub + so++] = 0xA0; g_mem[stub + so++] = 0x89;
    /* RTS */
    g_mem[stub + so++] = 0x4E; g_mem[stub + so++] = 0x75;

    printf("\nRunning SCSI INQUIRY stub (bus=0, target=6)...\n");
    printf("  PB at 0x%08x, data buffer at 0x%08x\n", pb, inq_buf);
    printf("  CDB: %02x %02x %02x %02x %02x %02x\n\n",
        g_mem[pb+42], g_mem[pb+43], g_mem[pb+44], g_mem[pb+45], g_mem[pb+46], g_mem[pb+47]);

    run_subroutine(stub, "SCSI INQUIRY stub");

    /* ================================================================== */
    /* Phase 3: MIDI-over-SCSI protocol — talk to the S3000XL directly    */
    /* ================================================================== */
    printf("\n========================================\n");
    printf("Phase 3: MIDI-over-SCSI Protocol\n");
    printf("========================================\n\n");

    int target = 6;
    uint8_t cdb[6];
    uint8_t buf[4096];
    size_t buf_len;
    int status;

    /* Step 1: TEST UNIT READY */
    printf("--- Step 1: TEST UNIT READY ---\n");
    memset(cdb, 0, 6);  /* CDB: 00 00 00 00 00 00 */
    status = scsi_bridge_exec(target, 0, cdb, 6, NULL, 0, NULL, NULL, 10);
    printf("  status=%d %s\n\n", status, status == 0 ? "(ready)" : "(NOT ready)");
    if (status != 0) {
        printf("Device not ready, aborting.\n");
        free(g_mem);
        return 1;
    }

    /* Step 2: Enable MIDI mode */
    printf("--- Step 2: SET MIDI MODE (enable) ---\n");
    cdb[0] = 0x09; cdb[1] = 0x00; cdb[2] = 0x01; /* MIDI on */
    cdb[3] = 0x00; /* thru off */ cdb[4] = 0x00; cdb[5] = 0x00;
    printf("  CDB: %02x %02x %02x %02x %02x %02x\n", cdb[0],cdb[1],cdb[2],cdb[3],cdb[4],cdb[5]);
    status = scsi_bridge_exec(target, 0, cdb, 6, NULL, 0, NULL, NULL, 10);
    printf("  status=%d\n\n", status);

    /* Brief pause after MIDI mode enable */
    usleep(200000);

    /* Step 2b: TEST UNIT READY before send (MESA does this) */
    printf("--- Step 2b: TEST UNIT READY (pre-send check) ---\n");
    memset(cdb, 0, 6);
    status = scsi_bridge_exec(target, 0, cdb, 6, NULL, 0, NULL, NULL, 10);
    printf("  status=%d\n\n", status);

    /* Step 3: Send AKAI SysEx — RSLIST */
    printf("--- Step 3: SEND AKAI SYSEX (Request Sample Names) ---\n");
    uint8_t sysex_identity[] = { 0xF0, 0x47, 0x00, 0x04, 0x48, 0xF7 };
    size_t sysex_len = sizeof(sysex_identity);
    cdb[0] = 0x0C; cdb[1] = 0x00;
    cdb[2] = (sysex_len >> 16) & 0xFF;
    cdb[3] = (sysex_len >> 8) & 0xFF;
    cdb[4] = sysex_len & 0xFF;
    cdb[5] = 0x00;  /* flag=0 works (0x80 causes CHECK CONDITION) */

    /* Probe all exclusive channels 0-15 to find the device */
    int found_channel = -1;
    for (int ch = 0; ch < 16 && found_channel < 0; ch++) {
        sysex_identity[2] = ch;
        printf("  ch=%d: F0 47 %02x 04 48 F7 → ", ch, ch);
        status = scsi_bridge_exec(target, 0, cdb, 6, sysex_identity, sysex_len, NULL, NULL, 10);
        usleep(100000);
        /* Poll */
        uint8_t poll_cdb[6] = { 0x0D, 0, 0, 0, 0, 0x00 };
        uint8_t poll_buf[3] = {0};
        size_t poll_len = 3;
        scsi_bridge_exec(target, 0, poll_cdb, 6, NULL, 0, poll_buf, &poll_len, 10);
        uint32_t avail = 0;
        if (poll_len >= 3)
            avail = ((uint32_t)poll_buf[0]<<16)|((uint32_t)poll_buf[1]<<8)|poll_buf[2];
        printf("send=%d, avail=%u\n", status, avail);
        if (avail > 0) { found_channel = ch; break; }
    }
    if (found_channel >= 0) {
        printf("  *** Found device on exclusive channel %d! ***\n", found_channel);
        /* Read the sample name list response */
        uint8_t rd_cdb[6] = { 0x0E, 0, 0, 0, 0, 0x00 };
        uint32_t avail_now = 0;
        /* Re-poll to get current count (probe already polled) */
        {
            uint8_t poll_cdb2[6] = { 0x0D, 0, 0, 0, 0, 0x00 };
            uint8_t pb2[3] = {0};
            size_t pb2_len = 3;
            /* The probe already read the count; send RSLIST again on found channel */
            sysex_identity[2] = found_channel;
            cdb[0] = 0x0C; cdb[1]=0; cdb[2]=0; cdb[3]=0; cdb[4]=sysex_len; cdb[5]=0x00;
            scsi_bridge_exec(target, 0, cdb, 6, sysex_identity, sysex_len, NULL, NULL, 10);
            usleep(200000);
            scsi_bridge_exec(target, 0, poll_cdb2, 6, NULL, 0, pb2, &pb2_len, 10);
            if (pb2_len >= 3)
                avail_now = ((uint32_t)pb2[0]<<16)|((uint32_t)pb2[1]<<8)|pb2[2];
        }
        if (avail_now > 0) {
            printf("\n--- Reading SAMPLE NAME LIST (%u bytes) ---\n", avail_now);
            if (avail_now > sizeof(buf)) avail_now = sizeof(buf);
            rd_cdb[2] = (avail_now>>16)&0xFF;
            rd_cdb[3] = (avail_now>>8)&0xFF;
            rd_cdb[4] = avail_now&0xFF;
            buf_len = avail_now;
            memset(buf, 0, buf_len);
            status = scsi_bridge_exec(target, 0, rd_cdb, 6, NULL, 0, buf, &buf_len, 10);
            printf("  status=%d, received %zu bytes\n", status, buf_len);
            if (buf_len > 0) {
                printf("  Raw:");
                for (size_t i = 0; i < buf_len && i < 80; i++) {
                    printf(" %02x", buf[i]);
                    if (i > 0 && buf[i] == 0xF7) { printf(" (EoSx)"); break; }
                }
                if (buf_len > 80) printf(" ...");
                printf("\n");

                /* Decode Akai name list */
                if (buf_len >= 6 && buf[0] == 0xF0 && buf[1] == 0x47) {
                    unsigned int opcode_r = buf[3];
                    if (opcode_r == 0x05 && buf_len >= 8) {
                        int count = buf[5] | (buf[6] << 8);
                        printf("\n  SAMPLES (%d):\n", count);
                        int off = 7;
                        for (int i = 0; i < count && off + 12 <= (int)buf_len; i++) {
                            char name[13];
                            for (int j = 0; j < 12; j++) {
                                unsigned char c = buf[off + j];
                                if (c <= 9) name[j] = '0' + c;
                                else if (c == 10) name[j] = ' ';
                                else if (c >= 11 && c <= 36) name[j] = 'A' + (c - 11);
                                else if (c >= 37 && c <= 62) name[j] = 'a' + (c - 37);
                                else if (c == 63) name[j] = '#';
                                else if (c == 64) name[j] = '+';
                                else if (c == 65) name[j] = '-';
                                else if (c == 66) name[j] = '.';
                                else name[j] = '?';
                            }
                            name[12] = '\0';
                            printf("    %3d: \"%s\"\n", i, name);
                            off += 12;
                        }
                    }
                }
            }
        }

        /* Now request PROGRAM names */
        printf("\n--- Requesting PROGRAM NAME LIST ---\n");
        uint8_t rplist[] = { 0xF0, 0x47, (uint8_t)found_channel, 0x02, 0x48, 0xF7 };
        cdb[0]=0x0C; cdb[1]=0; cdb[2]=0; cdb[3]=0; cdb[4]=sizeof(rplist); cdb[5]=0x00;
        scsi_bridge_exec(target, 0, cdb, 6, rplist, sizeof(rplist), NULL, NULL, 10);
        usleep(200000);
        {
            uint8_t poll_cdb3[6] = { 0x0D, 0, 0, 0, 0, 0x00 };
            uint8_t pb3[3] = {0};
            size_t pb3_len = 3;
            scsi_bridge_exec(target, 0, poll_cdb3, 6, NULL, 0, pb3, &pb3_len, 10);
            uint32_t pavail = 0;
            if (pb3_len >= 3)
                pavail = ((uint32_t)pb3[0]<<16)|((uint32_t)pb3[1]<<8)|pb3[2];
            if (pavail > 0) {
                printf("  %u bytes available\n", pavail);
                if (pavail > sizeof(buf)) pavail = sizeof(buf);
                rd_cdb[2]=(pavail>>16)&0xFF; rd_cdb[3]=(pavail>>8)&0xFF; rd_cdb[4]=pavail&0xFF;
                buf_len = pavail;
                memset(buf, 0, buf_len);
                status = scsi_bridge_exec(target, 0, rd_cdb, 6, NULL, 0, buf, &buf_len, 10);
                printf("  received %zu bytes\n", buf_len);
                if (buf_len >= 8 && buf[0]==0xF0 && buf[1]==0x47 && buf[3]==0x03) {
                    int cnt = buf[5]|(buf[6]<<8);
                    printf("\n  PROGRAMS (%d):\n", cnt);
                    int off = 7;
                    for (int i = 0; i < cnt && off+12<=(int)buf_len; i++) {
                        char nm[13];
                        for (int j=0;j<12;j++) {
                            unsigned char c=buf[off+j];
                            if(c<=9)nm[j]='0'+c; else if(c==10)nm[j]=' ';
                            else if(c>=11&&c<=36)nm[j]='A'+(c-11);
                            else if(c>=37&&c<=62)nm[j]='a'+(c-37);
                            else if(c==63)nm[j]='#'; else if(c==64)nm[j]='+';
                            else if(c==65)nm[j]='-'; else if(c==66)nm[j]='.';
                            else nm[j]='?';
                        }
                        nm[12]='\0';
                        printf("    %3d: \"%s\"\n", i, nm);
                        off += 12;
                    }
                }
            } else {
                printf("  No program data available.\n");
            }
        }
    } else {
        printf("  No response on any channel. Device may have SysEx disabled.\n");
    }

    /* If CHECK CONDITION, send REQUEST SENSE to get the error details */
    if (status < 0) {
        printf("  Sending REQUEST SENSE...\n");
        cdb[0] = 0x03; cdb[1] = 0; cdb[2] = 0; cdb[3] = 0;
        cdb[4] = 18;  /* allocation length */
        cdb[5] = 0;
        buf_len = 18;
        status = scsi_bridge_exec(target, 0, cdb, 6, NULL, 0, buf, &buf_len, 10);
        printf("  REQUEST SENSE status=%d, %zu bytes:", status, buf_len);
        for (size_t i = 0; i < buf_len; i++) printf(" %02x", buf[i]);
        printf("\n");
        if (buf_len >= 3) {
            printf("  Sense key: 0x%x, ASC: 0x%02x, ASCQ: 0x%02x\n",
                buf[2] & 0x0F,
                buf_len >= 13 ? buf[12] : 0,
                buf_len >= 14 ? buf[13] : 0);
        }
    }
    printf("\n");

    /* Step 4: Poll for reply — Data Byte Enquiry */
    printf("--- Step 4: DATA BYTE ENQUIRY (poll for reply) ---\n");
    int retries = 10;
    uint32_t reply_len = 0;
    while (retries-- > 0) {
        cdb[0] = 0x0D; cdb[1] = 0x00; cdb[2] = 0x00;
        cdb[3] = 0x00; cdb[4] = 0x00; cdb[5] = 0x80;
        buf_len = 3;
        status = scsi_bridge_exec(target, 0, cdb, 6, NULL, 0, buf, &buf_len, 10);
        if (buf_len >= 3) {
            reply_len = ((uint32_t)buf[0] << 16) | ((uint32_t)buf[1] << 8) | buf[2];
            printf("  status=%d, bytes available: %u (raw: %02x %02x %02x)\n",
                status, reply_len, buf[0], buf[1], buf[2]);
            if (reply_len > 0) break;
        } else {
            printf("  status=%d, no data (buf_len=%zu)\n", status, buf_len);
        }
        /* Brief pause before retry */
        usleep(100000);  /* 100ms */
    }

    /* Step 5: Read reply if available */
    if (reply_len > 0) {
        printf("\n--- Step 5: RECEIVE MIDI DATA (%u bytes) ---\n", reply_len);
        if (reply_len > sizeof(buf)) reply_len = sizeof(buf);
        cdb[0] = 0x0E; cdb[1] = 0x00;
        cdb[2] = (reply_len >> 16) & 0xFF;
        cdb[3] = (reply_len >> 8) & 0xFF;
        cdb[4] = reply_len & 0xFF;
        cdb[5] = 0x80;
        buf_len = reply_len;
        memset(buf, 0, buf_len);
        status = scsi_bridge_exec(target, 0, cdb, 6, NULL, 0, buf, &buf_len, 10);
        printf("  status=%d, received %zu bytes\n", status, buf_len);
        if (buf_len > 0) {
            printf("  MIDI response:");
            for (size_t i = 0; i < buf_len; i++) {
                printf(" %02x", buf[i]);
                if (i > 0 && buf[i] == 0xF7) { printf(" (End of SysEx)"); break; }
            }
            printf("\n");

            /* Decode Akai SysEx response */
            if (buf_len >= 6 && buf[0] == 0xF0 && buf[1] == 0x47) {
                unsigned int channel = buf[2];
                unsigned int opcode = buf[3];
                unsigned int device_id = buf[4];
                printf("\n  Akai SysEx: channel=%d opcode=0x%02x device=0x%02x\n",
                    channel, opcode, device_id);

                /* Opcode 0x05 = SLIST (sample name list response) */
                /* Opcode 0x03 = PLIST (program name list response) */
                if ((opcode == 0x05 || opcode == 0x03) && buf_len >= 8) {
                    int count = buf[5] | (buf[6] << 8);  /* 16-bit LE count */
                    const char *kind = (opcode == 0x05) ? "Sample" : "Program";
                    printf("  %s count: %d\n", kind, count);
                    int offset = 7;
                    for (int i = 0; i < count && offset + 12 <= (int)buf_len; i++) {
                        /* Each name is 12 bytes, Akai encoding (roughly ASCII with some mapping) */
                        char name[13];
                        for (int j = 0; j < 12; j++) {
                            unsigned char c = buf[offset + j];
                            /* Basic Akai→ASCII: 0-9 = '0'-'9', 10-35 = ' ','A'-'Z', etc. */
                            if (c <= 9) name[j] = '0' + c;
                            else if (c == 10) name[j] = ' ';
                            else if (c >= 11 && c <= 36) name[j] = 'A' + (c - 11);
                            else if (c >= 37 && c <= 62) name[j] = 'a' + (c - 37);
                            else if (c == 63) name[j] = '#';
                            else if (c == 64) name[j] = '+';
                            else if (c == 65) name[j] = '-';
                            else if (c == 66) name[j] = '.';
                            else name[j] = '?';
                        }
                        name[12] = '\0';
                        printf("    %3d: \"%s\"\n", i, name);
                        offset += 12;
                    }
                }
                /* Opcode 0x16 = REPLY (error or OK) */
                else if (opcode == 0x16) {
                    int code = (buf_len > 5) ? buf[5] : -1;
                    printf("  Reply: %s (code=%d)\n", code == 0 ? "OK" : "ERROR", code);
                }
            }
        }
    } else {
        printf("\n  No reply data available.\n");
    }

    /* Step 6: Now try AKAI SysEx — RSLIST (sample names) */
    printf("\n--- Step 6: SEND AKAI SYSEX (Request Sample Names) ---\n");
    {
        /* Try multiple exclusive channels: 0x00 (default), then 0x7F (all) */
        int channels[] = { 0x00, 0x7F };
        for (int ci = 0; ci < 2; ci++) {
            int ch = channels[ci];
            uint8_t rslist[] = { 0xF0, 0x47, (uint8_t)ch, 0x04, 0x48, 0xF7 };
            printf("  Trying channel %d: F0 47 %02x 04 48 F7\n", ch, ch);
            cdb[0] = 0x0C; cdb[1] = 0; cdb[2] = 0; cdb[3] = 0;
            cdb[4] = sizeof(rslist); cdb[5] = 0x80;
            status = scsi_bridge_exec(target, 0, cdb, 6, rslist, sizeof(rslist), NULL, NULL, 10);
            printf("  send status=%d\n", status);
            usleep(200000);

            /* Poll */
            cdb[0] = 0x0D; cdb[1]=0; cdb[2]=0; cdb[3]=0; cdb[4]=0; cdb[5]=0x80;
            buf_len = 3;
            status = scsi_bridge_exec(target, 0, cdb, 6, NULL, 0, buf, &buf_len, 10);
            reply_len = 0;
            if (buf_len >= 3)
                reply_len = ((uint32_t)buf[0] << 16) | ((uint32_t)buf[1] << 8) | buf[2];
            printf("  poll: %u bytes available\n", reply_len);

            if (reply_len > 0) {
                if (reply_len > sizeof(buf)) reply_len = sizeof(buf);
                cdb[0] = 0x0E; cdb[1]=0;
                cdb[2]=(reply_len>>16)&0xFF; cdb[3]=(reply_len>>8)&0xFF;
                cdb[4]=reply_len&0xFF; cdb[5]=0x80;
                buf_len = reply_len;
                memset(buf, 0, buf_len);
                status = scsi_bridge_exec(target, 0, cdb, 6, NULL, 0, buf, &buf_len, 10);
                printf("  received %zu bytes:", buf_len);
                for (size_t i = 0; i < buf_len && i < 80; i++) {
                    printf(" %02x", buf[i]);
                    if (i > 0 && buf[i] == 0xF7) { printf(" (EoSx)"); break; }
                }
                printf("\n");
                break;  /* Got a response, stop trying channels */
            }
        }
    }

    /* Step 7: Request Program Name List (RPLIST = 0x02) */
    printf("\n--- Step 6: SEND AKAI SYSEX (Request Program Names) ---\n");
    {
        uint8_t rplist[] = { 0xF0, 0x47, 0x00, 0x02, 0x48, 0xF7 };
        size_t rplist_len = sizeof(rplist);
        cdb[0] = 0x0C; cdb[1] = 0x00;
        cdb[2] = 0; cdb[3] = 0; cdb[4] = rplist_len & 0xFF; cdb[5] = 0x80;
        printf("  SysEx: F0 47 00 02 48 F7 (RPLIST)\n");
        status = scsi_bridge_exec(target, 0, cdb, 6, rplist, rplist_len, NULL, NULL, 10);
        printf("  send status=%d\n", status);

        /* Poll for reply */
        usleep(100000);
        cdb[0] = 0x0D; cdb[1] = 0; cdb[2] = 0; cdb[3] = 0; cdb[4] = 0; cdb[5] = 0x80;
        buf_len = 3;
        reply_len = 0;
        retries = 10;
        while (retries-- > 0) {
            buf_len = 3;
            status = scsi_bridge_exec(target, 0, cdb, 6, NULL, 0, buf, &buf_len, 10);
            if (buf_len >= 3) {
                reply_len = ((uint32_t)buf[0] << 16) | ((uint32_t)buf[1] << 8) | buf[2];
                printf("  poll: %u bytes available\n", reply_len);
                if (reply_len > 0) break;
            }
            usleep(100000);
        }
        if (reply_len > 0) {
            if (reply_len > sizeof(buf)) reply_len = sizeof(buf);
            cdb[0] = 0x0E; cdb[1] = 0;
            cdb[2] = (reply_len >> 16) & 0xFF;
            cdb[3] = (reply_len >> 8) & 0xFF;
            cdb[4] = reply_len & 0xFF; cdb[5] = 0x80;
            buf_len = reply_len;
            memset(buf, 0, buf_len);
            status = scsi_bridge_exec(target, 0, cdb, 6, NULL, 0, buf, &buf_len, 10);
            printf("  received %zu bytes\n", buf_len);
            if (buf_len > 0) {
                printf("  MIDI response:");
                for (size_t i = 0; i < buf_len && i < 80; i++) {
                    printf(" %02x", buf[i]);
                    if (i > 0 && buf[i] == 0xF7) { printf(" (EoSx)"); break; }
                }
                if (buf_len > 80) printf(" ...");
                printf("\n");

                /* Decode Akai name list */
                if (buf_len >= 6 && buf[0] == 0xF0 && buf[1] == 0x47) {
                    unsigned int opcode2 = buf[3];
                    if (opcode2 == 0x03 && buf_len >= 8) {
                        int count2 = buf[5] | (buf[6] << 8);
                        printf("\n  Program count: %d\n", count2);
                        int off = 7;
                        for (int i = 0; i < count2 && off + 12 <= (int)buf_len; i++) {
                            char name[13];
                            for (int j = 0; j < 12; j++) {
                                unsigned char c = buf[off + j];
                                if (c <= 9) name[j] = '0' + c;
                                else if (c == 10) name[j] = ' ';
                                else if (c >= 11 && c <= 36) name[j] = 'A' + (c - 11);
                                else if (c >= 37 && c <= 62) name[j] = 'a' + (c - 37);
                                else if (c == 63) name[j] = '#';
                                else if (c == 64) name[j] = '+';
                                else if (c == 65) name[j] = '-';
                                else if (c == 66) name[j] = '.';
                                else name[j] = '?';
                            }
                            name[12] = '\0';
                            printf("    %3d: \"%s\"\n", i, name);
                            off += 12;
                        }
                    }
                }
            }
        }
    }

    /* Step 7: Disable MIDI mode */
    printf("\n--- Step 7: SET MIDI MODE (disable) ---\n");
    cdb[0] = 0x09; cdb[1] = 0x00; cdb[2] = 0x00;
    cdb[3] = 0x00; cdb[4] = 0x00; cdb[5] = 0x00;
    status = scsi_bridge_exec(target, 0, cdb, 6, NULL, 0, NULL, NULL, 10);
    printf("  status=%d\n", status);

    printf("\n========================================\n");
    printf("MIDI-over-SCSI protocol complete.\n");
    printf("========================================\n");

    free(g_mem);
    return 0;
}
