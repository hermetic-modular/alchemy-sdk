/**
 * @file tests/host/fs_tests.h
 * @brief Filesystem-block (protocol §8) test battery + golden emitter.
 */

#pragma once

#include <cstdio>

/** Run the §8 filesystem tests (full FatFS stack on the RAM disk).
 *  Prints failures; returns the failure count and adds to the totals
 *  via the returned pair semantics of the caller. */
int RunFsTests(int& checks, int& failures);

/** Emit the `"fs_exchange"` golden array (inner content of the JSON
 *  key) — a deterministic FS command exchange recorded frame-by-frame
 *  for the cross-language interop tests.  Writes
 *  `  "fs_exchange": [ ... ],\n` including the trailing comma. */
void EmitFsGolden(FILE* f);
