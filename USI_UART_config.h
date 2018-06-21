/*****************************************************************************
*
* Copyright (C) 2003 Atmel Corporation
*
* File          : USI_UART_config.h
* Compiler      : IAR EWAAVR 2.28a
* Created       : 18.07.2002 by JLL
* Modified      : 02-10-2003 by LTA
*
* Support mail  : avr@atmel.com
*
* AppNote       : AVR307 - Half duplex UART using the USI Interface
*
* Description   : Header file for USI_UART driver
*
*
****************************************************************************/

//********** USI UART Defines **********//

#define SYSTEM_CLOCK              F_CPU

#define BAUDRATE                     9600

#define TIMER_PRESCALER           1
//#define TIMER_PRESCALER           8

#define UART_RX_BUFFER_SIZE        16     /* 2,4,8,16,32,64,128 or 256 bytes */
#define UART_TX_BUFFER_SIZE        4


//********** USI_UART Prototypes **********//

unsigned char Bit_Reverse( unsigned char );
void          USI_UART_Flush_Buffers( void );
void          USI_UART_Initialise_Receiver( void );
void          USI_UART_Initialise_Transmitter( void );
void          USI_UART_Transmit_Byte( unsigned char );
unsigned char USI_UART_Receive_Byte( void );
unsigned char USI_UART_Data_In_Receive_Buffer( void );
