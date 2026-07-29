/**
 * @file tests/host/button_tests.h
 * @brief VirtualButton / ButtonBank test battery.
 */

#pragma once

/** Run the button-surface tests (roster, serialization, gestures,
 *  consume handshake, descriptor emission).  Adds to the caller's
 *  totals. */
int RunButtonTests(int& checks, int& failures);
