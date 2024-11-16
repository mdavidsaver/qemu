/*
 * Emulation of xilinx XIIC interface
 *
 * Copyright (c) 2024 Michael Davidsaver
 *
 * This code is licensed under the GNU GPL v2.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */
/* https://docs.amd.com/v/u/en-US/pg090-axi-iic
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/fifo8.h"
#include "hw/irq.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/i2c/i2c.h"
#include "qom/object.h"

#if 0
#define DPRINTF(fmt, args...) \
    do { \
            fprintf(stderr, "[%s]%s:%d: " fmt , TYPE_XILINX_I2C, \
                                             __func__, __LINE__, ##args); \
    } while (0)
#else
#define DPRINTF(fmt, args...) do{}while(0)
#endif

#define XILINX_I2C_MEM_SIZE 0x128

#define XR(S, NAME) (S)->reg[XIIC_ ## NAME ## _REG_OFFSET / 4u]

#define XIIC_DGIER_REG_OFFSET	0x1C  /**< Global Interrupt Enable Register */
#define XIIC_IISR_REG_OFFSET	0x20  /**< Interrupt Status Register */
#define XIIC_IIER_REG_OFFSET	0x28  /**< Interrupt Enable Register */
#define XIIC_RESETR_REG_OFFSET	0x40  /**< Reset Register */
#define XIIC_CR_REG_OFFSET	0x100 /**< Control Register */
#define XIIC_SR_REG_OFFSET	0x104 /**< Status Register */
#define XIIC_DTR_REG_OFFSET	0x108 /**< Data Tx Register */
#define XIIC_DRR_REG_OFFSET	0x10C /**< Data Rx Register */
#define XIIC_ADR_REG_OFFSET	0x110 /**< Address Register */
#define XIIC_TFO_REG_OFFSET	0x114 /**< Tx FIFO Occupancy */
#define XIIC_RFO_REG_OFFSET	0x118 /**< Rx FIFO Occupancy */
#define XIIC_TBA_REG_OFFSET	0x11C /**< 10 Bit Address reg */
#define XIIC_RFD_REG_OFFSET	0x120 /**< Rx FIFO Depth reg */
#define XIIC_GPO_REG_OFFSET	0x124 /**< Output Register */

static inline const char* xr_name(hwaddr offset) {
    switch(offset) {
#define CASE(NAME) case XIIC_ ## NAME ## _REG_OFFSET: return #NAME
    CASE(DGIER);
    CASE(IISR);
    CASE(IIER);
    CASE(RESETR);
    CASE(CR);
    CASE(SR);
    CASE(DTR);
    CASE(DRR);
    CASE(TFO);
    CASE(RFO);
    CASE(TBA);
    CASE(RFD);
    CASE(GPO);
#undef CASE
    default: return "?";
    }
}


/**
 * @name Device Global Interrupt Enable Register masks (CR) mask(s)
 * @{
 */
#define XIIC_GINTR_ENABLE_MASK	0x80000000 /**< Global Interrupt Enable Mask */
/* @} */

/** @name IIC Device Interrupt Status/Enable (INTR) Register Masks
 *
 * <b> Interrupt Status Register (IISR) </b>
 *
 * This register holds the interrupt status flags for the Spi device.
 *
 * <b> Interrupt Enable Register (IIER) </b>
 *
 * This register is used to enable interrupt sources for the IIC device.
 * Writing a '1' to a bit in this register enables the corresponding Interrupt.
 * Writing a '0' to a bit in this register disables the corresponding Interrupt.
 *
 * IISR/IIER registers have the same bit definitions and are only defined once.
 * @{
 */
#define XIIC_INTR_ARB_LOST_MASK	0x00000001 /**< 1 = Arbitration lost */
#define XIIC_INTR_TX_ERROR_MASK	0x00000002 /**< 1 = Tx error/msg complete */
#define XIIC_INTR_TX_EMPTY_MASK	0x00000004 /**< 1 = Tx FIFO/reg empty */
#define XIIC_INTR_RX_FULL_MASK	0x00000008 /**< 1 = Rx FIFO/reg=OCY level */
#define XIIC_INTR_BNB_MASK	0x00000010 /**< 1 = Bus not busy */
#define XIIC_INTR_AAS_MASK	0x00000020 /**< 1 = When addr as slave */
#define XIIC_INTR_NAAS_MASK	0x00000040 /**< 1 = Not addr as slave */
#define XIIC_INTR_TX_HALF_MASK	0x00000080 /**< 1 = Tx FIFO half empty */

/**
 * All Tx interrupts commonly used.
 */
#define XIIC_TX_INTERRUPTS	(XIIC_INTR_TX_ERROR_MASK | \
                 XIIC_INTR_TX_EMPTY_MASK |  \
                 XIIC_INTR_TX_HALF_MASK)

/**
 * All interrupts commonly used
 */
#define XIIC_TX_RX_INTERRUPTS	(XIIC_INTR_RX_FULL_MASK | XIIC_TX_INTERRUPTS)

/* @} */

/**
 * @name Reset Register mask
 * @{
 */
#define XIIC_RESET_MASK		0x0000000A /**< RESET Mask  */
/* @} */


/**
 * @name Control Register masks (CR) mask(s)
 * @{
 */
#define XIIC_CR_ENABLE_DEVICE_MASK	0x00000001 /**< Device enable = 1 */
#define XIIC_CR_TX_FIFO_RESET_MASK	0x00000002 /**< Transmit FIFO reset=1 */
#define XIIC_CR_MSMS_MASK		0x00000004 /**< Master starts Txing=1 */
#define XIIC_CR_DIR_IS_TX_MASK		0x00000008 /**< Dir of Tx. Txing=1 */
#define XIIC_CR_NO_ACK_MASK		0x00000010 /**< Tx Ack. NO ack = 1 */
#define XIIC_CR_REPEATED_START_MASK	0x00000020 /**< Repeated start = 1 */
#define XIIC_CR_GENERAL_CALL_MASK	0x00000040 /**< Gen Call enabled = 1 */
/* @} */

/**
 * @name Status Register masks (SR) mask(s)
 * @{
 */
#define XIIC_SR_GEN_CALL_MASK		0x00000001 /**< 1 = A Master issued
                            * a GC */
#define XIIC_SR_ADDR_AS_SLAVE_MASK	0x00000002 /**< 1 = When addressed as
                            * slave */
#define XIIC_SR_BUS_BUSY_MASK		0x00000004 /**< 1 = Bus is busy */
#define XIIC_SR_MSTR_RDING_SLAVE_MASK	0x00000008 /**< 1 = Dir: Master <--
                            * slave */
#define XIIC_SR_TX_FIFO_FULL_MASK	0x00000010 /**< 1 = Tx FIFO full */
#define XIIC_SR_RX_FIFO_FULL_MASK	0x00000020 /**< 1 = Rx FIFO full */
#define XIIC_SR_RX_FIFO_EMPTY_MASK	0x00000040 /**< 1 = Rx FIFO empty */
#define XIIC_SR_TX_FIFO_EMPTY_MASK	0x00000080 /**< 1 = Tx FIFO empty */
/* @} */

/**
 * @name Data Tx Register (DTR) mask(s)
 * @{
 */
#define XIIC_TX_DYN_START_MASK		0x00000100 /**< 1 = Set dynamic start */
#define XIIC_TX_DYN_STOP_MASK		0x00000200 /**< 1 = Set dynamic stop */
#define IIC_TX_FIFO_DEPTH		16     /**< Tx fifo capacity */
/* @} */

/**
 * @name Data Rx Register (DRR) mask(s)
 * @{
 */
#define IIC_RX_FIFO_DEPTH		16	/**< Rx fifo capacity */
/* @} */


#define XIIC_TX_ADDR_SENT		0x00
#define XIIC_TX_ADDR_MSTR_RECV_MASK	0x02


/**
 * The following constants are used to specify whether to do
 * Read or a Write operation on IIC bus.
 */
#define XIIC_READ_OPERATION	1 /**< Read operation on the IIC bus */
#define XIIC_WRITE_OPERATION	0 /**< Write operation on the IIC bus */

/**
 * The following constants are used with the transmit FIFO fill function to
 * specify the role which the IIC device is acting as, a master or a slave.
 */
#define XIIC_MASTER_ROLE	1 /**< Master on the IIC bus */
#define XIIC_SLAVE_ROLE		0 /**< Slave on the IIC bus */

/**
 * The following constants are used with Transmit Function (XIic_Send) to
 * specify whether to STOP after the current transfer of data or own the bus
 * with a Repeated start.
 */
#define XIIC_STOP		0x00 /**< Send a stop on the IIC bus after
                    * the current data transfer */
#define XIIC_REPEATED_START	0x01 /**< Donot Send a stop on the IIC bus after
                    * the current data transfer */

#define TYPE_XILINX_I2C "xilinx.i2c"
OBJECT_DECLARE_SIMPLE_TYPE(XILINXI2CState, XILINX_I2C)

struct XILINXI2CState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    I2CBus *bus;
    qemu_irq irq;

    uint32_t reg[XILINX_I2C_MEM_SIZE / 4u];

    Fifo8 rx;
};

static void xilinx_i2c_update_irq(XILINXI2CState *s)
{
    // TX FIFO not modeled, so always empty
    uint32_t active = XIIC_INTR_TX_EMPTY_MASK | XIIC_INTR_TX_HALF_MASK;

    if(XR(s, CR) & XIIC_CR_NO_ACK_MASK) {
        active |= XIIC_INTR_TX_ERROR_MASK;
    }
    if(!(XR(s, SR) & XIIC_SR_BUS_BUSY_MASK)) {
        active |= XIIC_INTR_BNB_MASK;
    }
    if(fifo8_num_used(&s->rx) >= (XR(s, RFD)+1)) {
        active |= XIIC_INTR_RX_FULL_MASK;
    }
    // slave operation not modeled

    {
        uint32_t raise = ~XR(s, IISR) & active;
        if(raise)
            DPRINTF("assert 0x%08x\n", raise);
    }

    XR(s, IISR) |= active;
    qemu_set_irq(s->irq,
                 (XR(s, DGIER) & XIIC_GINTR_ENABLE_MASK) && (XR(s, IISR) & XR(s, IIER)));
}

static void xilinx_i2c_reset(DeviceState *dev)
{
    XILINXI2CState *s = XILINX_I2C(dev);

    DPRINTF("reset\n");

    if(i2c_bus_busy(s->bus)) {
        i2c_end_transfer(s->bus);
    }

    fifo8_reset(&s->rx);

    memset(s->reg, 0, sizeof(s->reg));

    XR(s, SR) = XIIC_SR_RX_FIFO_EMPTY_MASK | XIIC_SR_TX_FIFO_EMPTY_MASK;

    xilinx_i2c_update_irq(s);
}

static void xilinx_i2c_recv_one(XILINXI2CState *s)
{
    uint8_t v = i2c_recv(s->bus);
    fifo8_push(&s->rx, v);
    DPRINTF("  push 0x%02x\n", v);
    if(XR(s, CR) & XIIC_CR_NO_ACK_MASK) {
        DPRINTF("  final\n");
    }
}

static void xilinx_i2c_do_start(XILINXI2CState *s)
{
    uint32_t addr = XR(s, DTR);
    DPRINTF("START 0x%02x\n", extract32(addr, 1, 7));
    int err = i2c_start_transfer(s->bus,
                                 extract32(addr, 1, 7),
                                 extract32(addr, 0, 1));

    XR(s, SR) |= XIIC_SR_BUS_BUSY_MASK;
    if(err) {
        DPRINTF("No slave ack!\n");
        // STOP
        XR(s, CR) &= ~XIIC_CR_MSMS_MASK;
        //XR(s, SR) &= ~XIIC_SR_BUS_BUSY_MASK; // ???
        XR(s, IISR) |= XIIC_INTR_ARB_LOST_MASK;

        if(XR(s, CR) & XIIC_CR_DIR_IS_TX_MASK) { // master TX
            XR(s, IISR) |= XIIC_INTR_TX_ERROR_MASK;
        }

    } else if(!(XR(s, CR) & XIIC_CR_DIR_IS_TX_MASK)) {
        uint32_t nread = (XR(s, RFD)&0xf)+1;
        DPRINTF("Read %u byte(s) / %u\n", nread, fifo8_num_used(&s->rx));
        while(fifo8_num_used(&s->rx) < nread) {
            xilinx_i2c_recv_one(s);
        }
    }

    xilinx_i2c_update_irq(s);
}

static uint64_t xilinx_i2c_read(void *opaque, hwaddr offset, unsigned size)
{
    uint64_t value;
    XILINXI2CState *s = XILINX_I2C(opaque);
    (void)size;

    value = s->reg[offset / 4u];

    switch(offset) {
    case XIIC_TFO_REG_OFFSET:
        value = 0;
        break;
    case XIIC_RFO_REG_OFFSET:
        value = (fifo8_num_used(&s->rx)-1u)&0xf;
        break;
    case XIIC_RESETR_REG_OFFSET:
    case XIIC_DTR_REG_OFFSET:
        // write only...
        value = 0xdeadbeef;
        break;
    case XIIC_DRR_REG_OFFSET:
        if(fifo8_num_used(&s->rx)) {
            value = fifo8_pop(&s->rx);
            xilinx_i2c_update_irq(s);
        } else {
            value = 0xffffffff;
        }
        if(XR(s, CR) & XIIC_CR_MSMS_MASK) {
            xilinx_i2c_recv_one(s);
        } else {
            DPRINTF("STOP\n");
            i2c_end_transfer(s->bus);
            XR(s, SR) &= ~XIIC_SR_BUS_BUSY_MASK;
        }
        break;
    }

    DPRINTF("read %s [0x%" HWADDR_PRIx "] -> 0x%02x\n", xr_name(offset), offset, (int)value);

    return value;
}

static void xilinx_i2c_write(void *opaque, hwaddr offset,
                          uint64_t value, unsigned size)
{
    XILINXI2CState *s = XILINX_I2C(opaque);
    uint64_t prev = s->reg[offset / 4u];
    (void)size;

    DPRINTF("write %s [0x%" HWADDR_PRIx "] <- 0x%02x\n",
            xr_name(offset), offset, (int)value);

    switch(offset) {
    case XIIC_SR_REG_OFFSET:
        // read-only
        return;
    case XIIC_IISR_REG_OFFSET:
        // write 1 to clear
        value = prev & ~value;
        break;
    case XIIC_DGIER_REG_OFFSET:
        value = 0;
        qemu_log_mask(LOG_UNIMP, "%s: interrupts not implemented\n",
                      __func__);
        break;
    case XIIC_RESETR_REG_OFFSET:
        if(extract32(value, 0, 4)==0x0a) {
            xilinx_i2c_reset(DEVICE(opaque));
        }
        return;
    case XIIC_CR_REG_OFFSET:
    case XIIC_DTR_REG_OFFSET:
        break;
    case XIIC_RFD_REG_OFFSET:
        value &= 0xf;
        break;
    case XIIC_GPO_REG_OFFSET:
        value = 0;
        // fall through
    default:
        qemu_log_mask(LOG_UNIMP, "%s: 0x%" HWADDR_PRIx " 0x%" PRIx64 "\n",
                      __func__, offset, value);
    }

    s->reg[offset / 4u] = value;

    switch(offset) {
    case XIIC_CR_REG_OFFSET:
        if(value & XIIC_CR_ENABLE_DEVICE_MASK) {

            uint64_t bset = ~prev & value;
            uint64_t bclr = prev & ~value;

            // MSMS is edge sensitive
            if(bclr & XIIC_CR_MSMS_MASK) {

            } else if(bset & XIIC_CR_MSMS_MASK) {
                xilinx_i2c_do_start(s);
            }
        }
        break;
    case XIIC_DTR_REG_OFFSET:

        if((value & XIIC_TX_DYN_START_MASK) || (XR(s, CR) & XIIC_CR_REPEATED_START_MASK)) {
            XR(s, CR) |= XIIC_CR_MSMS_MASK;
            XR(s, CR) &= ~XIIC_CR_REPEATED_START_MASK;
            // TODO: dyn start RX needs totake next DTR as byte count
            xilinx_i2c_do_start(s);

        } else if(i2c_bus_busy(s->bus)) {
            DPRINTF("SEND 0x%02lx\n", value);
            i2c_send(s->bus, value);

            if((value & XIIC_TX_DYN_STOP_MASK) || !(XR(s, CR) & XIIC_CR_MSMS_MASK)) {
                DPRINTF("STOP\n");
                i2c_end_transfer(s->bus);
                XR(s, SR) &= ~XIIC_SR_BUS_BUSY_MASK;
            }
        }

        break;
    default:
        break;
    }

    xilinx_i2c_update_irq(s);
}

static const MemoryRegionOps xilinx_i2c_ops = {
    .read = xilinx_i2c_read,
    .write = xilinx_i2c_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void xilinx_i2c_realize(DeviceState *dev, Error **errp)
{
    XILINXI2CState *s = XILINX_I2C(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &xilinx_i2c_ops, s, TYPE_XILINX_I2C,
                          XILINX_I2C_MEM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    s->bus = i2c_init_bus(dev, NULL);

    fifo8_create(&s->rx, 16);
}

static void xilinx_i2c_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, xilinx_i2c_reset);
    dc->realize = xilinx_i2c_realize;
    dc->desc = "Xilinx XIIC I2C Controller";
}

static const TypeInfo xilinx_i2c_type_info = {
    .name = TYPE_XILINX_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(XILINXI2CState),
    .class_init = xilinx_i2c_class_init,
};

static void xilinx_i2c_register_types(void)
{
    type_register_static(&xilinx_i2c_type_info);
}

type_init(xilinx_i2c_register_types)
