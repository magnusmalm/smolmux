#ifndef SM_MONITOR_ESC_H
#define SM_MONITOR_ESC_H

/* Escape-command kinds for smolmux-monitor. 'c' is not a role upgrade
 * (no second hello). Restart with -c. */
enum {
    SM_MON_ESC_FORWARD = 0,
    SM_MON_ESC_QUIT,
    SM_MON_ESC_HELP,
    SM_MON_ESC_STATUS,
    SM_MON_ESC_RESTART_C,   /* deleted upgrade; print restart hint */
    SM_MON_ESC_TAKEOVER,
    SM_MON_ESC_RELEASE,
    SM_MON_ESC_BREAK,
    SM_MON_ESC_SUSPEND,
    SM_MON_ESC_RESUME
};

static inline int sm_mon_esc_kind(int ch)
{
    switch (ch) {
    case 'q': return SM_MON_ESC_QUIT;
    case 'h':
    case '?': return SM_MON_ESC_HELP;
    case 's': return SM_MON_ESC_STATUS;
    case 'c': return SM_MON_ESC_RESTART_C;
    case 't': return SM_MON_ESC_TAKEOVER;
    case 'r': return SM_MON_ESC_RELEASE;
    case 'b': return SM_MON_ESC_BREAK;
    case 'z': return SM_MON_ESC_SUSPEND;
    case 'Z': return SM_MON_ESC_RESUME;
    default:  return SM_MON_ESC_FORWARD;
    }
}

/* Broker JSON the escape arm may send. Connect hello is not an escape
 * action ('c' is RESTART_C → NONE). */
enum {
    SM_MON_WIRE_NONE = 0,
    SM_MON_WIRE_STATUS,
    SM_MON_WIRE_TAKEOVER,
    SM_MON_WIRE_RELEASE,
    SM_MON_WIRE_BREAK,
    SM_MON_WIRE_SUSPEND,
    SM_MON_WIRE_RESUME
};

static inline int sm_mon_esc_wire(int kind)
{
    switch (kind) {
    case SM_MON_ESC_STATUS:   return SM_MON_WIRE_STATUS;
    case SM_MON_ESC_TAKEOVER: return SM_MON_WIRE_TAKEOVER;
    case SM_MON_ESC_RELEASE:  return SM_MON_WIRE_RELEASE;
    case SM_MON_ESC_BREAK:    return SM_MON_WIRE_BREAK;
    case SM_MON_ESC_SUSPEND:  return SM_MON_WIRE_SUSPEND;
    case SM_MON_ESC_RESUME:   return SM_MON_WIRE_RESUME;
    default:                  return SM_MON_WIRE_NONE;
    }
}

#endif /* SM_MONITOR_ESC_H */
