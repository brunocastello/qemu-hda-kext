/*
 * kmod_info.c
 * Equivalent of the <product>_info.c file Xcode generates for kernel
 * extensions. Defines the kmod_info structure the kernel reads when the kext
 * is loaded. QHDA_BUNDLE_ID and QHDA_VERSION_STRING come from the Makefile
 * and must match CFBundleIdentifier / CFBundleVersion in Info.plist.
 *
 * SPDX-License-Identifier: MIT
 */

#include <mach/mach_types.h>

#ifndef QHDA_BUNDLE_ID
#error QHDA_BUNDLE_ID must be defined by the build
#endif
#ifndef QHDA_VERSION_STRING
#error QHDA_VERSION_STRING must be defined by the build
#endif
#ifndef __APPLE_CC__
#define __APPLE_CC__ 6000
#endif

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

/* Indirection so QHDA_BUNDLE_ID is macro-expanded before KMOD_EXPLICIT_DECL
 * stringifies it (it uses #name). */
#define QHDA_KMOD_DECL(id, ver) KMOD_EXPLICIT_DECL(id, ver, _start, _stop)

__attribute__((visibility("default")))
QHDA_KMOD_DECL(QHDA_BUNDLE_ID, QHDA_VERSION_STRING)

__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t  *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
