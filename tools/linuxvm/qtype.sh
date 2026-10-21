#!/bin/bash
# qtype.sh <vm> <us|de> <text> - types text into a VM one key at a time through QMP (`linuxvm.sh type` drops characters
# under load). Letters, digits and  . - / : ; _ + = @ | < > ~ & $ ( ) ' \" space are mapped for the given guest keyboard layout; "ret" as the
# whole text presses Enter. For the German layout y and z are swapped, - is the slash key, : is shift-dot, / is shift-7.
HERE=$(cd "$(dirname "$0")" && pwd)
vm=$1 layout=$2 text=$3
k() { "$HERE/linuxvm.sh" keys "$vm" "$1" >/dev/null; sleep ${QTYPE_DELAY:-0.25}; }
[ "$text" = ret ] && { k ret; exit 0; }
for ((i = 0; i < ${#text}; i++)); do
    c=${text:i:1}
    case $c in
    [a-z0-9])
        if [ "$layout" = de ]; then case $c in y) c=z ;; z) c=y ;; esac; fi; k "$c" ;;
    [A-Z]) c=$(echo "$c" | tr A-Z a-z); if [ "$layout" = de ]; then case $c in y) c=z ;; z) c=y ;; esac; fi; k "shift-$c" ;;
    ' ') k spc ;;
    .) k dot ;;
    -) [ "$layout" = de ] && k slash || k minus ;;
    /) [ "$layout" = de ] && k shift-7 || k slash ;;
    :) [ "$layout" = de ] && k shift-dot || k shift-semicolon ;;
    _) [ "$layout" = de ] && k shift-slash || k shift-minus ;;
    \;) [ "$layout" = de ] && k shift-comma || k semicolon ;;
    =) [ "$layout" = de ] && k shift-0 || k equal ;;
    +) [ "$layout" = de ] && k bracket_right || k shift-equal ;;
    @) [ "$layout" = de ] && k alt_r-q || k shift-2 ;;
    '|') [ "$layout" = de ] && k alt_r-less || k shift-backslash ;;
    '>') [ "$layout" = de ] && k shift-less || k shift-dot ;;
    '<') [ "$layout" = de ] && k less || k shift-comma ;;
    '~') [ "$layout" = de ] && k alt_r-bracket_right || k shift-grave_accent ;;
    '&') [ "$layout" = de ] && k shift-6 || k shift-7 ;;
    '$') [ "$layout" = de ] && k shift-4 || k shift-4 ;;
    '(') [ "$layout" = de ] && k shift-8 || k shift-9 ;;
    ')') [ "$layout" = de ] && k shift-9 || k shift-0 ;;
    "'") [ "$layout" = de ] && k shift-backslash || k apostrophe ;;
    '"') [ "$layout" = de ] && k shift-2 || k shift-apostrophe ;;
    *) echo "qtype: no mapping for '$c'" >&2; exit 1 ;;
    esac
done
