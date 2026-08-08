/*
 * Copyright (c) 2026 Qualcomm Technologies, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Shared scaffolding for M-core QEMU machines. A per-chip QsimSocDesc table
 * (e.g. osaka.c) drives the armv7m container, RAM/ROM-alias, and ns16550 build.
 */
#ifndef HW_ARM_QSIM_SOC_H
#define HW_ARM_QSIM_SOC_H

#include "qemu/osdep.h"
/*
 * QEMU 11.x moved these headers under hw/core/ and system/. Select on the tree
 * being built against so one copy of the model builds both the 10.x and 11.x
 * vintages; they are gathered here rather than at each use site because the
 * choice is a property of the QEMU version, not of any one source file.
 */
#if defined(__has_include) && __has_include("hw/core/boards.h")
#include "hw/core/boards.h"          /* MachineState (embedded by value below) */
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/irq.h"
#include "hw/core/loader.h"
#include "system/address-spaces.h"
#else
#include "hw/boards.h"               /* MachineState (embedded by value below) */
#include "hw/qdev-properties.h"
#include "hw/qdev-clock.h"
#include "hw/irq.h"
#include "hw/loader.h"
#include "exec/address-spaces.h"
#endif
#include "hw/arm/armv7m.h"    /* ARMv7MState (embedded by value below) */
#include "exec/hwaddr.h"

#define QSIM_MAX_RAM      24
#define QSIM_MAX_UART     8
#define QSIM_MAX_DOORBELL 8
#define QSIM_MAX_DB_CHAN  16
#define QSIM_MAX_DB_DATA  64

/* A RAM region. If window is set it is a low-priority background partition
 * window (executable RAM per ARM B3-2; real RAM/devices mapped inside it win).
 * If alias_base is set, an alias is also mapped there (models the CM0 ROM
 * remap to 0x0). */
typedef struct QsimRamDesc {
    const char *name;
    hwaddr      base;
    hwaddr      size;
    hwaddr      alias_base;   /* 0 = no alias */
    bool        has_alias;
    bool        window;       /* true = low-priority background partition RAM */
} QsimRamDesc;

/* A memory-mapped ns16550 UART wired to one NVIC line. */
typedef struct QsimUartDesc {
    hwaddr base;
    int    regshift;          /* Osaka ns16550 = 2 */
    int    nvic_irq;          /* per-core NVIC input line (from the INT MAP) */
    int    baudbase;          /* clock-frequency from the dtsi */
} QsimUartDesc;

/* A doorbell mailbox instance. Register map from the cpvs doorbell unit test:
 * MASK 0x00, CLR 0x04 (W1C of ST), TRIGGER 0x08 (W1S, gated by MASK),
 * ST 0x0C, PERIOD 0x10, MODE 0x14 (0 level, 1 one-shot), DATA<n> 0x18 + 4n.
 * irq_base = NVIC line of channel 0, or -1 when this core has no line to it. */
typedef struct QsimDoorbellDesc {
    const char *name;
    hwaddr      base;
    int         num_channel;
    int         num_data;
    int         irq_base;
} QsimDoorbellDesc;

/* The complete per-chip description consumed by qsim_soc_init(). */
typedef struct QsimSocDesc {
    const char  *cpu_type;        /* ARM_CPU_TYPE_NAME("cortex-m7") etc. */
    uint32_t     num_irq;         /* NVIC line ceiling from the INT MAP */
    QsimRamDesc  ram[QSIM_MAX_RAM];
    int          num_ram;
    QsimUartDesc uart[QSIM_MAX_UART];
    int          num_uart;
    QsimDoorbellDesc doorbell[QSIM_MAX_DOORBELL];
    int          num_doorbell;
    hwaddr       tzc_base;        /* TZC-400 filter + config block, 0 = absent */
    hwaddr       tzpc_base;       /* TZPC DEC_PROT block, 0 = absent */
    hwaddr       kernel_load_base;   /* armv7m_load_kernel mem_base */
    int          kernel_load_size;   /* armv7m_load_kernel mem_size */
    /* Optional boot-ROM image installed when the machine runs with rom-stub=on
     * (baremetal flavour: -kernel carries the FW, this blob provides the real
     * ROM at rom_base whose alias at 0x0 serves the reset vector fetch). The
     * ROM polls rom_flag_base for rom_flag_magic before handing off, so the
     * flag is pre-seeded into the owning doorbell's DATA word. */
    const unsigned char *rom_stub;
    int          rom_stub_size;
    hwaddr       rom_stub_base;
    hwaddr       rom_flag_base;
    uint32_t     rom_flag_magic;
    uint32_t     init_vtor;          /* reset vector base (0 = fetch from 0x0) */
    uint32_t     mpu_dregion;        /* explicit MPU region count (0 = QEMU default) */
} QsimSocDesc;

/* Shared machine state: just the armv7m container + clocks. Per-chip RAM
 * MemoryRegions are heap-allocated by the helper (lifetime = machine). */
typedef struct QsimSocState {
    MachineState parent;
    ARMv7MState  armv7m;
    Clock       *sysclk;
    Clock       *refclk;
    bool         rom_stub;    /* -M <m>,rom-stub=on: install desc->rom_stub */
} QsimSocState;

#define TYPE_QSIM_MACHINE "qsim"
OBJECT_DECLARE_SIMPLE_TYPE(QsimSocState, QSIM_MACHINE)

/* Build the machine from @desc: clocks, RAM/alias, armv7m (num-irq, cpu-type),
 * ns16550 UARTs wired to their NVIC lines, unimplemented fill, kernel load.
 * Called from each chip's MachineClass::init via a desc lookup. */
void qsim_soc_init(MachineState *machine, const QsimSocDesc *desc);

#endif /* HW_ARM_QSIM_SOC_H */
