#ifndef SM_LINK_SERIAL_TCP_H
#define SM_LINK_SERIAL_TCP_H

#include "links/link.h"

/*
 * serial-over-TCP device link: the broker connects OUT to a raw/telnet TCP
 * device server (ser2net, socat TCP-LISTEN, esp-link, a terminal server) and
 * treats the byte stream as the device. Telnet IAC negotiation is stripped from
 * the inbound stream and refused, so ser2net's default telnet mode works; a
 * telnet-escaped 0xFF (IAC IAC) is unescaped back to a literal 0xFF.
 *
 * When the server offers RFC 2217 (COM-PORT-OPTION), it is accepted so
 * set_param can drive baud / DTR / RTS / break over the network. Negotiation is
 * reactive (we never initiate COM-PORT), so a raw server sees no telnet bytes.
 *
 * Returns a link that connects to host:port on open(), or NULL on allocation
 * failure. The connect itself is bounded by SM_SERIAL_TCP_CONNECT_TIMEOUT_MS.
 */
sm_link_t *sm_serial_tcp_new(const char *host, int port);

#endif /* SM_LINK_SERIAL_TCP_H */
