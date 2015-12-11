/*!
 * @file uart.c
 * @author Alex Murray
 *
 * Created on 12 November 2015, 14:29
 */

#include "drv/uart.h"
#include "drv/hw.h"
#include "core/event.h"

/*  based on Example 5-1 in UART pdf */
#define BAUDRATE 115200
#define BRGVAL ((FP/BAUDRATE)/16)-1

#define SEND_QUEUE_SIZE 64

static void process_incoming_data(unsigned int data);
static void configure_pins();
static void configure_uart();

typedef enum
{
    STATE_IDLE,
    STATE_SELECT_MODEL,
    /* awaits a selector for what is to be done to a model*/
    STATE_AWAIT_MODEL_CONFIG,
    STATE_CONFIG_OPEN_CIRCUIT_VOLTAGE,
    STATE_CONFIG_SHORT_CIRCUIT_CURRENT,
    STATE_CONFIG_TEMPERATURE,
    STATE_CONFIG_EXPOSURE,
} state_e;

typedef enum
{
    CASE_SELECT_MODEL = 'm',
} case_e;

struct data_t {
    union
    {
        struct {
            unsigned char selected_model; /* number representing the model */
            unsigned char selected_param; /* defines whether param holds value
                                           * for Current, Voltage, Temperature
                                           * or Exposure */
            unsigned int param;           /* holds numerical value
                                           * UNITS:
                                           * I: milli ampere
                                           * U: milli volt
                                           * T: degrees celsius
                                           * E: percent */
        } config_model;
    };
};

struct ring_buffer_t
{
    volatile unsigned char read;
    unsigned char write;
    unsigned char data[SEND_QUEUE_SIZE];
};

static state_e              state       = STATE_IDLE;
static struct data_t        state_data;
static struct ring_buffer_t send_queue;

/* -------------------------------------------------------------------------- */
void uart_init(void)
{
    configure_pins();
    configure_uart();

    event_register_listener(EVENT_DATA_RECEIVED, process_incoming_data);

    /* init ring buffer */
    send_queue.read = 0;
    send_queue.write = 0;
}

/* -------------------------------------------------------------------------- */
void uart_send_byte(unsigned char byte)
{
    unsigned char write = send_queue.write;

    if(write == send_queue.read)
        if(U1STA.TRMT)
        {
            U1TXREG = byte;
            return;
        }

    /* increment and wrap write position */
    ++send_queue.write;
    send_queue.write = (send_queue.write == SEND_QUEUE_SIZE ?
            0 : send_queue.write);

    /* if queue is full, block until space frees up */
    while(send_queue.write == send_queue.read);

    send_queue.data[write] = byte;
}

/* -------------------------------------------------------------------------- */
static void configure_pins()
{
    /* See also Example 10-2 from I/O Ports pdf.*/

    ANSELCbits.ANSC10 = 0;    /* Set RP58 to digital */
    ANSELCbits.ANSC11 = 0;    /* Set RP59 to digital */
    ANSELCbits.ANSC12 = 0;    /* set RP60 to digital */
                              /* RP61 seems to already be digital. */

    __builtin_write_OSCCONL(OSCCON & ~ (1<<6)); /* Unlock registers */

    RPINR18bits.U1RXR = 59;  /* Assign U1Rx to Pin RP59 */
    RPINR18bits.U1CTSR = 58; /* Assign U1TCS to Pin RP58 */

    RPOR14bits.RP60R = 1; /* Assign U1Tx to Pin RP60, see p.10-11 I/O
                           * ports documentation */
    RPOR14bits.RP61R = 3; /* Assign U1RTS to RP61 */
    CNPUCbits.CNPUC11 = 1; /* RX requires pull-up */

    __builtin_write_OSCCONL(OSCCON | (1<<6)); /* Lock registers */
}

/* -------------------------------------------------------------------------- */
static void configure_uart()
{
    /* Example 5-1 from UART manual pdf */
    U1MODEbits.STSEL = 0;           /* 1 stop bit */
    /*U1MODEbits.PDSEL = 0;             no parity */
    U1MODEbits.PDSEL = 1;           /* 8-bit data, even parity */
    U1MODEbits.ABAUD = 0;           /* Auto-Baud disabled */
    U1MODEbits.BRGH = 0;            /* Standard-Speech mode */
    U1MODEbits.RXINV = 1;           /* Invert RX (due to isolating IC) */
    U1STAbits.TXINV = 1;            /* Invert TX (due to isolating IC) */

    U1BRG = BRGVAL;                 /* baud rate setting, see #defines at top */

    U1STAbits.UTXISEL0 = 0;         /* interrupt after one Tx char is transmitted */
    U1STAbits.UTXISEL1 = 0;

    U1MODEbits.UARTEN = 1;          /* enable UART */
    U1STAbits.UTXEN = 1;            /* enable UART TX */

    IFS0bits.U1TXIF = 0;
    IEC0bits.U1TXIE = 1;            /* enable UART TX interrupt */

    IFS0bits.U1RXIF = 0;
    IEC0bits.U1RXIE = 1;            /* enable RX interrupt */
}

/* -------------------------------------------------------------------------- */
static void process_incoming_data(unsigned int data)
{

#define CHAR_TO_INT(x) ((unsigned short)(x - '0'))

    switch (state)
    {
        case STATE_IDLE:
            if (data == CASE_SELECT_MODEL)
            {
                state_data.config_model.selected_model = 0;
                state_data.config_model.param = 0;
                state = STATE_SELECT_MODEL;
            }
            break;

        case STATE_SELECT_MODEL:
            if (data >= '0' && data <= '9')
            {
                /* Process multi-digit model numbers */
                state_data.config_model.selected_model *= 10;
                state_data.config_model.selected_model += CHAR_TO_INT(data);

                /*
                 * Hijack param as a flag to indicate if a number was received
                 * or not so we can detect the error case when two non-numbers
                 * are sent consecutively.
                 */
                state_data.config_model.param = 1;
            } else if (state_data.config_model.param == 0) {
                /*
                 * Letter directly followed by another letter: error,
                 * revert back to idle state
                 */
                state = STATE_IDLE;
            } else {
                state = STATE_AWAIT_MODEL_CONFIG;
            }
            break;

        case STATE_AWAIT_MODEL_CONFIG:
            /*
             * We can configure current, voltage, temp or exposure. Anything
             * else is an error and results in reverting back to idle state.
             */
            switch (data)
            {
                case 'I':
                    state_data.config_model.selected_param = data;
                    state = STATE_CONFIG_SHORT_CIRCUIT_CURRENT;
                    break;

                case 'U':
                    state_data.config_model.selected_param = data;
                    state = STATE_CONFIG_OPEN_CIRCUIT_VOLTAGE;
                    break;

                case 'T':
                    state_data.config_model.selected_param = data;
                    state = STATE_CONFIG_TEMPERATURE;
                    break;

                case 'E':
                    state_data.config_model.selected_param = data;
                    state = STATE_CONFIG_EXPOSURE;
                    break;

                default:
                    state = STATE_IDLE;
                    break;
            }
            break;

        case STATE_CONFIG_OPEN_CIRCUIT_VOLTAGE:
            break;

        case STATE_CONFIG_SHORT_CIRCUIT_CURRENT:
            break;

        case STATE_CONFIG_TEMPERATURE:
            break;

        case STATE_CONFIG_EXPOSURE:
            break;

        default:
            state = STATE_IDLE;
            break;
    }
}


/* -------------------------------------------------------------------------- */
void _ISR_NOPSV _U1RXInterrupt(void)
{
    // U1TXREG = U1RXREG; /* echo back whatever we receive */

    event_post(EVENT_DATA_RECEIVED, U1RXREG);

    /* clear interrupt flag */
    IFS0bits.U1RXIF = 0;
}

/* -------------------------------------------------------------------------- */
void _ISR_NOPSV _U1TXInterrupt(void)
{
    for(;;)
    {
        /* send the next queued byte if available, otherwise stop sending */
        if(send_queue.read != send_queue.write)
            U1TXREG = send_queue.data[send_queue.read];
        else
            break;

        /* increment and write read position */
        ++send_queue.read;
        if(send_queue == SEND_QUEUE_SIZE) send_queue.read = 0;
    }

    /* clear interrupt flag */
    IFS0bits.U1TXIF = 0;
}

/* -------------------------------------------------------------------------- */
/* Unit Tests */
/* -------------------------------------------------------------------------- */

#ifdef TESTING

#include "gmock/gmock.h"

using namespace ::testing;

/* -------------------------------------------------------------------------- */
/* Test fixture -- Defines stuff that gets called for every test case */
class uart : public Test
{
    virtual void SetUp()
    {
        /* free all listeners and destroy pending events */
        event_deinit();

        /* initialise uart so our listeners are registered */
        uart_init();
        /* set initial state */
        state = STATE_IDLE;
    }

    virtual void TearDown()
    {
    }
};

/* -------------------------------------------------------------------------- */
/* Simulates a byte being received */
void sendByte(unsigned char byte)
{
    U1RXREG = byte;
    _U1RXInterrupt();
    event_dispatch_all();
}
void sendString(const char* str)
{
    while(*str)
        sendByte(*(unsigned char*)const_cast<char*>(str++));
}

/* -------------------------------------------------------------------------- */
TEST_F(uart, selected_model_and_param_is_reset_correctly_when_switching_to_STATE_SELECT_MODEL)
{
    /* set some garbage values */
    state_data.config_model.param = 88;
    state_data.config_model.selected_model = 5;

    sendString("m");

    EXPECT_THAT(state, Eq(STATE_SELECT_MODEL));
    EXPECT_THAT(state_data.config_model.param, Eq((unsigned int)0));
    EXPECT_THAT(state_data.config_model.selected_model, Eq((unsigned int)0));
}

TEST_F(uart, model_is_correctly_selected)
{
    sendString("m2");

    EXPECT_THAT(state, Eq(STATE_SELECT_MODEL));
    EXPECT_THAT(state_data.config_model.selected_model, Eq(2));
}

TEST_F(uart, model_with_multiple_digits_is_correctly_selected)
{
    sendString("m195");

    EXPECT_THAT(state, Eq(STATE_SELECT_MODEL));
    EXPECT_THAT(state_data.config_model.selected_model, Eq(195));
}

TEST_F(uart, abort_model_selection_if_no_number_is_sent)
{
    sendString("ma");

    EXPECT_THAT(state, Eq(STATE_IDLE));
}

#endif /* TESTING */

