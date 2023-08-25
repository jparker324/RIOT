/*
 * Copyright (C) 2023 BISSELL Homecare, Inc.
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @ingroup     cpu_gd32e23x
 * @ingroup     drivers_periph_spi
 * @{
 *
 * @file
 * @brief       Low-level SPI driver implementation
 *
 * @author      Jason Parker <jason.parker@bissell.com>
 *
 * @}
 */

#include <assert.h>

#include "bitarithm.h"
#include "cpu.h"
#include "mutex.h"
#include "periph/gpio.h"
#include "periph/spi.h"
#include "pm_layered.h"

#define ENABLE_DEBUG        0
#include "debug.h"

/**
 * @brief   Number of bits to shift the BR value in the CTL0 register
 */
#define BR_SHIFT            (3U)
#define BR_MAX              (7U)

#ifdef SPI_CTL1_FRXTH
/* configure SPI for 8-bit data width */
#define SPI_CTL1_SETTINGS    (SPI_CTL1_FRXTH |\
                             SPI_CTL1_DS_0 |\
                             SPI_CTL1_DS_1 |\
                             SPI_CTL1_DS_2)
#else
#define SPI_CTL1_SETTINGS    0
#endif

/**
 * @brief   Allocate one lock per SPI device
 */
static mutex_t locks[SPI_NUMOF];

/**
 * @brief   Clock configuration cache
 */
static uint32_t clocks[SPI_NUMOF];

/**
 * @brief   Clock divider cache
 */
static uint8_t dividers[SPI_NUMOF];

static inline SPI_Type *dev(spi_t bus)
{
    return spi_config[bus].dev;
}

#ifdef MODULE_PERIPH_DMA
static inline bool _use_dma(const spi_conf_t *conf)
{
    return conf->tx_dma != DMA_STREAM_UNDEF && conf->rx_dma != DMA_STREAM_UNDEF;
}
#endif

/**
 * @brief Multiplier for clock divider calculations
 *
 * Makes the divider calculation fixed point
 */
#define SPI_APB_CLOCK_SHIFT          (4U)
#define SPI_APB_CLOCK_MULT           (1U << SPI_APB_CLOCK_SHIFT)

static uint8_t _get_clkdiv(const spi_conf_t *conf, uint32_t clock)
{
    uint32_t bus_clock = periph_apb_clk(conf->apbbus);
    /* Shift bus_clock with SPI_APB_CLOCK_SHIFT to create a fixed point int */
    uint32_t div = (bus_clock << SPI_APB_CLOCK_SHIFT) / (2 * clock);
    DEBUG("[spi] clock: divider: %"PRIu32"\n", div);
    /* Test if the divider is 2 or smaller, keeping the fixed point in mind */
    if (div <= SPI_APB_CLOCK_MULT) {
        return 0;
    }
    /* determine MSB and compensate back for the fixed point int shift */
    uint8_t rounded_div = bitarithm_msb(div) - SPI_APB_CLOCK_SHIFT;
    /* Determine if rounded_div is not a power of 2 */
    if ((div & (div - 1)) != 0) {
        /* increment by 1 to ensure that the clock speed at most the
         * requested clock speed */
        rounded_div++;
    }
    return rounded_div > BR_MAX ? BR_MAX : rounded_div;
}

void spi_init(spi_t bus)
{
    assert(bus < SPI_NUMOF);

    /* initialize device lock */
    mutex_init(&locks[bus]);
    /* trigger pin initialization */
    spi_init_pins(bus);

    periph_clk_en(spi_config[bus].apbbus, spi_config[bus].rcumask);
    /* reset configuration */
    dev(bus)->CTL0 = 0;
#ifdef SPI_I2SCFGR_I2SE
    dev(bus)->I2SCFGR = 0;
#endif
    dev(bus)->CTL1 = SPI_CTL1_SETTINGS;
    periph_clk_dis(spi_config[bus].apbbus, spi_config[bus].rcumask);
}

void spi_init_pins(spi_t bus)
{
    if (gpio_is_valid(spi_config[bus].mosi_pin)) {
        gpio_init(spi_config[bus].mosi_pin, GPIO_OUT);
        gpio_init_af(spi_config[bus].mosi_pin, spi_config[bus].mosi_af);
    }

    if (gpio_is_valid(spi_config[bus].miso_pin)) {
        gpio_init(spi_config[bus].miso_pin, GPIO_IN);
        gpio_init_af(spi_config[bus].miso_pin, spi_config[bus].miso_af);
    }

    if (gpio_is_valid(spi_config[bus].sclk_pin)) {
        gpio_init(spi_config[bus].sclk_pin, GPIO_OUT);
        gpio_init_af(spi_config[bus].sclk_pin, spi_config[bus].sclk_af);
    }
}

int spi_init_cs(spi_t bus, spi_cs_t cs)
{
    if (bus >= SPI_NUMOF) {
        return SPI_NODEV;
    }
    if (!gpio_is_valid(cs) ||
        (((cs & SPI_HWCS_MASK) == SPI_HWCS_MASK) && (cs & ~(SPI_HWCS_MASK)))) {
        return SPI_NOCS;
    }

    if (cs == SPI_HWCS_MASK) {
        if (!gpio_is_valid(spi_config[bus].cs_pin)) {
            return SPI_NOCS;
        }
        gpio_init(spi_config[bus].cs_pin, GPIO_OUT);
        gpio_init_af(spi_config[bus].cs_pin, spi_config[bus].cs_af);
    }
    else {
        gpio_init((gpio_t)cs, GPIO_OUT);
        gpio_set((gpio_t)cs);
    }

    return SPI_OK;
}

#ifdef MODULE_PERIPH_SPI_GPIO_MODE
int spi_init_with_gpio_mode(spi_t bus, const spi_gpio_mode_t* mode)
{
    assert(bus < SPI_NUMOF);

    int ret = 0;

    if (gpio_is_valid(spi_config[bus].mosi_pin)) {
        ret += gpio_init(spi_config[bus].mosi_pin, mode->mosi);
        gpio_init_af(spi_config[bus].mosi_pin, spi_config[bus].mosi_af);
    }

    if (gpio_is_valid(spi_config[bus].miso_pin)) {
        ret += gpio_init(spi_config[bus].miso_pin, mode->miso);
        gpio_init_af(spi_config[bus].miso_pin, spi_config[bus].miso_af);
    }

    if (gpio_is_valid(spi_config[bus].sclk_pin)) {
        ret += gpio_init(spi_config[bus].sclk_pin, mode->sclk);
        gpio_init_af(spi_config[bus].sclk_pin, spi_config[bus].sclk_af);
    }
    return ret;
}
#endif

void spi_acquire(spi_t bus, spi_cs_t cs, spi_mode_t mode, spi_clk_t clk)
{
    assert((unsigned)bus < SPI_NUMOF);

    /* lock bus */
    mutex_lock(&locks[bus]);
// #ifdef STM32_PM_STOP
//     /* block STOP mode */
//     pm_block(STM32_PM_STOP);
// #endif
    /* enable SPI device clock */
    periph_clk_en(spi_config[bus].apbbus, spi_config[bus].rcumask);
    /* enable device */
    if (clk != clocks[bus]) {
        dividers[bus] = _get_clkdiv(&spi_config[bus], clk);
        clocks[bus] = clk;
    }
    uint8_t br = dividers[bus];

    DEBUG("[spi] acquire: requested clock: %"PRIu32", resulting clock: %"PRIu32
          " BR divider: %u\n",
          clk,
          periph_apb_clk(spi_config[bus].apbbus)/(1 << (br + 1)),
          br);

    uint16_t ctl0_settings = ((br << BR_SHIFT) | mode | SPI0_CTL0_MSTMOD_Msk);
    /* Settings to add to CTL1 in addition to SPI_CTL1_SETTINGS */
    uint16_t ctl1_extra_settings = 0;
    if (cs != SPI_HWCS_MASK) {
        ctl0_settings |= (SPI0_CTL0_SWNSSEN_Msk | SPI0_CTL0_SWNSS_Msk);
    }
    else {
        ctl1_extra_settings = (SPI0_CTL1_NSSDRV_Msk);
    }

#ifdef MODULE_PERIPH_DMA
    if (_use_dma(&spi_config[bus])) {
        ctl1_extra_settings |= SPI_CTL1_TXDMAEN | SPI_CTL1_RXDMAEN;

        dma_acquire(spi_config[bus].tx_dma);
        dma_setup(spi_config[bus].tx_dma,
                  spi_config[bus].tx_dma_chan,
                  (uint32_t*)&(dev(bus)->DATA),
                  DMA_MEM_TO_PERIPH,
                  DMA_DATA_WIDTH_BYTE,
                  0);

        dma_acquire(spi_config[bus].rx_dma);
        dma_setup(spi_config[bus].rx_dma,
                  spi_config[bus].rx_dma_chan,
                  (uint32_t*)&(dev(bus)->DATA),
                  DMA_PERIPH_TO_MEM,
                  DMA_DATA_WIDTH_BYTE,
                  0);
    }
#endif
    dev(bus)->CTL0 = ctl0_settings;
    /* Only modify CTL1 if needed */
    if (ctl1_extra_settings) {
        dev(bus)->CTL1 = (SPI_CTL1_SETTINGS | ctl1_extra_settings);
    }
}

void spi_release(spi_t bus)
{
#ifdef MODULE_PERIPH_DMA
    if (_use_dma(&spi_config[bus])) {
        dma_release(spi_config[bus].tx_dma);
        dma_release(spi_config[bus].rx_dma);
    }
#endif
    /* disable device and release lock */
    dev(bus)->CTL0 = 0;
    dev(bus)->CTL1 = SPI_CTL1_SETTINGS; /* Clear the DMA and SSOE flags */
    periph_clk_dis(spi_config[bus].apbbus, spi_config[bus].rcumask);
#ifdef GD32_PM_STOP
    /* unblock STOP mode */
    pm_unblock(GD32_PM_STOP);
#endif
    mutex_unlock(&locks[bus]);
}

static inline void _wait_for_end(spi_t bus)
{
    /* make sure the transfer is completed before continuing, see reference
     * manual(s) -> section 'Disabling the SPI' */
    while (!(dev(bus)->STAT & SPI0_STAT_TBE_Msk)) {}
    while (dev(bus)->STAT & SPI0_STAT_TRANS_Msk) {}
}

#ifdef MODULE_PERIPH_DMA
static void _transfer_dma(spi_t bus, const void *out, void *in, size_t len)
{
    uint8_t tmp = 0;

    if (out) {
        dma_prepare(spi_config[bus].tx_dma, (void*)out, len, 1);
    }
    else {
        dma_prepare(spi_config[bus].tx_dma, &tmp, len, 0);
    }
    if (in) {
        dma_prepare(spi_config[bus].rx_dma, in, len, 1);
    }
    else {
        dma_prepare(spi_config[bus].rx_dma, &tmp, len, 0);
    }

    /* Start RX first to ensure it is active before the SPI transfers are
     * triggered by the TX dma activity */
    dma_start(spi_config[bus].rx_dma);
    dma_start(spi_config[bus].tx_dma);

    dma_wait(spi_config[bus].rx_dma);
    dma_wait(spi_config[bus].tx_dma);

#ifdef DMA_CCR_EN
    dma_stop(spi_config[bus].rx_dma);
    dma_stop(spi_config[bus].tx_dma);
#endif
    _wait_for_end(bus);
}
#endif

static void _transfer_no_dma(spi_t bus, const void *out, void *in, size_t len)
{
    const uint8_t *outbuf = out;
    uint8_t *inbuf = in;

    /* we need to recast the data register to uint_8 to force 8-bit access */
    volatile uint8_t *DATA = (volatile uint8_t*)&(dev(bus)->DATA);

    /* transfer data, use shortpath if only sending data */
    if (!inbuf) {
        for (size_t i = 0; i < len; i++) {
            while (!(dev(bus)->STAT & SPI0_STAT_TBE_Msk));
            *DATA = outbuf[i];
        }
        /* wait until everything is finished and empty the receive buffer */
        while (!(dev(bus)->STAT & SPI0_STAT_TBE_Msk)) {}
        while (dev(bus)->STAT & SPI0_STAT_TRANS_Msk) {}
        while (dev(bus)->STAT & SPI0_STAT_RBNE_Msk) {
            dev(bus)->DATA;       /* we might just read 2 bytes at once here */
        }
    }
    else if (!outbuf) {
        for (size_t i = 0; i < len; i++) {
            while (!(dev(bus)->STAT & SPI0_STAT_TBE_Msk));
            *DATA = 0;
            while (!(dev(bus)->STAT & SPI0_STAT_RBNE_Msk));
            inbuf[i] = *DATA;
        }
    }
    else {
        for (size_t i = 0; i < len; i++) {
            while (!(dev(bus)->STAT & SPI0_STAT_TBE_Msk));
            *DATA = outbuf[i];
            while (!(dev(bus)->STAT & SPI0_STAT_RBNE_Msk));
            inbuf[i] = *DATA;
        }
    }

    _wait_for_end(bus);
}

void spi_transfer_bytes(spi_t bus, spi_cs_t cs, bool cont,
                        const void *out, void *in, size_t len)
{
    /* make sure at least one input or one output buffer is given */
    assert(out || in);

    /* active the given chip select line */
    dev(bus)->CTL0 |= (SPI0_CTL0_SPIEN_Msk);     /* this pulls the HW CS line low */
    if ((cs != SPI_HWCS_MASK) && gpio_is_valid(cs)) {
        gpio_clear((gpio_t)cs);
    }

#ifdef MODULE_PERIPH_DMA
    if (_use_dma(&spi_config[bus])) {
        _transfer_dma(bus, out, in, len);
    }
    else {
#endif
    _transfer_no_dma(bus, out, in, len);
#ifdef MODULE_PERIPH_DMA
    }
#endif

    /* release the chip select if not specified differently */
    if ((!cont) && gpio_is_valid(cs)) {
        dev(bus)->CTL0 &= ~(SPI0_CTL0_SPIEN_Msk);    /* pull HW CS line high */
        if (cs != SPI_HWCS_MASK) {
            gpio_set((gpio_t)cs);
        }
    }
}
