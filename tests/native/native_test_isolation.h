/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NATIVE_TEST_ISOLATION_H
#define NATIVE_TEST_ISOLATION_H

#include "nk_types.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NATIVE_TEST_PATH_MAX 1024

/* Creates a per-run temporary root named by tag and process id, arranges for
 * removal at exit and on SIGABRT. Must be called before any fixture is written. */
void native_test_create_root(const char *tag);

/* Points per-user environment variables (LOCALAPPDATA/APPDATA on Windows,
 * XDG variables and HOME on POSIX) into the temporary root created by
 * native_test_create_root. */
void native_test_isolate_user_data_roots(void);

/* Guard: fails when the per-user cache root resolves outside the temporary
 * root. Call after native_test_isolate_user_data_roots. */
void native_test_assert_cache_root_isolated(void);

/* Guard: fails when the per-user config root resolves outside the temporary
 * root. Call after native_test_isolate_user_data_roots. */
void native_test_assert_config_root_isolated(void);

/* Captures the real config path before isolation, for later verification that
 * the real config directory was not touched. */
void native_test_capture_real_config_probe(void);

/* Verifies the real config directory (and input_profiles subdirectory) was not
 * modified during the test run. */
void native_test_assert_real_config_untouched(void);

/* Returns the path to the temporary root created for this run. */
const char *native_test_get_root(void);

/* Returns the real config probe path captured before isolation. */
const char *native_test_get_real_config_probe(void);

/* Returns true when child lies within parent directory. */
bool native_test_path_within(const char *child, const char *parent);

/* Removes a directory tree without following links. */
void native_test_remove_tree(const char *path);

/* Helper to set or clear (value == NULL or empty) an environment variable. */
void native_test_set_env(const char *name, const char *value);

#ifdef __cplusplus
}
#endif

#endif /* NATIVE_TEST_ISOLATION_H */
