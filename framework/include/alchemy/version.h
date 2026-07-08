/**
 * @file alchemy/version.h
 * @brief SDK version string, reported over HostLink HELLO and stamped
 *        into descriptors.
 *
 * Bump on release.  Firmware builds may additionally stamp their own
 * app version + git hash (see HostLink::Info).
 */

#pragma once

#define ALCHEMY_SDK_VERSION "1.1.0-beta"
