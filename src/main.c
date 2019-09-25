/*
 * main.c
 * 
 * Bootstrap the STM32F103C8T6 and get things moving.
 * 
 * Written & released by Keir Fraser <keir.xen@gmail.com>
 * 
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

/*
 * PIN ASSIGNMENTS:
 * 
 * FF OSD I2C Special Protocol (use with FlashFloppy v3.4a or later):
 *  A0-A1: Jumper/Strap
 * 
 * Rotary Encoder:
 *  A0: CLK
 *  A1: DAT
 *  A2: SEL
 * [NB. Rotary Encoder is unavailable if A0-A1 is jumpered, but FF OSD
 *      can be configured via FlashFloppy]
 * 
 * Serial Console:
 *  A9: TX
 *  A10: RX
 * 
 * I2C Interface (to Gotek):
 *  B6: CLK
 *  B7: DAT
 * 
 * Buttons (Gotek):
 *  A3: PREV/LEFT/DOWN
 *  A4: NEXT/RIGHT/UP
 *  A5: SELECT/EJECT
 * 
 * Display:
 *  A8: CSYNC or HSYNC
 *  B14: VSYNC (only needed with HSYNC)
 *  B15: Display output
 * 
 * Amiga keyboard:
 *  B3: KBDAT
 *  B4: KBCLK
 */

/* CSYNC/HSYNC (A8): EXTI IRQ trigger and TIM1 Ch.1 trigger. */
#define gpio_csync gpioa
#define pin_csync  8
#define irq_csync  23
void IRQ_23(void) __attribute__((alias("IRQ_csync"))); /* EXTI9_5 */

/* VSYNC (B14): EXTI IRQ trigger. */
#define gpio_vsync gpiob
#define pin_vsync  14
#define irq_vsync  40
void IRQ_40(void) __attribute__((alias("IRQ_vsync"))); /* EXTI15_10 */

/* TIM1 Ch.3: Triggered at horizontal end of OSD box. 
 * TIM1 counter is started by TIM2 UEV (ie. when SPI DMA begins). */
#define tim1_ch3_dma (dma1->ch6)
#define tim1_ch3_dma_ch 6
#define tim1_ch3_dma_tc_irq 16
void IRQ_16(void) __attribute__((alias("IRQ_osd_end")));

/* TIM1 Ch.4: Triggered 1us before TIM1 Ch.3. Generates IRQ. */
#define tim1_cc_irq 27
void IRQ_27(void) __attribute__((alias("IRQ_osd_pre_end")));

/* TIM2: Ch.1 Output Compare triggers IRQ. Overflow triggers SPI DMA. 
 * Counter starts on TIM1 UEV (itself triggered by TIM1 Ch.1 input pin). */
#define tim2_irq 28
void IRQ_28(void) __attribute__((alias("IRQ_osd_pre_start")));
#define tim2_up_dma (dma1->ch2)
#define tim2_up_dma_ch 2
#define tim2_up_dma_tc_irq 12

/* TIM3: Overflow triggers DMA to enable Display Output.
 * Counter starts on TIM1 UEV (itself triggered by TIM1 Ch.1 input pin). */
#define tim3_irq 29
#define tim3_up_dma (dma1->ch3)
#define tim3_up_dma_ch 3
#define tim3_up_dma_tc_irq 13

/* Display Output (B15): Pixels are generated by SPI. */
#define gpio_display gpiob
#define pin_display  15
#define spi_display  (spi2)
#define dma_display  (dma1->ch5)
#define dma_display_ch 5
#define dma_display_irq 15

/* List of interrupts used by the display-sync and -output system. */
const static uint8_t irqs[] = {
    tim1_cc_irq, tim2_irq, tim1_ch3_dma_tc_irq, irq_csync, irq_vsync
};

int EXC_reset(void) __attribute__((alias("main")));

/* Are we using the FF OSD custom I2C protocol? */
bool_t ff_osd_i2c_protocol;

#include "font.h"

/* Guard the stacks with known values. */
static void canary_init(void)
{
    _irq_stackbottom[0] = _thread_stackbottom[0] = 0xdeadbeef;
}

/* Has either stack been clobbered? */
static void canary_check(void)
{
    ASSERT(_irq_stackbottom[0] == 0xdeadbeef);
    ASSERT(_thread_stackbottom[0] == 0xdeadbeef);
}

static struct timer button_timer;
static uint8_t rotary;
static volatile uint8_t buttons;

static uint8_t get_buttons(void)
{
    /* Rotary encoder outputs a Gray code, counting clockwise: 00-01-11-10. */
    enum { ROT_none, ROT_full, ROT_half, ROT_quarter } rotary_type = ROT_full;
    const uint32_t rotary_transitions[] = {
        [ROT_none]    = 0x00000000, /* No encoder */
        [ROT_full]    = 0x20000100, /* 4 transitions (full cycle) per detent */
        [ROT_half]    = 0x24000018, /* 2 transitions (half cycle) per detent */
        [ROT_quarter] = 0x24428118  /* 1 transition (quarter cyc) per detent */
    };

    static uint16_t _b;
    uint8_t b = 0;

    /* We debounce the switch by waiting for it to be pressed continuously 
     * for 16 consecutive sample periods (16 * 5ms == 80ms) */
    _b <<= 1;
    _b |= gpio_read_pin(gpioa, 2);
    if (_b == 0)
        b |= B_SELECT;

    rotary = ((rotary << 2) | (gpioa->idr & 3)) & 15;
    b |= (rotary_transitions[rotary_type] >> (rotary << 1)) & 3;

    return b;
}

static void button_timer_fn(void *unused)
{
    uint8_t b = B_PROCESSED;

    /* Rotary Encoder is not supported with FF OSD custom I2C protocol. */
    if (!ff_osd_i2c_protocol)
        b |= get_buttons();

    /* Latch final button state and reset the timer. */
    buttons |= b;
    timer_set(&button_timer, button_timer.deadline + time_ms(5));
}

static int hline, frame;
#define HLINE_EOF -1
#define HLINE_VBL 0
#define HLINE_SOF 1

#define MAX_DISPLAY_HEIGHT 52
static uint16_t display_dat[MAX_DISPLAY_HEIGHT][40/2+1];
static struct display *cur_display = &lcd_display;
static uint16_t display_height;

static void slave_arr_update(void)
{
    unsigned int hstart = config.h_off * 20;

    /* Enable output pin first (TIM3) and then start SPI transfers (TIM2). */
    tim2->arr = hstart-1;
    tim3->arr = hstart-49;

    /* Trigger TIM2 IRQ 1us before OSD box. */
    tim2->ccr1 = hstart - sysclk_us(1);
}

static void set_polarity(void)
{
    if (config.polarity) {
        /* Active High: Rising edge = sync start */
        exti->ftsr &= ~(m(pin_csync) | m(pin_vsync));
        exti->rtsr |= m(pin_csync) | m(pin_vsync); /* Rising edge */
        tim1->ccer |= TIM_CCER_CC1P; /* Falling edge */
    } else {
        /* Active Low: Falling edge = sync start */
        exti->rtsr &= ~(m(pin_csync) | m(pin_vsync));
        exti->ftsr |= m(pin_csync) | m(pin_vsync); /* Falling edge */
        tim1->ccer &= ~TIM_CCER_CC1P; /* Rising edge */
    }
}

static void IRQ_vsync(void)
{
    exti->pr = m(pin_vsync);
    tim1->smcr = 0;
    hline = HLINE_VBL;
}

static void IRQ_csync(void)
{
    exti->pr = m(pin_csync);

    if (hline <= 0) { /* EOF or VBL */

        static time_t p;
        time_t t = time_now();

        /* Trigger on both sync edges so we can measure sync pulse width: 
         * Normal Sync ~= 5us, Porch+Data ~= 59us */
        exti->ftsr |= m(pin_csync) | m(pin_vsync);
        exti->rtsr |= m(pin_csync) | m(pin_vsync);

        if (gpio_read_pin(gpio_csync, pin_csync) == config.polarity) {

            /* Sync pulse start: remember the current time. */
            p = t;

        } else if (time_diff(p, t) > time_us(10)) {

            /* Long sync: We are in vblank. */
            hline = HLINE_VBL;

        } else if (hline == HLINE_VBL) {

            /* Short sync: We are outside the vblank period. Start frame (we
             * were previously in vblank). */
            if (display_height == 0)
                goto eof;
            hline = HLINE_SOF;
            slave_arr_update();
            set_polarity();

        }

    } else if (++hline < config.v_off) {

        /* Before vertical start of OSD: Do nothing. */

    } else if (hline >= (config.v_off + display_height)) {

    eof:
        /* Vertical end of OSD: Disable TIM1 trigger and signal main loop. */
        tim1->smcr = 0;
        hline = HLINE_EOF;
        frame++;

    } else {

        /* Within OSD vertical area: Set up for next line. */

        /* Set TIM1 to reset (causing UEV) when triggered by Ch.1 input pin
         * (Ch.1 input pin is CSYNC/HSYNC, triggering on end-of-sync). */
        tim1->smcr = (TIM_SMCR_MSM
                      | TIM_SMCR_TS(5) /* Filtered TI1 */
                      | TIM_SMCR_SMS(4)); /* Reset Mode */

        if (hline == config.v_off) {
            /* Set up for first line of OSD box. */
            dma_display.cmar = (uint32_t)(unsigned long)display_dat;
        }


    }
}

#define OSD_OFF 0
#define OSD_ON 1
static uint32_t gpio_display_crh[2]; /* OSD_??? */
static uint16_t dma_display_ccr = (DMA_CCR_PL_V_HIGH |
                                   DMA_CCR_MSIZE_16BIT |
                                   DMA_CCR_PSIZE_16BIT |
                                   DMA_CCR_MINC |
                                   DMA_CCR_DIR_M2P |
                                   DMA_CCR_EN);

/* Triggered by TIM2 1us before the start of the OSD box. We use this to 
 * quiesce interrupts during the critical initial OSD DMAs. We also retask
 * TIM1 to cleanly finish the OSD box at end of line. */
static void IRQ_osd_pre_start(void)
{
    /* Set TIM1 to start counting when triggered by TIM2. Output-compare 
     * will trigger DMA to disable OSD output at end of line. */
    tim1->smcr = (TIM_SMCR_TS(1) /* Timer 2 */
                  | TIM_SMCR_SMS(6)); /* Trigger Mode (starts counter) */

    tim2->sr = 0;
    delay_us(1);
}

/* Triggered by TIM1's Ch.4 Output Compare. */
static void IRQ_osd_pre_end(void)
{
    tim1->sr = 0;
    delay_us(1);
}

/* Triggered by TIM1's DMA completion at horizontal end of OSD box. */
static void IRQ_osd_end(void)
{
    /* Clear interrupt and stop timer. */
    dma1->ifcr = DMA_IFCR_CGIF(tim1_ch3_dma_ch);
    tim1->cr1 &= ~TIM_CR1_CEN;

    /* Point SPI DMA at next line of data. */
    dma_display.ccr = 0;
    dma_display.cndtr = cur_display->cols/2 + 1;
    dma_display.cmar += sizeof(display_dat[0]);
}

/* Set up a slave timer to be triggered by TIM1. */
static void setup_slave_timer(TIM tim)
{
    tim->psc = 0;
    tim->egr = TIM_EGR_UG; /* update CNT, PSC, ARR */
    tim->cr2 = 0;
    tim->dier = TIM_DIER_UDE;
    tim->cr1 = TIM_CR1_ARPE | TIM_CR1_URS | TIM_CR1_OPM;
    tim->smcr = (TIM_SMCR_TS(0) /* Timer 1 */
                 | TIM_SMCR_SMS(6)); /* Trigger Mode (starts counter) */
}

static void render_line(int y, const struct display *display)
{
    unsigned int x, row;
    const uint8_t *t;
    uint16_t *d = display_dat[y];

    memset(d, 0, sizeof(display_dat[0]));

    /* Top two lines are blank. */
    y -= 2;

    /* Work out which text row we are on. */
    for (row = 0; row < display->rows; row++) {
        int nr = (display->heights & (1u<<row)) ? 16 : 8;
        if (y < 0)
            return;
        if (y < nr)
            break;
        y -= nr + 2; /* Two blank lines between each row of text. */
    }

    /* Done all rows? Final two lines are blank. */
    if (row >= display->rows)
        return;

    /* If this is a double-height row, each pixel line is repeated. */
    if (display->heights & (1u<<row))
        y /= 2;

    t = display->text[row];

    for (x = 0; x < display->cols; x++) {
        uint8_t c = *t++;
        if ((c < 0x20) || (c > 0x7f))
            c = 0x20;
        c -= 0x20;
        d[x/2] |= (uint16_t)font[(c<<3)+y] << ((x&1)?0:8);
    }
}

/* We snapshot the relevant Amiga keys so that we can scan the keymap (and 
 * clear the sticky bits) in one place in the main loop. */
static uint8_t keys;
#define K_LEFT   B_LEFT
#define K_RIGHT  B_RIGHT
#define K_SELECT B_SELECT
#define K_MENU   8

static void update_amiga_keys(void)
{
    keys = 0;
    if (amiga_key_pressed(AMI_LEFT)) keys |= K_LEFT;
    if (amiga_key_pressed(AMI_RIGHT)) keys |= K_RIGHT;
    if (amiga_key_pressed(AMI_UP)) keys |= K_SELECT;
    if (amiga_key_pressed(AMI_HELP)) keys |= K_MENU;
}

struct gotek_button {
    bool_t pressed;
    time_t t;
} gl, gr, gs;

static bool_t gotek_active;
static void emulate_gotek_button(
    uint8_t keycode, struct gotek_button *button, int pin)
{
    bool_t pressed = (keys & keycode) && gotek_active;
    if (!(pressed ^ button->pressed))
        return; /* no change */
    if (pressed) {
        button->t = time_now();
        button->pressed = TRUE;
        gpio_write_pin(gpioa, pin, LOW);
    } else if (time_diff(button->t, time_now()) > time_ms(200)) {
        button->pressed = FALSE;
        gpio_write_pin(gpioa, pin, HIGH);
    }
}

static void emulate_gotek_buttons(void)
{
    if (config_active)
        gotek_active = FALSE;
    else if (!gotek_active && !keys)
        gotek_active = TRUE; /* only after keys are released */
    emulate_gotek_button(K_LEFT, &gl, 3);
    emulate_gotek_button(K_RIGHT, &gr, 4);
    emulate_gotek_button(K_SELECT, &gs, 5);
}

static struct display notify;
static time_t notify_time;

int main(void)
{
    int i;
    time_t frame_time;
    bool_t lost_sync, _keyboard_held;

    /* Relocate DATA. Initialise BSS. */
    if (_sdat != _ldat)
        memcpy(_sdat, _ldat, _edat-_sdat);
    memset(_sbss, 0, _ebss-_sbss);

    canary_init();

    stm32_init();
    time_init();
    console_init();

    ff_osd_i2c_protocol = gpio_pins_connected(gpioa, 0, gpioa, 1);
    lcd_init();

    /* PC13: Blue Pill Indicator LED (Active Low) */
    gpio_configure_pin(gpioc, 13, GPI_pull_up);

    /* PA0, PA1, PA2: Rotary encoder */
    for (i = 0; i < 3; i++)
        gpio_configure_pin(gpioa, i, GPI_pull_up);

    /* PA8 = CSYNC/HSYNC input */
    gpio_configure_pin(gpio_csync, pin_csync, GPI_pull_up);

    /* PB14 = VSYNC input */
    gpio_configure_pin(gpio_vsync, pin_vsync, GPI_pull_up);

    /* PB15 = Colour output */
    gpio_configure_pin(gpio_display, pin_display, GPI_floating);

    /* PA3,4,5: Gotek buttons */
    gpio_configure_pin(gpioa, 3, GPO_opendrain(_2MHz, HIGH));
    gpio_configure_pin(gpioa, 4, GPO_opendrain(_2MHz, HIGH));
    gpio_configure_pin(gpioa, 5, GPO_opendrain(_2MHz, HIGH));

    /* Turn on the clocks. */
    rcc->apb1enr |= (RCC_APB1ENR_SPI2EN
                     | RCC_APB1ENR_TIM2EN
                     | RCC_APB1ENR_TIM3EN);
    rcc->apb2enr |= RCC_APB2ENR_TIM1EN;

    config_init();

    /* Configure SPI: 8-bit mode, MSB first, CPOL Low, CPHA Leading Edge. */
    spi_display->cr2 = SPI_CR2_TXDMAEN;
    spi_display->cr1 = (SPI_CR1_MSTR | /* master */
                        SPI_CR1_SSM | SPI_CR1_SSI | /* software NSS */
                        SPI_CR1_SPE | /* enable */
                        SPI_CR1_DFF | /* 16-bit */
                        SPI_CR1_CPHA |
                        SPI_CR1_BR_DIV4); /* 9MHz */

    /* Display DMA setup: From memory into the Display Timer's CCRx. */
    dma_display.cpar = (uint32_t)(unsigned long)&spi_display->dr;

    /* PA8 -> EXTI8 ; PB14 -> EXTI14 */
    afio->exticr4 |= 0x0100;
    exti->imr |= m(pin_csync) | m(pin_vsync);

    /* Timer 2 is triggered by Timer 1. On overflow it triggers DMA 
     * to start SPI transfer for the current hline. */
    tim2_up_dma.cpar = (uint32_t)(unsigned long)&dma_display.ccr;
    tim2_up_dma.cmar = (uint32_t)(unsigned long)&dma_display_ccr;
    tim2_up_dma.cndtr = 1;
    tim2_up_dma.ccr = (DMA_CCR_PL_V_HIGH |
                       DMA_CCR_MSIZE_16BIT |
                       DMA_CCR_PSIZE_32BIT |
                       DMA_CCR_CIRC |
                       DMA_CCR_DIR_M2P |
                       DMA_CCR_EN);
    setup_slave_timer(tim2);

    /* Timer 3 is triggered by Timer 1. On overflow it triggers DMA 
     * to switch on the SPI output pin. */
    gpio_configure_pin(gpio_display, pin_display, AFO_pushpull(_50MHz));
    gpio_display_crh[OSD_ON] = gpio_display->crh;
    gpio_configure_pin(gpio_display, pin_display, GPI_floating);
    gpio_display_crh[OSD_OFF] = gpio_display->crh;
    tim3_up_dma.cpar = (uint32_t)(unsigned long)&gpio_display->crh;
    tim3_up_dma.cmar = (uint32_t)(unsigned long)&gpio_display_crh[OSD_ON];
    tim3_up_dma.cndtr = 1;
    tim3_up_dma.ccr = (DMA_CCR_PL_V_HIGH |
                       DMA_CCR_MSIZE_32BIT |
                       DMA_CCR_PSIZE_32BIT |
                       DMA_CCR_CIRC |
                       DMA_CCR_DIR_M2P |
                       DMA_CCR_EN);
    setup_slave_timer(tim3);

    /* Timer 2 interrupts us horizontally just before the OSD box, so that 
     * we can pause I2C IRQ transfers. */
    tim2->ccmr1 = TIM_CCMR1_CC1S(TIM_CCS_OUTPUT);
    tim2->ccer = TIM_CCER_CC1E;
    tim2->dier |= TIM_DIER_CC1IE;

    /* CSYNC is on Timer 1 Channel 1. Use it to trigger Timer 2 and 3. */
    tim1->psc = 0;
    tim1->arr = 0xffff;
    tim1->ccmr1 = TIM_CCMR1_CC1S(TIM_CCS_INPUT_TI1);
    tim1->cr2 = TIM_CR2_MMS(2); /* UEV -> TRGO */
    tim1->cr1 = TIM_CR1_ARPE | TIM_CR1_OPM;
    tim1->ccer = TIM_CCER_CC1E;

    /* Timer 1 Channel 3 is used to disable the OSD box. */
    tim1_ch3_dma.cpar = (uint32_t)(unsigned long)&gpio_display->crh;
    tim1_ch3_dma.cmar = (uint32_t)(unsigned long)&gpio_display_crh[OSD_OFF];
    tim1_ch3_dma.cndtr = 1;
    tim1_ch3_dma.ccr = (DMA_CCR_PL_V_HIGH |
                        DMA_CCR_MSIZE_32BIT |
                        DMA_CCR_PSIZE_32BIT |
                        DMA_CCR_CIRC |
                        DMA_CCR_DIR_M2P |
                        DMA_CCR_TCIE |
                        DMA_CCR_EN);
    tim1->ccmr2 = TIM_CCMR2_CC3S(TIM_CCS_OUTPUT);
    tim1->dier = TIM_DIER_CC3DE;
    tim2->cr2 = TIM_CR2_MMS(2); /* UEV -> TRGO */
    tim1->ccer |= TIM_CCER_CC3E;

    /* Timer 1 Channel 4 is used to trigger an IRQ before OSD end. */
    tim1->ccmr2 |= TIM_CCMR2_CC4S(TIM_CCS_OUTPUT);
    tim1->dier |= TIM_DIER_CC4IE;
    tim1->ccer |= TIM_CCER_CC4E;

    slave_arr_update();
    set_polarity();

    amiga_init();

    rotary = gpioa->idr & 3;
    timer_init(&button_timer, button_timer_fn, NULL);
    timer_set(&button_timer, time_now());

    for (i = 0; i < ARRAY_SIZE(irqs); i++) {
        IRQx_set_prio(irqs[i], SYNC_IRQ_PRI);
        IRQx_set_pending(irqs[i]);
        IRQx_enable(irqs[i]);
    }

    frame_time = time_now();
    lost_sync = FALSE;

    _keyboard_held = keyboard_held;

    for (;;) {

        canary_check();

        /* Wait while displaying OSD box. This avoids modifying config values 
         * etc during the critical display period, which could cause
         * glitches. */
        for (i = 0; i < 5; i++) { /* up to 5ms */
            if (hline < (config.v_off - 3))
                break;
            delay_ms(1);
        }

        /* Check for losing sync: no valid frame in over 100ms. We repeat the 
         * forced reset every 100ms until sync is re-established. */
        if (time_diff(frame_time, time_now()) > time_ms(100)) {
            if (!lost_sync)
                printk("Sync lost\n");
            lost_sync = TRUE;
            frame_time = time_now();
            IRQ_global_disable();
            tim1->smcr = 0;
            hline = HLINE_EOF;
            IRQ_global_enable();
        }

        /* Keyboard hold/release notifier? */
        if (keyboard_held != _keyboard_held) {
            snprintf((char *)notify.text[0], sizeof(notify.text[0]),
                     "Keyboard %s",
                     keyboard_held ? "Held" : "Released");
            notify.cols = strlen((char *)notify.text[0]);
            notify.rows = 1;
            notify.on = TRUE;
            notify_time = time_now();
            _keyboard_held = keyboard_held;
        }

        /* Have we just finished generating a frame? */
        if (frame) {

            uint16_t height;

            if (lost_sync) {
                printk("Sync found\n");
                lost_sync = FALSE;
            }

            frame_time = time_now();
            frame = 0;

            /* Work out what to display next frame. */
            cur_display = config_active ? &config_display : &lcd_display;
            if (notify.on) {
                if (time_diff(notify_time, time_now()) > time_ms(2000)) {
                    notify.on = FALSE;
                } else {
                    cur_display = &notify;
                }
            }

            /* Next frame height depends on #rows and height of each row. */
            height = cur_display->rows*10+2;
            for (i = 0; i < cur_display->rows; i++)
                if (cur_display->heights & (1<<i))
                    height += 8;
            height = min_t(uint16_t, height, MAX_DISPLAY_HEIGHT);

            /* Render to the SPI DMA buffer. */
            for (i = 0; i < height; i++)
                render_line(i, cur_display);
            if (cur_display->on) {
                /* [8 ticks per pixel (at SPI 9MHz rate)]
                 * x [8 pixels per character] x [@cols characters] 
                 * + [allowance for OSD box lead-in and lead-out] */
                tim1->ccr3 = 8 * 8 * cur_display->cols + 80;
                tim1->ccr4 = tim1->ccr3 - sysclk_us(1);
                barrier(); /* Set post-OSD timeout /then/ enable display */
                display_height = height;
            } else {
                display_height = 0;
            }

        }

        update_amiga_keys();
        emulate_gotek_buttons();

        /* Clear keyboard-hold/release notifier upon further key presses. */
        if (keys)
            notify.on = FALSE;

        if (buttons) {
            /* Atomically snapshot and clear the button state. */
            uint8_t b;
            uint32_t oldpri;
            oldpri = IRQ_save(TIMER_IRQ_PRI);
            b = buttons;
            buttons = 0;
            IRQ_restore(oldpri);
            /* Fold in keyboard presses. */
            if (config_active) {
                b |= keys & (B_LEFT | B_RIGHT | B_SELECT);
            } else {
                if (keys & K_MENU) b |= B_SELECT;
            }
            /* Fold in button presses remoted via I2C. */
            b |= ff_osd_buttons;
            /* Pass button presses to config subsystem for processing. */
            config_process(b & ~B_PROCESSED);
        }

        lcd_process();
    }

    return 0;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "Linux"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
