/**
 * @file tests/host/param_lock_tests.h
 * @brief ParamLock / ParamLockManager test battery.
 */

#pragma once

/** Run the looping-automation tests (record, playback, sample clock,
 *  serialization, preset budgeting).  Adds to the caller's totals. */
int RunParamLockTests(int& checks, int& failures);
