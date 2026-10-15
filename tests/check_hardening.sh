#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# check_hardening.sh linux <rayomd-gui>       (readelf)
# check_hardening.sh windows <rayomd-gui.exe> (objdump; OBJDUMP overrides the tool)
#
# Fails when a release executable lost an exploit mitigation the top-level CMakeLists.txt asks for.
# Linux: a PIE bound at load time (FLAGS_1 NOW, BIND_NOW) with a read-only GNU_RELRO segment, a
# non-executable GNU_STACK, the CET property (IBT and SHSTK: every object, the assembly kernels
# included, was built with -fcf-protection), stack canaries (__stack_chk_fail) and fortified libc
# calls (__*_chk), and NEEDED exactly libc.so.6 and ld-linux-x86-64.so.2. Windows: DllCharacteristics
# with HIGH_ENTROPY_VA, DYNAMIC_BASE and NX_COMPAT (0x0160), and no import of the DLLs loaded on
# first use (comdlg32, shell32, dwmapi, imm32).
set -u
if [ $# -ne 2 ]; then
    echo "usage: check_hardening.sh linux|windows <executable>" >&2
    exit 2
fi
binary=$2
failed=0
fail() {
    echo "check_hardening: $binary: $1" >&2
    failed=1
}

case $1 in
linux)
    header=$(readelf -hW "$binary") || exit 1
    dynamic=$(readelf -dW "$binary")
    segments=$(readelf -lW "$binary")
    notes=$(readelf -nW "$binary")
    symbols=$(readelf --dyn-syms -W "$binary")
    echo "$header" | grep -q 'Type: *DYN' || fail "not a position-independent executable"
    echo "$dynamic" | grep 'FLAGS_1' | grep -q 'PIE' || fail "FLAGS_1 lacks PIE"
    echo "$dynamic" | grep 'FLAGS_1' | grep -q 'NOW' || fail "FLAGS_1 lacks NOW (-z now)"
    echo "$dynamic" | grep -q 'BIND_NOW' || fail "no BIND_NOW (-z now)"
    echo "$segments" | grep -q 'GNU_RELRO' || fail "no GNU_RELRO segment (-z relro)"
    stack=$(echo "$segments" | grep 'GNU_STACK')
    [ -n "$stack" ] || fail "no GNU_STACK segment"
    echo "$stack" | grep -q 'RWE' && fail "executable stack: $stack"
    echo "$notes" | grep -q 'x86 feature: IBT, SHSTK' || fail "no x86 feature: IBT, SHSTK property (-fcf-protection on every object)"
    echo "$symbols" | grep -q ' __stack_chk_fail' || fail "no __stack_chk_fail (-fstack-protector-strong)"
    echo "$symbols" | grep -v '__stack_chk_fail' | grep -Eq ' __[a-z0-9_]+_chk(@|$)' || fail "no __*_chk call (-D_FORTIFY_SOURCE)"
    needed=$(echo "$dynamic" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | sort | tr '\n' ' ')
    [ "$needed" = "ld-linux-x86-64.so.2 libc.so.6 " ] || fail "unexpected NEEDED entries: $needed"
    echo "$dynamic" | grep -E 'FLAGS|NEEDED'
    echo "$segments" | grep -E 'GNU_RELRO|GNU_STACK'
    echo "$notes" | grep 'x86 feature:'
    echo "fortified: $(echo "$symbols" | grep -oE ' __[a-z0-9_]+_chk(@|$)' | tr -d ' @' | sort -u | tr '\n' ' ')"
    ;;
windows)
    objdump=${OBJDUMP:-objdump}
    headers=$("$objdump" -p "$binary") || exit 1
    characteristics=$(echo "$headers" | sed -n 's/^DllCharacteristics[[:space:]]*\([0-9a-fA-F]*\).*/\1/p')
    [ -n "$characteristics" ] || fail "no DllCharacteristics"
    if [ -n "$characteristics" ] && [ $((0x$characteristics & 0x0160)) -ne $((0x0160)) ]; then
        fail "DllCharacteristics 0x$characteristics lacks HIGH_ENTROPY_VA, DYNAMIC_BASE or NX_COMPAT (0x0160)"
    fi
    imports=$(echo "$headers" | sed -n 's/.*DLL Name: *//p' | tr 'A-Z' 'a-z')
    for dll in comdlg32.dll shell32.dll dwmapi.dll imm32.dll; do
        echo "$imports" | grep -qx "$dll" && fail "imports $dll, which must be loaded on first use"
    done
    echo "DllCharacteristics 0x$characteristics"
    echo "imports: $(echo "$imports" | tr '\n' ' ')"
    ;;
*)
    echo "usage: check_hardening.sh linux|windows <executable>" >&2
    exit 2
    ;;
esac
if [ "$failed" -ne 0 ]; then exit 1; fi
echo "check_hardening: $binary: ok"
