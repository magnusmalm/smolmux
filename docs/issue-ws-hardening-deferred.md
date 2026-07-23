# ISSUE: WebSocket sink hardening (deferred, local-only)

**Status:** OPEN - deferred
**Severity:** LOW (both items)
**Component:** `src/sinks/ws.c`
**Date:** 2026-07-02
**Related:** in-code `NOTE:` comments above the affected code in `ws.c`

## Shared context

The WebSocket sink's listener binds **loopback only**
(`ws.c:561`, `addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK)`), so only a
process on the same host can connect. Both items below are therefore
**local-only** concerns. They were reviewed, judged not worth their fix's
risk/cost given the loopback binding, and consciously deferred rather than
silently dropped. If the WS listener is ever exposed beyond loopback, both
must be revisited **before** that change ships.

---

## ISSUE-WS-1 (LOW): synchronous handshake enables a local WS-thread stall

**Where:** `ws_handshake()` - `src/sinks/ws.c:174`, called inline in the accept
branch of the single WS thread's poll loop (`ws.c:370`). The request read loop
(`ws.c:181`) polls up to `50 x 100 ms`.

**Problem:** the handshake is performed synchronously inside the one WS thread.
A client that connects and never sends a complete request
(`\r\n\r\n`) holds the thread for the full ~5 s budget. During that window the
thread services no other WS client (no device output delivered, pings
unanswered) and accepts no new connections. A local process looping
connect-and-stall can keep the WS sink effectively dead.

**Threat model:** local-only DoS. A browser cannot control byte timing well
enough to stall the handshake, so the malicious-webpage path does not apply;
this needs a local non-browser process. Availability of a loopback-only debug
transport is the only thing at risk.

**Why deferred:** a correct fix tracks per-connection handshake state in the
thread's `poll()` set (like the data path already does) with a wall-clock
deadline enforced across iterations - a non-trivial concurrency refactor of the
WS thread. For a loopback-only availability issue, that rewrite's regression
risk outweighs the benefit.

**Proposed fix:**
- Give each accepted-but-not-yet-handshaked fd an entry in the poll set with a
  per-connection deadline; drive the handshake incrementally as bytes arrive.
- Drop any connection that misses the deadline without blocking the thread.
- Cap concurrent in-progress handshakes.

**Acceptance criteria:** a client that connects and sends nothing does not
delay device-output delivery to other WS clients, and does not block new
accepts, beyond a short bounded per-connection deadline.

---

## ISSUE-WS-2 (LOW): Origin check fails open when the header is absent

**Where:** `ws.c:220` - `find_header(buf, "Origin", ...)`; the localhost-origin
allowlist runs only inside `if (origin) { ... }`.

**Problem:** when a request carries no `Origin` header, the cross-origin
(CSWSH) check is skipped entirely and the connection is allowed.

**Assessment - intentional, not a weakening:** this is deliberately left
fail-open and is **not** a CSWSH hole:
- Browsers **always** send `Origin` on WebSocket handshakes, so a malicious web
  page is still caught by the present-and-non-localhost rejection.
- A missing `Origin` means a **non-browser** local client (e.g. `websocat`, the
  smolmux monitor), which is not a CSWSH vector.
- The listener is loopback-only, so failing open does not widen exposure.

Failing **closed** on absent `Origin` would only break legitimate non-browser
local clients for no real security gain, so the behavior is intentionally
unchanged. Documented here so the decision is explicit and revisitable.

**Revisit if:** the WS listener is bound beyond loopback, or a requirement
appears to restrict WS to browser clients only. In that case, fail closed on
missing `Origin` (and reconsider requiring an allowlisted `Origin`).

**Acceptance criteria (if reopened):** with a non-loopback bind, a handshake
lacking a valid localhost/allowlisted `Origin` is rejected.

---

## Related fixes already in tree

Other findings from the same review are fixed: MCP report-builder heap overflows,
GDB `python`/`guile`/`make` RCE guard bypass, three broker/CLI stability
defects, a LOW hardening batch, and the coalescer O(K²) head cap. Only the two
WS items above remain open.
