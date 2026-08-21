#ifndef REGS_H
#define REGS_H

/* ISP_CTRL block, AXI4-Lite, base 0xA0000000, 4K */

#define ISP_BASE        0xA0000000
#define ISP_SIZE        0x1000

#define ISP_ID          0x000
#define ISP_VERSION     0x004
#define ISP_CTRL        0x008
#define ISP_STATUS      0x00C
#define ISP_GAIN        0x010
#define ISP_OFFSET      0x014
#define ISP_FRAME_W     0x018
#define ISP_FRAME_H     0x01C
#define ISP_FRAME_CNT   0x020
#define ISP_IRQ_EN      0x024
#define ISP_IRQ_STAT    0x028
#define ISP_IRQ_CLR     0x02C
#define ISP_TEMP_MC     0x030

#define ISP_ID_MAGIC    0x49535031  /* "ISP1" */

/* CTRL bits */
#define CTRL_ENABLE     (1u << 0)
#define CTRL_NUC_EN     (1u << 1)
#define CTRL_SW_RESET   (1u << 2)
#define CTRL_TEST_PAT   (1u << 8)

/* STATUS bits */
#define STAT_RUNNING    (1u << 0)
#define STAT_FRAME_DONE (1u << 1)
#define STAT_OVERFLOW   (1u << 4)
#define STAT_UNDERFLOW  (1u << 5)

#define GAIN_MASK       0x0FFF

#endif
