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

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>
#include <avr/power.h>
#include <avr/eeprom.h>

#define NLEDS 6           // per group
#define DIM_DUTYCYCLE  20 // duty cycle for dimmed LEDs

#define F_TIMER (100 * NLEDS) // 100 Hz per LED

#define EE_CALIB_LOC ((void *)16) // avoid using cell #0

#define CALIB_OFFSET_UP()     (calib_jumpers & _BV(4))
#define CALIB_OFFSET_DOWN()   (calib_jumpers & _BV(5))
#define CALIB_TOGGLE_STRIPE() (calib_jumpers & _BV(6))

enum ledstatus
{
    OFF, DIM, DIM_FLASH, FLASH, ON
} __attribute__((packed));

enum opmode
{
    NORMAL, CALIBRATION
} __attribute__((packed));

struct eeprom_data
{
    int t_offset;
    bool led_stripe;
} __attribute__((packed));

enum ledstatus leds[2 * NLEDS];
enum opmode opmode;

struct eeprom_data calib_data;

static volatile uint32_t ticks;
static volatile uint16_t adc_result;

static uint8_t calib_jumpers;
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

    // LEDs are on PA0, PA1, PA2
    uint8_t port = PORTA & ~0b00000111; // preserve all non-LED bits,
    uint8_t ddr  = DDRA  & ~0b00000111; // but mask LED bits to 0

    if (leds[cycle] == OFF ||
        ((leds[cycle] == DIM_FLASH || leds[cycle] == FLASH) && (ticks & 1) == 0))
    {
        // Current LED must be turned off
        // output low, all off
        ddr |= 0b00000111;
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
        ((leds[cycle + NLEDS] == DIM_FLASH || leds[cycle + NLEDS] == FLASH) && (ticks & 1) == 0))
    {
        // Current LED must be turned off
        // output low, all off
        ddr |= 0b00000111;
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

/*
 * Start one ADC temperature conversion
 */
static void
start_adc(void)
{
    /* divider 1:64 => 125 kHz ADC clock */
    ADCSRA = _BV(ADPS2) | _BV(ADPS1) |_BV(ADEN) | _BV(ADIF);
    // now, actually start the conversion
    ADCSRA = _BV(ADPS0) | _BV(ADPS1) | _BV(ADEN) | _BV(ADSC) | _BV(ADIE);
}

/*
 * Turn off current LEDs if they are in DIM (or DIM_FLASH) state.
 *
 * Called early during the timer counting cycle.  Remainder of timer
 * period then proceeds with this LED turned off.
 */
static void
led_dim(void)
{
    // LEDs are on PA0, PA1, PA2
    if (leds[cycle] == DIM || leds[cycle] == DIM_FLASH)
    {
        PORTA &= ~0b00000111; // output low
        DDRA  |=  0b00000111;
    }

    // LEDs are on PB0, PB1, PB2
    if (leds[cycle + NLEDS] == DIM || leds[cycle + NLEDS] == DIM_FLASH)
    {
        PORTB &= ~0b00000111; // output low
        DDRB  |=  0b00000111;
    }
}

/*
 * ADC interrupt service: called at end of ADC conversion
 *
 * Just registers current ADC readout for later processing (by
 * display_temperature(), and stops ADC.
 */
ISR(ADC_vect)
{
    adc_result = ADC;
    ADCSRA = _BV(ADPS0) | _BV(ADPS1) | _BV(ADEN);
}

/*
 * Timer 1 compare/match A interrupt is triggered when the counter
 * reaches its final value (where it will automatically restart from
 * 0).
 *
 * This is the basic heartbeat software clock of the system, ticking
 * with a frequency of F_TIMER.
 */
ISR(TIM1_COMPA_vect)
{
    static int t;

    // At each tick of the heartbeat timer, update the LED
    // charlieplexing (i.e. configure next LED within its group).
    led_update();

    if (++t == F_TIMER / 2)
    {
        // Each half second, trigger one temperature measurement, and
        // read the calibration jumper state from PA4 through PA6.
        t = 0;
        start_adc();
        calib_jumpers = ~PINA & 0b01110000; // PA4 ... PA6
        ticks++;
    }
}

/*
 * Timer 1 compare/match B interrupt is triggered early in the counter
 * cycle, and used to turn off dimmed LEDs quickly.
 */
ISR(TIM1_COMPB_vect)
{
    led_dim();
}

/*
 * Initialize everything.
 */
static void
setup(void)
{
    clock_prescale_set(clock_div_8); // => 1 MHz main system clock

    /* TIMER1: interrupt each 1/F_TIMER */
    OCR1A = F_CPU / F_TIMER;
    OCR1B = F_CPU / F_TIMER / DIM_DUTYCYCLE; /* dimmed LED turnoff time */
    TIMSK1 = _BV(OCIE1A) | _BV(OCIE1B);      /* enable compare/match
                                                interrupts A and B */
    TCCR1B = _BV(WGM12) | _BV(CS10);         /* CTC mode, full clock */

    /* PA3: jumper 2 */
    /* PA4/5/6: ISP / USI */
    /* PA7: jumper 1 */
    /* Enable pullups for all these pins */
    PORTA = _BV(3) | _BV(4) | _BV(5) | _BV(6) | _BV(7);

    /* LED group 1: PA0/1/2 */
    /* LED group 2: PB0/1/2 */

    /* ADC: internal 1.1 V reference, channel 8 (temp sensor) */
    ADMUX = _BV(REFS1) | 0b100010;
    /* divider 1:64 => 125 kHz ADC clock */
    ADCSRA = _BV(ADPS2) | _BV(ADPS1) |_BV(ADEN);

    sei();

    /* Read in calibration data record from EEPROM */
    eeprom_read_block(&calib_data, EE_CALIB_LOC, sizeof(calib_data));
    if (calib_data.t_offset == (int)0xFFFF)
    {
        // EEPROM not written yet, use default data
        calib_data.t_offset = 275;
        calib_data.led_stripe = false;
        eeprom_write_block(&calib_data, EE_CALIB_LOC, sizeof(calib_data));
    }
}

/*
 * Configure LED display according to measured temperature ADC readout.
 */
static void
display_temperature(uint16_t t)
{
    int temp = (int)t - calib_data.t_offset;

    if ((PINA & _BV(7)) == 0)
    {
        // jumper 1 set: range 12 ... 32 degC

        memset(leds, OFF, sizeof leds);
        if (temp < 12)
        {
            // low temperature: flash-dim first LED
            leds[0] = DIM_FLASH;
        }
        else if (temp > 32)
        {
            // over temperature: flash-dim all LEDs
            memset(leds, DIM_FLASH, sizeof leds);
        }
        else
        {
            // normal range
            if (calib_data.led_stripe)
                // dim LEDs below actual value when in "stripe" mode
                memset(leds, DIM, (temp - 12) / 2);
            // turn on LED for actual value
            if (temp & 1)
                leds[(temp - 12) / 2] = ON;
            else
                leds[(temp - 12) / 2] = FLASH;
        }
    }
    else
    {
        // jumper 1 pulled: range 0 ... 40 degC

        memset(leds, OFF, sizeof leds);
        if (temp < 0)
        {
            // low temperature: flash-dim first LED
            leds[0] = DIM_FLASH;
        }
        else if (temp > 40)
        {
            // over temperature: flash-dim all LEDs
            memset(leds, DIM_FLASH, sizeof leds);
        }
        else
        {
            // normal range:
            if (calib_data.led_stripe)
                // dim LEDs below actual value when in "stripe" mode
                memset(leds, DIM, temp / 4);
            // turn on LED for actual value
            if ((temp & 3) < 2)
                leds[temp / 4] = ON;
            else
                leds[temp / 4] = FLASH;
        }
    }
}

/*
 * Periodic task
 *
 * Called once per interrupt (about 2 * F_TIMER times per second).
 */
static void
loop(void)
{
    uint16_t t;

    if ((t = adc_result) != 0)
    {
        // new temperature value available
        adc_result = 0;
        display_temperature(t);
    }

    if ((PINA & _BV(3)) == 0)
    {
        // jumper 2 set: calibration mode
        opmode = CALIBRATION;
    }
    else
    {
        // jumper 2 pulled: normal mode
        if (opmode == CALIBRATION)
        {
            // leaving calibration mode: store
            // calibration data in EEPROM
            eeprom_write_block(&calib_data, EE_CALIB_LOC, sizeof(calib_data));
        }
        opmode = NORMAL;
    }

    // When in calibration mode, handle the individual jumpers
    if (opmode == CALIBRATION)
    {
        if (CALIB_TOGGLE_STRIPE())
            calib_data.led_stripe = !calib_data.led_stripe;
        else if (CALIB_OFFSET_UP())
            calib_data.t_offset++;
        else if (CALIB_OFFSET_DOWN())
            calib_data.t_offset--;
        calib_jumpers = 0;
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