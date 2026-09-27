/**
 * @file tests/host/firmware_tests.h
 * @brief FirmwareCatalog test battery (full FatFS stack on the RAM disk).
 */

#pragma once

/** Run the FirmwareCatalog tests.  Adds to @p checks / @p failures and
 *  returns this battery's failure count. */
int RunFirmwareTests(int& checks, int& failures);
