/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * <dl8dtl@s09.sax.de> wrote this file.  As long as you retain this notice you
 * can do whatever you want with this stuff. If we meet some day, and you think
 * this stuff is worth it, you can buy me a beer in return.        Joerg Wunsch
 * ----------------------------------------------------------------------------
 *
 */

#include <string.h>
#include <stdbool.h>
#include <math.h>

#include <avr/power.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>

#define NLEDS 6 // per group
#define NDIM  20 // duty cycle for dimmed LEDs

#define F_TIMER (100 * NLEDS) // 100 Hz per LED

enum ledstatus
{
    OFF, DIM, ON
} __attribute__((packed));

enum ledstatus leds[2 * NLEDS];

static volatile uint32_t ticks;

static uint8_t cycle;

/*
 * LEDs are operated in "Charliexplexing" mode
 * https://en.wikipedia.org/wiki/Charlieplexing
 *
 * This requires to re-decide about each LED pin's state between high,
 * low, and off (high-Z) on each timestep.  On the pro side, it allows
 * for connecting a fairly large number of LEDs with only few
 * controller pins.  Very large matrices however cause a poor LED duty
 * cycle, thus we operate two groups of 6 LEDs each on 3 controller
 * pins per group, yielding a 1:6 duty cycle for each LED.  Depending
 * on battery voltage, this yields in an effective LED current of
 * about 30 mA / 6 = 5 mA for each active LED.
 *
 * Note that LED12 is not mounted, as using an odd number of LEDs
 * makes the overall design look nicer, since there's a single center
 * LED.
 */
static void
led_update(void)
{

    if (++cycle == NLEDS)
    {
        cycle = 0;
    }

    // LEDs are on PD7, PD6, PD5
    uint8_t port = PORTD.OUT & ~0b00000111; // preserve all non-LED bits,
    uint8_t ddr  = PORTD.DIR  & ~0b00000111; // but mask LED bits to 0

    if (leds[cycle] == OFF)
    {
        ddr |= 0b00000111; // output low, all off
    }
    else // this LED needs to be turned on during this cycle
    {
        /*
         * charlieplexing matrix lower LEDs
         *
         * LED1: PD7 high, PD6 low,  PD5 off
         * LED2: PD7 low,  PD6 high, PD5 off
         * LED3: PD7 off,  PD6 low,  PD5 high
         * LED4: PD7 off,  PD6 high, PD5 low
         * LED5: PD7 high, PD6 off,  PD5 low
         * LED6: PD7 low,  PD6 off,  PD5 high
         * off:  PD7 low,  PD6 low,  PD5 low
         */

        if (cycle == 0)      { ddr |= 0b00000011; port |= 0b00000001; }
        else if (cycle == 1) { ddr |= 0b00000011; port |= 0b00000010; }
        else if (cycle == 2) { ddr |= 0b00000110; port |= 0b00000100; }
        else if (cycle == 3) { ddr |= 0b00000110; port |= 0b00000010; }
        else if (cycle == 4) { ddr |= 0b00000101; port |= 0b00000001; }
        else                 { ddr |= 0b00000101; port |= 0b00000100; }
    }

    PORTD.OUT = port;
    PORTD.DIR = ddr;

    // LEDs are on PC3, PC2, PC1
    port = PORTC.OUT & ~0b00000111;
    ddr = PORTC.DIR & ~0b00000111;

    if (leds[cycle + NLEDS] == OFF)
    {
        ddr |= 0b00000111; // output low, all off
    }
    else
    {
        /*
         * charlieplexing matrix upper LEDs
         *
         * LED7:  PC3 high, PC2 off,  PC1 low
         * LED8:  PC3 low,  PC2 off,  PC1 high
         * LED9:  PC3 off,  PC2 high, PC1 low
         * LED10: PC3 off,  PC2 low,  PC1 high
         * LED11: PC3 high, PC2 low,  PC1 off
         * LED12: PC3 low,  PC2 high, PC1 off  // not mounted
         * off:   PC3 low,  PC2 low,  PC1 low
         */
        if (cycle == 0)      { ddr |= 0b00000101; port |= 0b00000001; }
        else if (cycle == 1) { ddr |= 0b00000101; port |= 0b00000100; }
        else if (cycle == 2) { ddr |= 0b00000110; port |= 0b00000010; }
        else if (cycle == 3) { ddr |= 0b00000110; port |= 0b00000100; }
        else if (cycle == 4) { ddr |= 0b00000011; port |= 0b00000001; }
        //else               { ddr |= 0b00000011; port |= 0b00000010; }
    }

    PORTC.OUT = port;
    PORTC.DIR = ddr;
}

/*
 * Turn off current LEDs if they are in DIM state.
 *
 * Called early during the timer counting cycle.  Remainder of timer
 * period the proceeds with this LED turned off.
 */
static void
led_dim(void)
{
    // LEDs are on PD7, PD6, PD5
    if (leds[cycle] == DIM)
    {
        PORTD.OUT &= ~0b00000111; // output low
        PORTD.DIR  |=  0b00000111;
    }

    // LEDs are on PC3, PC2, PC1
    if (leds[cycle + NLEDS] == DIM)
    {
        PORTC.OUT &= ~0b00000111; // output low
        PORTC.DIR  |=  0b00000111;
    }
}

ISR(TCE0_OVF_vect)
{
    static int t;

    if (++t == F_TIMER / 2)
    {
        t = 0;
        ticks++;
    }

    led_update();
}

ISR(TCE0_CMP0_vect)
{
    led_dim();
}

static void
setup(void)
{
    // 20 MHz high frequency RC oscillator / 24 => 0.866 MHz main system clock
    CLKCTRL.MCLKCTRLB =  CLKCTRL_PEN_bm | CLKCTRL_PDIV_DIV24_gc;
    CLKCTRL.MCLKTIMEBASE = 1; // actually 1.2 µs, must be larger than 1 µs

    /* TCE0: interrupt each 1/F_TIMER */
    TCE0.PER = F_CPU / F_TIMER;
    TCE0.CMP0 = F_CPU / F_TIMER / DIM_DUTYCYCLE; /* dimmed LED turnoff time */
    TCE0.INTCTRL = TCE_CMP0_bm | TCE_OVF_bm;  /* overflow and compare 0 interrupts */
    TCE0.CTRLA = TCE_ENABLE_bm;

    /* PD4: jumper 2 */
    /* PF7: UPDI */
    /* PA0/1: spare/adjust */
    /* PC0: jumper 1 */
    /* Enable pullups for all these pins */
    PORTD.PIN4CTRL = PORT_PULLUPEN_bm;
    PORTF.PIN7CTRL = PORT_PULLUPEN_bm;
    PORTA.PIN0CTRL = PORT_PULLUPEN_bm;
    PORTA.PIN1CTRL = PORT_PULLUPEN_bm;
    PORTC.PIN1CTRL = PORT_PULLUPEN_bm;

    /* LED group 1: PD7/6/5 */
    /* LED group 2: PC3/2/1 */

    sei();
}

static void
loop(void)
{
    static uint32_t oldtime;
    static int state;

    if (oldtime != ticks)
    {
        oldtime = ticks;

        switch (state)
        {
        case 0:
            memset(leds, DIM, 2 * NLEDS);
            break;

        case 1 ... 9:
            // wait
            break;

        case 10 ... 20:
            memset(leds, DIM, 2 * NLEDS);
            leds[state - 10] = ON;
            break;

        case 21 ... 31:
            memset(leds, OFF, 2 * NLEDS);
            leds[31 - state] = ON;
            break;

        case 32:
            memset(leds, OFF, 2 * NLEDS);
            break;

        case 33 ... 35:
            // wait
            break;

        case 36:
            state = -1; // advance to 0 below
            break;
        }
        state++;
    }
    sleep_mode();
}


int main(void) __attribute__((OS_main));
int
main(void)
{
    setup();

    for (;;)
      loop();
}

/*
 * Local Variables:
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
