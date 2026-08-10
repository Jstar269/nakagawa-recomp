// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
//
// fbcap_policy.h - policy governing the frame-capture visual-oracle slot (issue #57)
//
// The host runtime can capture exactly one presented frame at a time.  Two independent
// debug switches both want that slot; this header decides, deterministically, who owns
// it and what the capture means, so the renderer never has to guess:
//
//   SR_FBDUMP ("present_source.ppm", issue #37)
//     Diagnostic owner of the NEXT frame only: on the next present, the renderer
//     publishes an exact P6 PPM of what the presentation engine actually received, then
//     the runtime exits.  "No lying" extends to exits: the process may only exit
//     success(0) if the capture really was published.
//
//   SR_FBSNAP ("build/snapshots/frame_%04u.ppm", issue #24)
//     Visual-oracle owner: every frame armed by the visual-oracle route publishes its
//     P6 PPM under build/snapshots/.
//
// Ownership rules (pure function; unit-tested in gpu_capture_selftest):
//   - both unset                    -> none
//   - only SR_FBSNAP                -> SR_FBSNAP
//   - only SR_FBDUMP                -> SR_FBDUMP
//   - both set                      -> SR_FBDUMP (diagnostic wins; it must not be
//                                         silently starved out by the oracle loop)
//   - SR_FBSNAP value "0"           -> none   (switch values are parsed as numbers,
//                                         see sdl3vk.c _cap_env_isset)
//   - SR_FBDUMP value "0"           -> none
//
// Exit-status table (SR_FBDUMP route):
//   capture result 1  -> 0 (the presented frame WAS published before exit)
//   capture result 0  -> 1 (nothing was presented before exit: fail closed)
//   capture result -1 -> 1 (a capture was attempted but failed: fail closed)
//
// Paths: both files are exact P6 PPMs regardless of source format; the renderer keeps
// the BGRA/RGBA byte order internal, so .ppm never lies about being .png or .jpg.
#ifndef NAKAGAWA_RECOMP_FBCAP_POLICY_H
#define NAKAGAWA_RECOMP_FBCAP_POLICY_H

enum {
    SR_FBCAP_NONE = 0,
    SR_FBCAP_FBSNAP,
    SR_FBCAP_FBDUMP
};

#ifdef __cplusplus
extern "C" {
#endif

/* 0 = switch off; the value is parsed as a number so the legacy literal "0" disables. */
int sr_fbcap_env_on(const char *name);

/* Who owns the single capture slot?  Both-set goes to SR_FBDUMP. */
int sr_fbcap_owner(int fbdu, int fbsnap);

/* FBSNAP frames are numbered by the visual-oracle route. */
int sr_fbcap_path(int owner, int index, char *out, size_t outsz);

/* Exit status for the SR_FBDUMP route (see header comment). */
int sr_fbcap_exit_status(int owner, int capture_result);

#ifdef __cplusplus
}
#endif

#endif /* NAKAGAWA_RECOMP_FBCAP_POLICY_H */
