#ifndef SM_TCP_SINK_H
#define SM_TCP_SINK_H

#include "sink.h"

sm_sink_t *sm_tcp_sink_new(int port, const char *bind_addr);

#endif /* SM_TCP_SINK_H */
