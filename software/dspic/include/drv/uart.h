/*! 
 * @file uart.h
 * @author Alex Murray
 *
 * Created on 12 November 2015, 14:29
 */

#ifndef UART_H
#define	UART_H

#ifdef	__cplusplus
extern "C" {
#endif

/*!
 * @brief Initialises the UART driver. Call this for communication to work.
 */
void uart_init(void);

/*!
 * @brief Queues a byte for sending.
 * @param byte The byte to send.
 */
void uart_send_byte(unsigned char byte);

#ifdef	__cplusplus
}
#endif

#endif	/* UART_H */
