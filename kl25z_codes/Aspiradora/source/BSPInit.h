#ifndef _BSPINIT_H_
#define _BSPINIT_H_

/*
 * BSPInit.h
 * ---------
 * UART0  →  Terminal/PC      (115200 bps, PTA1=RX / PTA2=TX, polling)
 * UART1  →  ESP32 SuperMini  (9600 bps, PTE0=RX / PTE1=TX, con IRQ)
 */

void bsp_init(void);

void uart0_debug_init(void);   /* UART0 para Terminal  – 115200 bps (Polling) */
void uart1_esp32_init(void);   /* UART1 para ESP32     – 9600 bps   (IRQ)     */

/* Transmisión por UART0 (terminal) */
void uart0_send_byte(char c);
void uart0_send_string(const char *s);

#endif /* _BSPINIT_H_ */
