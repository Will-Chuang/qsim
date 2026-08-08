/*
 * Copyright (c) 2026 Qualcomm Technologies, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Shared scaffolding for Qualcomm M-core QEMU machines (see qsim_soc.h).
 * Builds an armv7m container + RAM/ROM-alias + ns16550 UARTs from a per-chip
 * QsimSocDesc. Stock QEMU parts only; no new device type.
 */
#include "qsim_soc.h"   /* also supplies the headers QEMU 11.x relocated */
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/char/serial-mm.h"
#include "hw/arm/boot.h"
#include "system/system.h"
#include "qemu/timer.h"
#include "qemu/host-utils.h"

/* armv7m cpuclk/refclk; matches osaka_evk_cm7.dts cpu0 clock-frequency (12 MHz FPGA). */
#define QSIM_SYSCLK_HZ  12000000
#define QSIM_REFCLK_HZ  1000000

static void qsim_make_ram(MemoryRegion *parent, const QsimRamDesc *r)
{
    MemoryRegion *mr = g_new(MemoryRegion, 1);

    memory_region_init_ram(mr, NULL, r->name, r->size, &error_fatal);
    if (r->window) {
        /* Background partition fill: low priority so RAM/devices mapped inside
         * the window win; executable per ARM B3-2 (0x80000000-0x9FFFFFFF). */
        memory_region_add_subregion_overlap(parent, r->base, mr, -1);
        return;
    }
    memory_region_add_subregion(parent, r->base, mr);

    if (r->has_alias) {
        MemoryRegion *alias = g_new(MemoryRegion, 1);
        g_autofree char *aname = g_strdup_printf("%s.alias", r->name);

        memory_region_init_alias(alias, NULL, aname, mr, 0, r->size);
        memory_region_add_subregion(parent, r->alias_base, alias);
    }
}

/* ---- doorbell mailbox ---------------------------------------------------- */

#define DB_MASK_OFF    0x00
#define DB_CLR_OFF     0x04
#define DB_TRIGGER_OFF 0x08
#define DB_ST_OFF      0x0C
#define DB_PERIOD_OFF  0x10
#define DB_MODE_OFF    0x14
#define DB_DATA0_OFF   0x18

#define DB_MODE_LEVEL   0
#define DB_MODE_ONESHOT 1

typedef struct QsimDoorbell {
    MemoryRegion mr;
    uint32_t mask;
    uint32_t st;
    uint32_t mode;
    uint32_t period;
    uint32_t data[QSIM_MAX_DB_DATA];
    int num_channel;
    int num_data;
    int num_line;
    qemu_irq line[QSIM_MAX_DB_CHAN];
} QsimDoorbell;

/* Level mode holds the line while ST is set and the channel is unmasked; the
 * NVIC then refuses a pending clear per R_CVJS. One-shot drives no level. */
static void qsim_doorbell_refresh(QsimDoorbell *d)
{
    uint32_t active = (d->mode == DB_MODE_LEVEL) ? (d->st & ~d->mask) : 0;
    int i;

    for (i = 0; i < d->num_line; i++) {
        qemu_set_irq(d->line[i], (active >> i) & 1);
    }
}

static uint64_t qsim_doorbell_read(void *opaque, hwaddr addr, unsigned size)
{
    QsimDoorbell *d = opaque;

    switch (addr) {
    case DB_MASK_OFF:
        return d->mask;
    case DB_ST_OFF:
        return d->st;
    case DB_PERIOD_OFF:
        return d->period;
    case DB_MODE_OFF:
        return d->mode;
    case DB_CLR_OFF:
    case DB_TRIGGER_OFF:
        return 0;
    default:
        break;
    }

    if (addr >= DB_DATA0_OFF) {
        hwaddr idx = (addr - DB_DATA0_OFF) / 4;

        if (idx < (hwaddr)d->num_data) {
            return d->data[idx];
        }
    }
    return 0;
}

static void qsim_doorbell_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    QsimDoorbell *d = opaque;
    uint32_t chan_mask = (d->num_channel >= 32) ? 0xFFFFFFFFu
                                                : ((1u << d->num_channel) - 1u);
    uint32_t fired;
    int i;

    switch (addr) {
    case DB_MASK_OFF:
        d->mask = (uint32_t)val;
        qsim_doorbell_refresh(d);
        return;
    case DB_CLR_OFF:
        d->st &= ~(uint32_t)val;
        qsim_doorbell_refresh(d);
        return;
    case DB_TRIGGER_OFF:
        /* A masked channel sets neither ST nor pending. */
        fired = (uint32_t)val & chan_mask & ~d->mask;
        d->st |= fired;
        if (d->mode == DB_MODE_ONESHOT) {
            for (i = 0; i < d->num_line; i++) {
                if (fired & (1u << i)) {
                    qemu_set_irq(d->line[i], 1);
                    qemu_set_irq(d->line[i], 0);
                }
            }
        } else {
            qsim_doorbell_refresh(d);
        }
        return;
    case DB_PERIOD_OFF:
        d->period = (uint32_t)val & 0xFFu;
        return;
    case DB_MODE_OFF:
        d->mode = (uint32_t)val;
        qsim_doorbell_refresh(d);
        return;
    default:
        break;
    }

    if (addr >= DB_DATA0_OFF) {
        hwaddr idx = (addr - DB_DATA0_OFF) / 4;

        if (idx < (hwaddr)d->num_data) {
            d->data[idx] = (uint32_t)val;
        }
    }
}

static const MemoryRegionOps qsim_doorbell_ops = {
    .read = qsim_doorbell_read,
    .write = qsim_doorbell_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static QsimDoorbell *qsim_make_doorbell(MemoryRegion *parent,
                                        DeviceState *armv7m,
                                        const QsimDoorbellDesc *k)
{
    QsimDoorbell *d = g_new0(QsimDoorbell, 1);
    int i;

    d->num_channel = k->num_channel;
    d->num_data = MIN(k->num_data, QSIM_MAX_DB_DATA);
    d->num_line = (k->irq_base >= 0) ? MIN(k->num_channel, QSIM_MAX_DB_CHAN) : 0;
    for (i = 0; i < d->num_line; i++) {
        d->line[i] = qdev_get_gpio_in(armv7m, k->irq_base + i);
    }

    memory_region_init_io(&d->mr, NULL, &qsim_doorbell_ops, d, k->name, 4 * KiB);
    memory_region_add_subregion(parent, k->base, &d->mr);
    return d;
}

/* ---- TZC-400 filter + config -------------------------------------------- */

#define TZC_GATE_KEEPER_OFF 0x0008
#define TZC_BLOCK_SIZE      (8 * KiB)

/* Storage-backed except GATE_KEEPER, whose open_status [19:16] mirrors the
 * open request [3:0]; osaka_tzc_open_gate() polls exactly that field. */
typedef struct QsimTzc {
    MemoryRegion mr;
    uint32_t reg[TZC_BLOCK_SIZE / 4];
} QsimTzc;

static uint64_t qsim_tzc_read(void *opaque, hwaddr addr, unsigned size)
{
    QsimTzc *t = opaque;
    uint32_t v = t->reg[addr / 4];

    if (addr == TZC_GATE_KEEPER_OFF) {
        return (v & 0xFu) | ((v & 0xFu) << 16);
    }
    return v;
}

static void qsim_tzc_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    QsimTzc *t = opaque;

    t->reg[addr / 4] = (uint32_t)val;
}

static const MemoryRegionOps qsim_tzc_ops = {
    .read = qsim_tzc_read,
    .write = qsim_tzc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* ---- TZPC peripheral security ------------------------------------------- */

#define TZPC_PROT0_STAT 0x0800
#define TZPC_BLOCK_SIZE (4 * KiB)

/* Three DEC_PROT banks, each STAT / SET / CLR at +0x0 / +0x4 / +0x8, stride
 * 0x0C, matching the offsets src/trustzone/trustzone.c programs. */
typedef struct QsimTzpc {
    MemoryRegion mr;
    uint32_t prot[3];
} QsimTzpc;

static int qsim_tzpc_slot(hwaddr addr, int *reg)
{
    hwaddr off;

    if (addr < TZPC_PROT0_STAT) {
        return -1;
    }
    off = addr - TZPC_PROT0_STAT;
    if (off >= 3 * 0x0C || (off % 4) != 0) {
        return -1;
    }
    *reg = (int)(off % 0x0C) / 4;
    return (int)(off / 0x0C);
}

static uint64_t qsim_tzpc_read(void *opaque, hwaddr addr, unsigned size)
{
    QsimTzpc *t = opaque;
    int bank, reg;

    bank = qsim_tzpc_slot(addr, &reg);
    if (bank < 0 || reg != 0) {
        return 0;
    }
    return t->prot[bank];
}

static void qsim_tzpc_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    QsimTzpc *t = opaque;
    int bank, reg;

    bank = qsim_tzpc_slot(addr, &reg);
    if (bank < 0) {
        return;
    }
    if (reg == 1) {
        t->prot[bank] |= (uint32_t)val & 0xFFu;
    } else if (reg == 2) {
        t->prot[bank] &= ~((uint32_t)val & 0xFFu);
    }
}

static const MemoryRegionOps qsim_tzpc_ops = {
    .read = qsim_tzpc_read,
    .write = qsim_tzpc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* DWT at 0xe0001000. Stock QEMU leaves it inside the PPB default region, so
 * every register reads 0: NUMCOMP=0 fatals Zephyr null-pointer detection and a
 * CYCCNT that never advances hangs the cpvs delay_ns() spin. */

#define QSIM_DWT_CTRL          0x000
#define QSIM_DWT_CYCCNT        0x004
#define QSIM_DWT_NUMCOMP_M7    (4u << 28)
#define QSIM_DWT_CYCCNTENA     (1u << 0)
#define QSIM_DWT_CTRL_WRITABLE 0x0FFFFFFFu

typedef struct QsimDwt {
    uint32_t ctrl;
    Clock   *cpuclk;
} QsimDwt;

static uint64_t qsim_dwt_read(void *opaque, hwaddr addr, unsigned size)
{
    QsimDwt *d = opaque;
    uint64_t hz;

    switch (addr) {
    case QSIM_DWT_CTRL:
        return QSIM_DWT_NUMCOMP_M7 | (d->ctrl & QSIM_DWT_CTRL_WRITABLE);
    case QSIM_DWT_CYCCNT:
        if (!(d->ctrl & QSIM_DWT_CYCCNTENA)) {
            return 0;
        }
        /* Derived from virtual time, so it advances with the guest. */
        hz = d->cpuclk ? clock_get_hz(d->cpuclk) : 0;
        return (uint32_t)muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), hz,
                                  NANOSECONDS_PER_SECOND);
    default:
        return 0;
    }
}

static void qsim_dwt_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    QsimDwt *d = opaque;

    if (addr == QSIM_DWT_CTRL) {
        d->ctrl = (uint32_t)val & QSIM_DWT_CTRL_WRITABLE;
    }
}

static const MemoryRegionOps qsim_dwt_ops = {
    .read = qsim_dwt_read,
    .write = qsim_dwt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* The PPB default region sits at priority -1, so priority 0 wins here without
 * touching armv7m.c. Runs after realize, when the container exists. */
static void qsim_make_dwt(QsimSocState *s)
{
    MemoryRegion *mr = g_new(MemoryRegion, 1);
    QsimDwt *d = g_new0(QsimDwt, 1);

    d->cpuclk = s->sysclk;
    memory_region_init_io(mr, NULL, &qsim_dwt_ops, d, "qsim.dwt", 0x1000);
    memory_region_add_subregion_overlap(&s->armv7m.container, 0xe0001000, mr, 0);
}

void qsim_soc_init(MachineState *machine, const QsimSocDesc *desc)
{
    QsimSocState *s = QSIM_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    DeviceState *armv7m;
    int i;

    s->sysclk = clock_new(OBJECT(machine), "SYSCLK");
    clock_set_hz(s->sysclk, QSIM_SYSCLK_HZ);
    s->refclk = clock_new(OBJECT(machine), "REFCLK");
    clock_set_hz(s->refclk, QSIM_REFCLK_HZ);

    for (i = 0; i < desc->num_ram; i++) {
        qsim_make_ram(sysmem, &desc->ram[i]);
    }

    object_initialize_child(OBJECT(s), "armv7m", &s->armv7m, TYPE_ARMV7M);
    armv7m = DEVICE(&s->armv7m);
    qdev_prop_set_uint32(armv7m, "num-irq", desc->num_irq);
    qdev_prop_set_string(armv7m, "cpu-type", machine->cpu_type);
    /* Pin MPU region count (Osaka M7 = 16); armv7m forwards this to cpu pmsav7-dregion. */
    if (desc->mpu_dregion) {
        qdev_prop_set_uint32(armv7m, "mpu-ns-regions", desc->mpu_dregion);
    }
    qdev_connect_clock_in(armv7m, "cpuclk", s->sysclk);
    qdev_connect_clock_in(armv7m, "refclk", s->refclk);
    object_property_set_link(OBJECT(&s->armv7m), "memory",
                             OBJECT(sysmem), &error_abort);
    /* Non-zero VTOR boots from a SRAM view; 0 = reset-from-0x0 ROM path. */
    if (desc->init_vtor) {
        qdev_prop_set_uint32(armv7m, "init-nsvtor", desc->init_vtor);
    }
    sysbus_realize(SYS_BUS_DEVICE(&s->armv7m), &error_fatal);

    qsim_make_dwt(s);

    for (i = 0; i < desc->num_uart; i++) {
        const QsimUartDesc *u = &desc->uart[i];

        serial_mm_init(sysmem, u->base, u->regshift,
                       qdev_get_gpio_in(armv7m, u->nvic_irq),
                       u->baudbase, serial_hd(i), DEVICE_LITTLE_ENDIAN);
    }

    for (i = 0; i < desc->num_doorbell; i++) {
        QsimDoorbell *d = qsim_make_doorbell(sysmem, armv7m,
                                             &desc->doorbell[i]);

        /* Baremetal flavour: the boot ROM polls a doorbell DATA word for the
         * release magic before handing off. The releasing core does not exist
         * in a single-core machine, so model "already released" by seeding the
         * flag; the FW consumes (clears) it right after handoff. */
        if (s->rom_stub && desc->rom_flag_base &&
            desc->rom_flag_base >= desc->doorbell[i].base + DB_DATA0_OFF) {
            hwaddr idx = (desc->rom_flag_base -
                          desc->doorbell[i].base - DB_DATA0_OFF) / 4;

            if (idx < (hwaddr)d->num_data) {
                d->data[idx] = desc->rom_flag_magic;
            }
        }
    }

    if (desc->tzc_base) {
        QsimTzc *t = g_new0(QsimTzc, 1);

        memory_region_init_io(&t->mr, NULL, &qsim_tzc_ops, t, "osaka.tzc",
                              TZC_BLOCK_SIZE);
        memory_region_add_subregion(sysmem, desc->tzc_base, &t->mr);
    }

    if (desc->tzpc_base) {
        QsimTzpc *t = g_new0(QsimTzpc, 1);

        memory_region_init_io(&t->mr, NULL, &qsim_tzpc_ops, t, "osaka.tzpc",
                              TZPC_BLOCK_SIZE);
        memory_region_add_subregion(sysmem, desc->tzpc_base, &t->mr);
    }

    /* Baremetal flavour: -kernel is the FW (linked in SRAM); the real boot ROM
     * blob goes to its home base, whose 0x0 alias serves the reset vectors.
     * Off by default so a ROM-code pattern loaded by -kernel cannot collide. */
    if (s->rom_stub && desc->rom_stub) {
        rom_add_blob_fixed("qsim.rom-stub", desc->rom_stub,
                           desc->rom_stub_size, desc->rom_stub_base);
    }

    armv7m_load_kernel(s->armv7m.cpu, machine->kernel_filename,
                       desc->kernel_load_base, desc->kernel_load_size);
}

static bool qsim_get_rom_stub(Object *obj, Error **errp)
{
    return QSIM_MACHINE(obj)->rom_stub;
}

static void qsim_set_rom_stub(Object *obj, bool value, Error **errp)
{
    QSIM_MACHINE(obj)->rom_stub = value;
}

static void qsim_soc_instance_init(Object *obj)
{
    object_property_add_bool(obj, "rom-stub",
                             qsim_get_rom_stub, qsim_set_rom_stub);
    object_property_set_description(obj, "rom-stub",
        "Install the chip's boot ROM blob; -kernel then carries baremetal FW");
}

static const TypeInfo qsim_soc_machine_info = {
    .name          = TYPE_QSIM_MACHINE,
    .parent        = TYPE_MACHINE,
    .abstract      = true,
    .instance_size = sizeof(QsimSocState),
    .instance_init = qsim_soc_instance_init,
};

static void qsim_soc_register_types(void)
{
    type_register_static(&qsim_soc_machine_info);
}

type_init(qsim_soc_register_types);
