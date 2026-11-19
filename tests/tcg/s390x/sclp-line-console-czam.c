/*
 * Line-mode console as the default first serial of the czam machine.
 *
 * Same guest as sclp-line-console.c; meson runs it with only -serial stdio
 * on machine czam (KVM), so the operator command typed on stdin must
 * reach the guest through the default line-mode console mapping.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "sclp-line-console.c"
