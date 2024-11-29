/*
 * Emulation of xilinx zynq 7000 XADC interface
 *
 * Copyright (c) 2024 Michael Davidsaver
 *
 * This code is licensed under the GNU GPL v2.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */
/* https://docs.amd.com/r/en-US/ug480_7Series_XADC/7-Series-FPGAs-and-Zynq-7000-SoC-XADC-Dual-12-Bit-1-MSPS-Analog-to-Digital-Converter-User-Guide-UG480
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/irq.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "qom/object.h"


#define TYPE_ZYNQ7_XADC "zynq7.xadc"
OBJECT_DECLARE_SIMPLE_TYPE(ZYNQ7XADCState, ZYNQ7_XADC)

#define ZYNQ7_XADC_MEM_SIZE 0x4B0

static uint32_t XSysMon_TemperatureToRaw(float Temperature)
{
    return (Temperature + 273.15f)*65536.0f*0.00198421639f;
}

//static uint32_t XSysMon_VoltageToRaw(float Voltage)
//{
//    return Voltage*65536.0f/3.0f;
//}

struct ZYNQ7XADCState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t reg[ZYNQ7_XADC_MEM_SIZE / 4u];
};

static void zynq7_xadc_soft_reset(ZYNQ7XADCState *s)
{
    s->reg[0x200 / 4u] = XSysMon_TemperatureToRaw(25.25);

    // TODO: zero MIN and 0xffff to MAX
}

static void zynq7_xadc_reset(DeviceState *dev)
{
    ZYNQ7XADCState *s = ZYNQ7_XADC(dev);

    memset(s->reg, 0, sizeof(s->reg));
    zynq7_xadc_soft_reset(s);
}

static uint64_t zynq7_xadc_read(void *opaque, hwaddr offset, unsigned size)
{
    ZYNQ7XADCState *s = ZYNQ7_XADC(opaque);

    return s->reg[offset / 4u];
}

static void zynq7_xadc_write(void *opaque, hwaddr offset,
                             uint64_t value, unsigned size)
{
    ZYNQ7XADCState *s = ZYNQ7_XADC(opaque);

    s->reg[offset / 4u] = value;
}

static const MemoryRegionOps zynq7_xadc_ops = {
    .read = zynq7_xadc_read,
    .write = zynq7_xadc_write,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void zynq7_xadc_realize(DeviceState *dev, Error **errp)
{
    ZYNQ7XADCState *s = ZYNQ7_XADC(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &zynq7_xadc_ops, s, TYPE_ZYNQ7_XADC,
                          ZYNQ7_XADC_MEM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static void zynq7_xadc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, zynq7_xadc_reset);
    dc->realize = zynq7_xadc_realize;
    dc->desc = "Xilinx XIIC I2C Controller";
}

static const TypeInfo zynq7_xadc_type_info = {
    .name = TYPE_ZYNQ7_XADC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ZYNQ7XADCState),
    .class_init = zynq7_xadc_class_init,
};

static void zynq7_xadc_register_types(void)
{
    type_register_static(&zynq7_xadc_type_info);
}

type_init(zynq7_xadc_register_types)
