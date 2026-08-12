/**
 * @file alchemy/version.h
 * @brief SDK version string, reported over HostLink HELLO and stamped
 *        into descriptors.
 *
 * Bump on release.  Firmware builds may additionally stamp their own
 * app version + git hash (see HostLink::Info).
 */

#pragma once

#define ALCHEMY_SDK_VERSION "0.9.0"
