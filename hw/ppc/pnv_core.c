/*
 * QEMU PowerPC PowerNV CPU Core model
 *
 * Copyright (c) 2016, IBM Corporation.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#include "qemu/osdep.h"
#include "sysemu/sysemu.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "target/ppc/cpu.h"
#include "hw/ppc/ppc.h"
#include "hw/ppc/pnv.h"
#include "hw/ppc/pnv_core.h"
#include "hw/ppc/pnv_xscom.h"
#include "hw/ppc/xics.h"

static const char *pnv_core_cpu_typename(PnvCore *pc)
{
    const char *core_type = object_class_get_name(object_get_class(OBJECT(pc)));
    int len = strlen(core_type) - strlen(PNV_CORE_TYPE_SUFFIX);
    char *s = g_strdup_printf(POWERPC_CPU_TYPE_NAME("%.*s"), len, core_type);
    const char *cpu_type = object_class_get_name(object_class_by_name(s));
    g_free(s);
    return cpu_type;
}

static void powernv_cpu_reset(void *opaque)
{
    PowerPCCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);
    CPUPPCState *env = &cpu->env;

    cpu_reset(cs);

    /*
     * the skiboot firmware elects a primary thread to initialize the
     * system and it can be any.
     */
    env->gpr[3] = PNV_FDT_ADDR;
    env->nip = 0x10;
    env->msr |= MSR_HVB; /* Hypervisor mode */
}

static void powernv_cpu_init(PowerPCCPU *cpu, Error **errp)
{
    CPUPPCState *env = &cpu->env;
    int core_pir;
    int thread_index = 0; /* TODO: TCG supports only one thread */
    ppc_spr_t *pir = &env->spr_cb[SPR_PIR];

    core_pir = object_property_get_uint(OBJECT(cpu), "core-pir", &error_abort);

    /*
     * The PIR of a thread is the core PIR + the thread index. We will
     * need to find a way to get the thread index when TCG supports
     * more than 1. We could use the object name ?
     */
    pir->default_value = core_pir + thread_index;

    /* Set time-base frequency to 512 MHz */
    cpu_ppc_tb_init(env, PNV_TIMEBASE_FREQ);

    qemu_register_reset(powernv_cpu_reset, cpu);
}

/*
 * These values are read by the PowerNV HW monitors under Linux
 */
#define PNV_XSCOM_EX_DTS_RESULT0     0x50000
#define PNV_XSCOM_EX_DTS_RESULT1     0x50001

static uint64_t pnv_core_xscom_read(void *opaque, hwaddr addr,
                                    unsigned int width)
{
    uint32_t offset = addr >> 3;
    uint64_t val = 0;

    /* The result should be 38 C */
    switch (offset) {
    case PNV_XSCOM_EX_DTS_RESULT0:
        val = 0x26f024f023f0000ull;
        break;
    case PNV_XSCOM_EX_DTS_RESULT1:
        val = 0x24f000000000000ull;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "Warning: reading reg=0x%" HWADDR_PRIx,
                  addr);
    }

    return val;
}

static void pnv_core_xscom_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned int width)
{
    qemu_log_mask(LOG_UNIMP, "Warning: writing to reg=0x%" HWADDR_PRIx,
                  addr);
}

static const MemoryRegionOps pnv_core_xscom_ops = {
    .read = pnv_core_xscom_read,
    .write = pnv_core_xscom_write,
    .valid.min_access_size = 8,
    .valid.max_access_size = 8,
    .impl.min_access_size = 8,
    .impl.max_access_size = 8,
    .endianness = DEVICE_BIG_ENDIAN,
};

static void pnv_core_realize_child(Object *child, XICSFabric *xi, Error **errp)
{
    Error *local_err = NULL;
    CPUState *cs = CPU(child);
    PowerPCCPU *cpu = POWERPC_CPU(cs);

    object_property_set_bool(child, true, "realized", &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    cpu->intc = icp_create(child, TYPE_PNV_ICP, xi, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    powernv_cpu_init(cpu, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }
}

static void pnv_core_realize(DeviceState *dev, Error **errp)
{
    PnvCore *pc = PNV_CORE(OBJECT(dev));
    CPUCore *cc = CPU_CORE(OBJECT(dev));
    const char *typename = pnv_core_cpu_typename(pc);
    size_t size = object_type_get_instance_size(typename);
    Error *local_err = NULL;
    void *obj;
    int i, j;
    char name[32];
    Object *xi;

    xi = object_property_get_link(OBJECT(dev), "xics", &local_err);
    if (!xi) {
        error_setg(errp, "%s: required link 'xics' not found: %s",
                   __func__, error_get_pretty(local_err));
        return;
    }

    pc->threads = g_malloc0(size * cc->nr_threads);
    for (i = 0; i < cc->nr_threads; i++) {
        obj = pc->threads + i * size;

        object_initialize(obj, size, typename);

        snprintf(name, sizeof(name), "thread[%d]", i);
        object_property_add_child(OBJECT(pc), name, obj, &local_err);
        object_property_add_alias(obj, "core-pir", OBJECT(pc),
                                  "pir", &local_err);
        if (local_err) {
            goto err;
        }
        object_unref(obj);
    }

    for (j = 0; j < cc->nr_threads; j++) {
        obj = pc->threads + j * size;

        pnv_core_realize_child(obj, XICS_FABRIC(xi), &local_err);
        if (local_err) {
            goto err;
        }
    }

    snprintf(name, sizeof(name), "xscom-core.%d", cc->core_id);
    pnv_xscom_region_init(&pc->xscom_regs, OBJECT(dev), &pnv_core_xscom_ops,
                          pc, name, PNV_XSCOM_EX_CORE_SIZE);
    return;

err:
    while (--i >= 0) {
        obj = pc->threads + i * size;
        object_unparent(obj);
    }
    g_free(pc->threads);
    error_propagate(errp, local_err);
}

static Property pnv_core_properties[] = {
    DEFINE_PROP_UINT32("pir", PnvCore, pir, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void pnv_core_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = pnv_core_realize;
    dc->props = pnv_core_properties;
}

#define DEFINE_PNV_CORE_TYPE(cpu_model)         \
    {                                           \
        .parent = TYPE_PNV_CORE,                \
        .name = PNV_CORE_TYPE_NAME(cpu_model),  \
    }

static const TypeInfo pnv_core_infos[] = {
    {
        .name           = TYPE_PNV_CORE,
        .parent         = TYPE_CPU_CORE,
        .instance_size  = sizeof(PnvCore),
        .class_size     = sizeof(PnvCoreClass),
        .class_init = pnv_core_class_init,
        .abstract       = true,
    },
    DEFINE_PNV_CORE_TYPE("power8e_v2.1"),
    DEFINE_PNV_CORE_TYPE("power8_v2.0"),
    DEFINE_PNV_CORE_TYPE("power8nvl_v1.0"),
    DEFINE_PNV_CORE_TYPE("power9_v2.0"),
};

DEFINE_TYPES(pnv_core_infos)
