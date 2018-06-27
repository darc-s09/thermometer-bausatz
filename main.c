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

#include <avr/power.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>

#define NLEDS 6 // per group
#define NDIM  20 // duty cycle for dimmed LEDs

// With 3000 Hz heartbeat clock, charlieplexing frequency is 500 Hz,
// and dimmed LED flicker frequency is 50 Hz.
#define F_TIMER 5000

enum ledstatus
{
    OFF, DIM, ON
} __attribute__((packed));

enum ledstatus leds[2 * NLEDS];

static volatile uint32_t ticks;

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
    static uint8_t cycle;
    static uint8_t dim_cycle;

    if (++cycle == NLEDS)
    {
        cycle = 0;
        // LEDs in DIM state are only activated each NDIMth cycle
        // LEDs in ON state are activated each time their cycle
        // number matches
        if (++dim_cycle == NDIM)
            dim_cycle = 0;
    }

    // LEDs are on PA0, PA1, PA2
    uint8_t port = PORTA & ~0b00000111; // preserve all non-LED bits,
    uint8_t ddr  = DDRA  & ~0b00000111; // but mask LED bits to 0

    if (leds[cycle] == OFF ||
        (leds[cycle] == DIM && dim_cycle != 0))
    {
        ddr |= 0b00000111; // output low, all off
    }
    else // this LED needs to be turned on during this cycle
    {
        /*
         * charlieplexing matrix lower LEDs
         *
         * LED1: PA0 high, PA1 low,  PA2 off
         * LED2: PA0 low,  PA1 high, PA2 off
         * LED3: PA0 off,  PA1 low,  PA2 high
         * LED4: PA0 off,  PA1 high, PA2 low
         * LED5: PA0 high, PA1 off,  PA2 low
         * LED6: PA0 low,  PA1 off,  PA2 high
         * off:  PA0 low,  PA1 low,  PA2 low
         */

        if (cycle == 0)      { ddr |= 0b00000011; port |= 0b00000001; }
        else if (cycle == 1) { ddr |= 0b00000011; port |= 0b00000010; }
        else if (cycle == 2) { ddr |= 0b00000110; port |= 0b00000100; }
        else if (cycle == 3) { ddr |= 0b00000110; port |= 0b00000010; }
        else if (cycle == 4) { ddr |= 0b00000101; port |= 0b00000001; }
        else                 { ddr |= 0b00000101; port |= 0b00000100; }
    }

    PORTA = port;
    DDRA = ddr;

    // LEDs are on PB0, PB1, PB2
    port = PORTB & ~0b00000111;
    ddr = DDRB & ~0b00000111;

    if (leds[cycle + NLEDS] == OFF ||
        (leds[cycle + NLEDS] == DIM && dim_cycle != 0))
    {
        ddr |= 0b00000111; // output low, all off
    }
    else
    {
        /*
         * charlieplexing matrix upper LEDs
         *
         * LED7:  PB0 high, PB1 off,  PB2 low
         * LED8:  PB0 low,  PB1 off,  PB2 high
         * LED9:  PB0 off,  PB1 high, PB2 low
         * LED10: PB0 off,  PB1 low,  PB2 high
         * LED11: PB0 high, PB1 low,  PB2 off
         * LED12: PB0 low,  PB1 high, PB2 off  // not mounted
         * off:   PB0 low,  PB1 low,  PB2 low
         */
        if (cycle == 0)      { ddr |= 0b00000101; port |= 0b00000001; }
        else if (cycle == 1) { ddr |= 0b00000101; port |= 0b00000100; }
        else if (cycle == 2) { ddr |= 0b00000110; port |= 0b00000010; }
        else if (cycle == 3) { ddr |= 0b00000110; port |= 0b00000100; }
        else if (cycle == 4) { ddr |= 0b00000011; port |= 0b00000001; }
        //else               { ddr |= 0b00000011; port |= 0b00000010; }
    }

    PORTB = port;
    DDRB = ddr;
}


ISR(TIM1_COMPA_vect)
{
    static int t;

    if (++t == F_TIMER / 2)
    {
        t = 0;
        ticks++;
    }

    led_update();
}

static void
setup(void)
{
    clock_prescale_set(clock_div_1);

    /* TIMER1: interrupt each 1/F_TIMER */
    OCR1A = F_CPU / F_TIMER;
    TIMSK1 = _BV(OCIE1A);
    TCCR1B = _BV(WGM12) | _BV(CS10); /* CTC mode, full clock */

    /* PA3: jumper 2 */
    /* PA4/5/6: ISP / USI */
    /* PA7: jumper 1 */
    PORTA = _BV(3) | _BV(4) | _BV(5) | _BV(6) | _BV(7);

    /* LED group 1: PA0/1/2 */
    /* LED group 2: PB0/1/2 */

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
