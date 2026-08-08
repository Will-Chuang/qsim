/*
 * Copyright (c) 2026 Qualcomm Technologies, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Osaka SoC QEMU machines: osaka-cm7, osaka-cm0. Data tables only; the build
 * logic lives in the shared qsim_soc helper.
 */
#include "qsim_soc.h"   /* also supplies the headers QEMU 11.x relocated */
#include "cm0_rom_blob.h"
#include "qemu/units.h"
#include "qemu/module.h"
#include "target/arm/cpu-qom.h"
/*
 * QEMU 11.x filters machines per target binary: one built into the shared
 * arm_common_ss is listed by qemu-system-arm only if it declares
 * arm_machine_interfaces[]. The header does not exist on 10.x, where every
 * machine in the ARM source set is listed unconditionally.
 */
#if defined(__has_include) && __has_include("hw/arm/machines-qom.h")
#include "hw/arm/machines-qom.h"
#define QSIM_MACHINE_IFACES .interfaces = arm_machine_interfaces,
#else
#define QSIM_MACHINE_IFACES
#endif

/* CM7 memory map per Confluence Sentry/Memory Map (id 2881789520), CM7 column.
 * ITCM@0/DTCM@0x20000000 (M7 core TCM); partition windows are low-priority
 * executable RAM (ARM B3-2) so the decode matches the SoC and embedded
 * RAM/devices mapped inside still win. */
static const QsimSocDesc osaka_cm7_desc = {
    .cpu_type = ARM_CPU_TYPE_NAME("cortex-m7"),
    .num_irq  = 240,
    .ram = {
        { .name = "osaka.itcm", .base = 0x00000000, .size = 256 * KiB },
        { .name = "osaka.dtcm", .base = 0x20000000, .size = 256 * KiB },
        /* DRAM @ 0x80000 (SoT CM7: after the 512KB M7_TCM region) up to PART_AON
         * @0x80000000; low priority so DTCM overlays its band. */
        { .name = "osaka.dram", .base = 0x00080000, .size = 2 * GiB - 512 * KiB, .window = true },
        /* Partition decode windows (PART_AON/PERI/MAIN/VID/DDR 16MB, NPU 256MB). */
        { .name = "osaka.part_aon", .base = 0x80000000, .size = 16 * MiB, .window = true },
        { .name = "osaka.part_peri",.base = 0x81000000, .size = 16 * MiB, .window = true },
        { .name = "osaka.part_main",.base = 0x82000000, .size = 16 * MiB, .window = true },
        { .name = "osaka.part_vid", .base = 0x83000000, .size = 16 * MiB, .window = true },
        { .name = "osaka.part_ddr", .base = 0x84000000, .size = 16 * MiB, .window = true },
        { .name = "osaka.part_npu", .base = 0x90000000, .size = 256 * MiB, .window = true },
        /* I2C0: RAM-backed (wins over part_peri) so driver init settles. */
        { .name = "osaka.i2c0", .base = 0x81030000, .size = 4 * KiB },
        /* AON watchdog page (wdt 0x80010800 + wdt_stable 0x80010C00 share it). */
        { .name = "osaka.aon_wdt", .base = 0x80010000, .size = 4 * KiB },
        /* PART_AON.SYSRAM / PART_AON.CM0_ROM (CM7 read-view, no 0x0 remap) /
         * PART_MAIN.SYSRAM / PART_MAIN.CA55_ROM. */
        { .name = "osaka.aon_sysram",  .base = 0xF0000000, .size = 64 * KiB },
        { .name = "osaka.cm0_rom",     .base = 0xF0018000, .size = 32 * KiB },
        { .name = "osaka.main_sysram", .base = 0xFFFE0000, .size = 64 * KiB },
        { .name = "osaka.ca55_rom",    .base = 0xFFFF0000, .size = 32 * KiB },
    },
    .num_ram = 15,
    .uart = {
        { .base = 0x811c0000, .regshift = 2, .nvic_irq = 85, .baudbase = 24000000 },
        { .base = 0x811d0000, .regshift = 2, .nvic_irq = 86, .baudbase = 24000000 },
        { .base = 0x811e0000, .regshift = 2, .nvic_irq = 87, .baudbase = 24000000 },
    },
    .num_uart = 3,
    /* Cross-core doorbells. Only the CM7 pair reaches this core's NVIC: channel 0
     * of the non-secure bank is line 120 and of the secure bank line 136, per the
     * cpvs doorbell unit test. The rest are data-accessible only. */
    .doorbell = {
        { "osaka.db_cm0",         0x80060000,  4, 64, -1 },
        { "osaka.db_cm0_secure",  0x80061000,  4, 32, -1 },
        { "osaka.db_cm7",         0x81130000, 16, 32, 120 },
        { "osaka.db_cm7_secure",  0x81131000,  4, 16, 136 },
        { "osaka.db_nsp",         0x81132000, 16, 16, -1 },
        { "osaka.db_nsp_secure",  0x81133000,  4, 16, -1 },
        { "osaka.db_ca55",        0x82B00000, 16, 32, -1 },
        { "osaka.db_ca55_secure", 0x82B01000,  4, 16, -1 },
    },
    .num_doorbell = 8,
    .tzc_base = 0x84010000,
    .tzpc_base = 0x80030000,
    .kernel_load_base = 0x00000000,
    .kernel_load_size = 256 * KiB,
    .mpu_dregion = 16,   /* Osaka M7 = 16 PMSAv7 regions (explicit, not QEMU default) */
};

/* CM0 memory map per Confluence Sentry/Memory Map (id 2881789520), CM0 column.
 * ROM 32K@0xF0018000 aliased to 0x0 (hw remap) where rom-resident tests run;
 * partition windows are low-priority executable RAM (ARM B3-2) so the decode
 * matches the SoC and embedded RAM/devices win. XN is enforced by the ptw.c
 * osaka-cm0-xn patch (architectural default map). */
static const QsimSocDesc osaka_cm0_desc = {
    .cpu_type = ARM_CPU_TYPE_NAME("cortex-m0"),
    .num_irq  = 32,
    .ram = {
        /* PART_AON.SYSRAM / PART_AON.CM0_ROM (remapped to 0x0). */
        { .name = "osaka.aon_sysram", .base = 0xF0000000, .size = 64 * KiB },
        { .name = "osaka.cm0_rom",    .base = 0xF0018000, .size = 32 * KiB,
          .has_alias = true, .alias_base = 0x00000000 },
        /* DRAM 2GB @ 0x0 (SoT range 0x8000_0000); low priority so the ROM alias overlays the bottom. */
        { .name = "osaka.dram", .base = 0x00000000, .size = 2 * GiB,  .window = true },
        /* M7_TCM external view (CM0 sees the CM7 TCM here) / PART_MAIN.SYSRAM /
         * PART_MAIN.CA55_ROM. */
        { .name = "osaka.m7_tcm",      .base = 0xF0020000, .size = 512 * KiB },
        { .name = "osaka.main_sysram", .base = 0xFFFE0000, .size = 64 * KiB },
        { .name = "osaka.ca55_rom",    .base = 0xFFFF0000, .size = 32 * KiB },
        /* Partition decode windows (PART_AON/PERI/MAIN/VID/DDR 16MB, NPU 256MB). */
        { .name = "osaka.part_aon", .base = 0x80000000, .size = 16 * MiB, .window = true },
        { .name = "osaka.part_peri",.base = 0x81000000, .size = 16 * MiB, .window = true },
        { .name = "osaka.part_main",.base = 0x82000000, .size = 16 * MiB, .window = true },
        { .name = "osaka.part_vid", .base = 0x83000000, .size = 16 * MiB, .window = true },
        { .name = "osaka.part_ddr", .base = 0x84000000, .size = 16 * MiB, .window = true },
        { .name = "osaka.part_npu", .base = 0x90000000, .size = 256 * MiB, .window = true },
        /* AON watchdog page (wdt 0x80010800 + wdt_stable 0x80010C00 share it). */
        { .name = "osaka.aon_wdt", .base = 0x80010000, .size = 4 * KiB },
    },
    .num_ram = 13,
    /* Console UART1 @ 0x811d0000 = serial_hd(0) = qrun stdio; polled by cpvs printf. */
    .uart = {
        { .base = 0x811d0000, .regshift = 2, .nvic_irq = 2, .baudbase = 24000000 },
    },
    .num_uart = 1,
    /* Only the CM0 pair reaches this core's NVIC: non-secure channel 0 is line 14
     * and secure channel 0 is line 18, per the cpvs doorbell unit test. */
    .doorbell = {
        { "osaka.db_cm0",         0x80060000,  4, 64, 14 },
        { "osaka.db_cm0_secure",  0x80061000,  4, 32, 18 },
        { "osaka.db_cm7",         0x81130000, 16, 32, -1 },
        { "osaka.db_cm7_secure",  0x81131000,  4, 16, -1 },
        { "osaka.db_nsp",         0x81132000, 16, 16, -1 },
        { "osaka.db_nsp_secure",  0x81133000,  4, 16, -1 },
        { "osaka.db_ca55",        0x82B00000, 16, 32, -1 },
        { "osaka.db_ca55_secure", 0x82B01000,  4, 16, -1 },
    },
    .num_doorbell = 8,
    .tzc_base = 0x84010000,
    .tzpc_base = 0x80030000,
    .kernel_load_base = 0xF0018000,
    .kernel_load_size = 32 * KiB,
    /* ARMv6-M has no VTOR: vectors fixed at 0x0 (the ROM alias). */
    .init_vtor = 0x0,
    /* rom-stub=on baremetal flavour: the real cm0_rom.S image (poll DATA0 for
     * 0xCA55CA55, trace 0xC0, hand MSP/PC to the FW table at 0x20000000). */
    .rom_stub = qsim_cm0_rom,
    .rom_stub_size = sizeof(qsim_cm0_rom),
    .rom_stub_base = 0xF0018000,
    .rom_flag_base = 0x80060018,
    .rom_flag_magic = 0xCA55CA55,
};

static void osaka_cm7_init(MachineState *machine)
{
    qsim_soc_init(machine, &osaka_cm7_desc);
}

static void osaka_cm0_init(MachineState *machine)
{
    qsim_soc_init(machine, &osaka_cm0_desc);
}

static void osaka_cm7_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-m7"), NULL
    };

    mc->desc = "Qualcomm Osaka Cortex-M7";
    mc->init = osaka_cm7_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-m7");
    mc->valid_cpu_types = valid_cpu_types;
    mc->max_cpus = 1;
}

static void osaka_cm0_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-m0"), NULL
    };

    mc->desc = "Qualcomm Osaka Cortex-M0";
    mc->init = osaka_cm0_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-m0");
    mc->valid_cpu_types = valid_cpu_types;
    mc->max_cpus = 1;
}

static const TypeInfo osaka_machine_types[] = {
    {
        .name       = MACHINE_TYPE_NAME("osaka-cm7"),
        .parent     = TYPE_QSIM_MACHINE,
        .class_init = osaka_cm7_class_init,
        QSIM_MACHINE_IFACES
    },
    {
        .name       = MACHINE_TYPE_NAME("osaka-cm0"),
        .parent     = TYPE_QSIM_MACHINE,
        .class_init = osaka_cm0_class_init,
        QSIM_MACHINE_IFACES
    },
};

DEFINE_TYPES(osaka_machine_types)
