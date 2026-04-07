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
#include "musashi/m68k.h"

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
            printf("  *** SCSI DISPATCH *** A0=PB@0x%08x\n", a0);
            if (a0 && a0 + 32 < MEM_SIZE) {
                printf("  PB dump:");
                for (int i = 0; i < 32; i++)
                    printf(" %02x", g_mem[a0 + i]);
                printf("\n");
            }
            /* Return noErr for now — will forward to scsi2pi later */
            m68k_set_reg(M68K_REG_D0, 0);
            break;

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
    WRITE_BE32(g_mem, pb + 20, 0x80000000); /* flags: read */
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

    return 0;
}
