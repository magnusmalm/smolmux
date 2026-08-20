#ifndef SM_TCP_SINK_H
#define SM_TCP_SINK_H

#include "sink.h"

sm_sink_t *sm_tcp_sink_new(int port, const char *bind_addr);

/* Would sm_tcp_sink_new() bind this address to loopback only?
 *
 * Lives here, next to the sink, so callers deciding whether a bind is exposed
 * cannot drift from how the sink actually resolves the string — including the
 * fallback where NULL or an unparseable address becomes loopback. Returns 1
 * for loopback (127.0.0.0/8 and the fallback), 0 for anything reachable from
 * another host, notably 0.0.0.0. */
int sm_tcp_bind_is_loopback(const char *bind_addr);

/* Must the broker refuse to serve TCP with these settings?
 *
 * A tokenless TCP sink gives every reachable peer a full controller session.
 * On loopback that is a local-trust decision (warn); bound anywhere else it
 * is an open console on the network, so refuse unless the operator opted in.
 *
 * Pure so the truth table can be tested directly, and the startup path in
 * main.c goes through this and nothing else, so the two cannot drift.
 * Returns 1 to refuse, 0 to proceed. */
int sm_tcp_refuse_unauthenticated(const char *bind_addr, int have_auth_token,
                                  int allow_insecure);

#endif /* SM_TCP_SINK_H */
