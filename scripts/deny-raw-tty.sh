#!/usr/bin/env bash
# Deny agent shell that opens a TTY without smolmux.
# Allow first-exe smolmux / smolmux-cli / openocd (broker, with-port, SWD).
# Deny screen, socat, and any other command that names /dev/ttyUSB* or
# /dev/ttyACM*.
#
# Usage:
#   deny-raw-tty.sh --check "command"
#   deny-raw-tty.sh --pretooluse
#   deny-raw-tty.sh --self-test
# Exit 0 allow, 2 deny. Fail open on parse errors so a hook crash cannot
# brick every Bash call.
set -u

block() { printf '%s\n' "$1" >&2; exit 2; }

# Basename of the first real executable. Skip env assignments and common
# wrappers so "sudo smolmux /dev/ttyUSB0" still allows and
# "cat /dev/ttyUSB2; smolmux" still denies.
first_exe() {
    local s="$1"
    local flat
    flat=$(printf '%s' "$s" | sed 's/[;|&]/ /g')
    local -a words
    # shellcheck disable=SC2206
    read -r -a words <<< "$flat"
    local w
    for w in "${words[@]}"; do
        case "$w" in
            *=*) continue ;;
            sudo|command|env|nice|timeout|nohup) continue ;;
            '') continue ;;
            *)
                w="${w##*/}"
                printf '%s' "$w"
                return 0
                ;;
        esac
    done
    printf ''
}

cmd_is_allow() {
    local exe
    exe=$(first_exe "$1")
    case "$exe" in
        smolmux|smolmux-cli|openocd|smolmux-deny-raw-tty) return 0 ;;
    esac
    return 1
}

cmd_is_deny() {
    local cmd="$1"
    if printf '%s' "$cmd" | grep -Eq '(^|[^[:alnum:]_-])(screen|socat)([^[:alnum:]_-]|$)'; then
        return 0
    fi
    if printf '%s' "$cmd" | grep -Eq '/dev/tty(USB|ACM)[0-9]*'; then
        return 0
    fi
    return 1
}

check_cmd() {
    local cmd="$1"
    [ -n "$cmd" ] || return 0
    if cmd_is_allow "$cmd"; then
        return 0
    fi
    if cmd_is_deny "$cmd"; then
        block "BLOCKED by smolmux-deny-raw-tty: use smolmux or smolmux-cli with-port, not screen/socat/raw TTY.
Command: $cmd"
    fi
    return 0
}

self_test() {
    local me="$0"
    expect_ok() {
        "$me" --check "$1" || { echo "FAIL allow: $1" >&2; exit 1; }
    }
    expect_deny() {
        if "$me" --check "$1"; then
            echo "FAIL deny: $1" >&2
            exit 1
        fi
    }
    expect_ok "smolmux-cli with-port esptool --chip esp32s3"
    expect_ok "smolmux /dev/ttyUSB0 -b 115200"
    expect_ok "openocd -v"
    expect_ok "ls /tmp"
    expect_deny "cat /dev/ttyUSB2"
    expect_deny "echo hi > /dev/ttyUSB2"
    expect_deny "screen /dev/ttyACM0 115200"
    expect_deny "socat - /dev/ttyUSB0"
    expect_deny "cat /dev/ttyUSB2; smolmux -V"
    if command -v jq >/dev/null 2>&1; then
        if printf '%s' '{"tool_name":"Bash","tool_input":{"command":"cat /dev/ttyUSB2"}}' \
            | "$me" --pretooluse; then
            echo "FAIL: Claude payload should deny" >&2
            exit 1
        fi
        if printf '%s' '{"toolName":"run_terminal_command","toolInput":{"command":"cat /dev/ttyUSB2"}}' \
            | "$me" --pretooluse; then
            echo "FAIL: Grok payload should deny" >&2
            exit 1
        fi
        if ! printf '%s' '{"toolName":"run_terminal_command","toolInput":{"command":"smolmux-cli with-port true"}}' \
            | "$me" --pretooluse; then
            echo "FAIL: Grok with-port should allow" >&2
            exit 1
        fi
    fi
    printf 'self-test ok\n'
    exit 0
}

case "${1:-}" in
    --self-test) self_test ;;
    --check)
        check_cmd "${2:-}"
        exit 0
        ;;
    --pretooluse)
        command -v jq >/dev/null 2>&1 || exit 0
        input="$(cat)"
        tool="$(printf '%s' "$input" | jq -r '.tool_name // .toolName // empty' 2>/dev/null)" || exit 0
        case "$tool" in
            Bash|run_terminal_command) ;;
            *) exit 0 ;;
        esac
        cmd="$(printf '%s' "$input" | jq -r '.tool_input.command // .toolInput.command // empty' 2>/dev/null)" || exit 0
        check_cmd "$cmd"
        exit 0
        ;;
    -h|--help)
        printf 'usage: %s --check CMD | --pretooluse | --self-test\n' "$0"
        exit 0
        ;;
    *)
        printf 'usage: %s --check CMD | --pretooluse | --self-test\n' "$0" >&2
        exit 2
        ;;
esac
