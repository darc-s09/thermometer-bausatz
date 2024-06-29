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

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>
#include <avr/power.h>
#include <avr/eeprom.h>

#define NLEDS 6           // per group
#define DIM_DUTYCYCLE  20 // duty cycle for dimmed LEDs

#define F_TIMER (100 * NLEDS) // 100 Hz per LED

#define EE_CALIB_LOC ((void *)16) // avoid using cell #0

#define CALIB_OFFSET_UP()     (calib_jumpers & 1)
#define CALIB_OFFSET_DOWN()   (calib_jumpers & 2)
#define CALIB_TOGGLE_STRIPE() (calib_jumpers & 4)

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

    // LEDs are on PD7, PD6, PD5
    uint8_t port = PORTD.OUT & ~0b11100000; // preserve all non-LED bits,
    uint8_t ddr  = PORTD.DIR & ~0b11100000; // but mask LED bits to 0

    if (leds[cycle] == OFF ||
        ((leds[cycle] == DIM_FLASH || leds[cycle] == FLASH) && (ticks & 1) == 0))
    {
        // Current LED must be turned off
        // output low, all off
        ddr |= 0b11100000; // output low, all off
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

        if (cycle == 0)      { ddr |= 0b11000000; port |= 0b10000000; }
        else if (cycle == 1) { ddr |= 0b11000000; port |= 0b01000000; }
        else if (cycle == 2) { ddr |= 0b01100000; port |= 0b00100000; }
        else if (cycle == 3) { ddr |= 0b01100000; port |= 0b01000000; }
        else if (cycle == 4) { ddr |= 0b10100000; port |= 0b10000000; }
        else                 { ddr |= 0b10100000; port |= 0b00100000; }
    }

    PORTD.OUT = port;
    PORTD.DIR = ddr;

    // LEDs are on PC3, PC2, PC1
    port = PORTC.OUT & ~0b00001110;
    ddr  = PORTC.DIR & ~0b00001110;

    if (leds[cycle + NLEDS] == OFF ||
        ((leds[cycle + NLEDS] == DIM_FLASH || leds[cycle + NLEDS] == FLASH) && (ticks & 1) == 0))
    {
        // Current LED must be turned off
        // output low, all off
        ddr |= 0b00001110; // output low, all off
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
        if (cycle == 0)      { ddr |= 0b00001010; port |= 0b00001000; }
        else if (cycle == 1) { ddr |= 0b00001010; port |= 0b00000010; }
        else if (cycle == 2) { ddr |= 0b00000110; port |= 0b00000100; }
        else if (cycle == 3) { ddr |= 0b00000110; port |= 0b00000010; }
        else if (cycle == 4) { ddr |= 0b00001100; port |= 0b00001000; }
        //else               { ddr |= 0b00001100; port |= 0b00000100; }
    }

    PORTC.OUT = port;
    PORTC.DIR = ddr;
}

/*
 * Start one ADC temperature conversion
 */
static void
start_adc(void)
{
    ADC0.COMMAND = ADC_MODE_SINGLE_12BIT_gc | ADC_START_IMMEDIATE_gc;
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
    // LEDs are on PD7, PD6, PD5
    if (leds[cycle] == DIM || leds[cycle] == DIM_FLASH)
    {
        PORTD.OUT &= ~0b11100000; // output low
        PORTD.DIR |=  0b11100000;
    }

    // LEDs are on PC3, PC2, PC1
    if (leds[cycle + NLEDS] == DIM || leds[cycle + NLEDS] == DIM_FLASH)
    {
        PORTC.OUT &= ~0b00001110; // output low
        PORTC.DIR |=  0b00001110;
    }
}

/*
 * Read the calibration jumpers
 *
 * bit 0 -> PF7: UP
 * bit 1 -> PA1: DOWN
 * bit 2 -> PA0: MODE
 */
static uint8_t
get_calib_jumpers(void)
{
    uint8_t val;

    uint8_t portf_in = PORTF.IN;
    uint8_t porta_in = PORTA.IN;

    val = (portf_in & _BV(7)) >> 7;  // bit 7 -> bit 0
    val |= (porta_in & _BV(1));      // bit 1 -> bit 1
    val |= (porta_in & _BV(0)) << 2; // bit 0 -> bit 2

    return val;
}

/*
 * ADC interrupt service: called at end of ADC conversion
 *
 * Just registers current ADC readout for later processing (by
 * display_temperature(), and stops ADC.
 */
ISR(ADC0_RESRDY_vect)
{
    adc_result = (uint16_t)ADC0.RESULT;
}

/*
 * Timer/counter E0 overflow interrupt is triggered when the counter
 * reaches its final value (where it will automatically restart from
 * 0).
 *
 * This is the basic heartbeat software clock of the system, ticking
 * with a frequency of F_TIMER.
 */
ISR(TCE0_OVF_vect)
{
    static int t;

    TCE0.INTFLAGS = TCE_OVF_bm; // clear interrupt flag

    // At each tick of the heartbeat timer, update the LED
    // charlieplexing (i.e. configure next LED within its group).
    led_update();

    if (++t == F_TIMER / 2)
    {
        // Each half second, trigger one temperature measurement, and
        // read the calibration jumper state from PA4 through PA6.
        t = 0;
        start_adc();
        calib_jumpers = get_calib_jumpers();
        ticks++;
    }
}

/*
 * Timer/counter E0 compare/match 0 interrupt is triggered early in the counter
 * cycle, and used to turn off dimmed LEDs quickly.
 */
ISR(TCE0_CMP0_vect)
{
    TCE0.INTFLAGS = TCE_CMP0_bm; // clear interrupt flag

    led_dim();
}

/*
 * Initialize everything.
 */
static void
setup(void)
{
    // 20 MHz high frequency RC oscillator / 24 => 0.866 MHz main system clock
    CCP = CCP_IOREG_gc;
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
    PORTC.PIN0CTRL = PORT_PULLUPEN_bm;

    /* LED group 1: PD7/6/5 */
    /* LED group 2: PC3/2/1 */

    /* ADC: internal 1.024 V reference, temperature sensor channel */
    ADC0.CTRLC = ADC_REFSEL_1V024_gc;
    ADC0.MUXPOS = ADC_MUXPOS_TEMPSENSE_gc;
    /* ADC clock must be between 300 kHz and 2 MHz - use divider 2
       (minimal divider) => 417 kHz ADC clock */
    ADC0.CTRLB = ADC_PRESC_DIV2_gc;
    /*
     * Sample duration >=32 µs according to datasheet.
     * With 100 µs, the result gets pretty stable so use that.
     */
    ADC0.CTRLE = (uint16_t)(ceil(100E-6 * (F_CPU / 2)));
    /* Enable result interrupt */
    ADC0.INTCTRL = ADC_RESRDY_bm;

    sei();

    /* Read in calibration data record from EEPROM */
    eeprom_read_block(&calib_data, EE_CALIB_LOC, sizeof(calib_data));
    if (calib_data.t_offset == (int)0xFFFF)
    {
        // EEPROM not written yet, use default data
        calib_data.t_offset = 273;
        calib_data.led_stripe = true;
        eeprom_write_block(&calib_data, EE_CALIB_LOC, sizeof(calib_data));
    }
}

static uint16_t
calc_temperature(uint16_t adc_reading)
{
    // this is described in the datasheet
#define SCALING_FACTOR 4096 // Enables integer in the signature row
    // Read signed offset from signature row
    int16_t sigrow_offset = (int16_t) SIGROW.TEMPSENSE1;
    // Read signed slope from signature row
    int16_t sigrow_slope = (int16_t) SIGROW.TEMPSENSE0;

    int32_t temp = ((int32_t) adc_reading) + sigrow_offset;
    temp *= sigrow_slope; // Result can overflow 16-bit variable
    temp += SCALING_FACTOR / 2; // Ensures correct rounding on division below
    temp /= SCALING_FACTOR; // Round to the nearest integer in Kelvin

    return temp; // in K
}

/*
 * Configure LED display according to measured temperature ADC readout.
 */
static void
display_temperature(uint16_t t)
{
    int temp = (int)t - calib_data.t_offset;

    if ((PORTC.IN & _BV(0)) == 0)
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
        uint16_t temp_k = calc_temperature(t);
        display_temperature(temp_k);
    }

    if ((PORTD.IN & _BV(4)) == 0)
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
