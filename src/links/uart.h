#ifndef SM_UART_H
#define SM_UART_H

#include "links/link.h"

sm_link_t *sm_uart_new(const char *port, int baud, int exclusive);

#endif /* SM_UART_H */
