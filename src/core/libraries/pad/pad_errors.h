// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/libraries/error_codes.h"

// Pad library
constexpr int ORBIS_PAD_ERROR_INVALID_ARG = 0x80920001;
constexpr int ORBIS_PAD_ERROR_INVALID_PORT = 0x80920002;
constexpr int ORBIS_PAD_ERROR_INVALID_HANDLE = 0x80920003;
constexpr int ORBIS_PAD_ERROR_ALREADY_OPENED = 0x80920004;
constexpr int ORBIS_PAD_ERROR_NOT_INITIALIZED = 0x80920005;
constexpr int ORBIS_PAD_ERROR_INVALID_LIGHTBAR_SETTING = 0x80920006;
constexpr int ORBIS_PAD_ERROR_DEVICE_NOT_CONNECTED = 0x80920007;
constexpr int ORBIS_PAD_ERROR_DEVICE_NO_HANDLE = 0x80920008;
constexpr int ORBIS_PAD_ERROR_FATAL = 0x809200FF;
constexpr int ORBIS_PAD_ERROR_NOT_PERMITTED = 0x80920101;
constexpr int ORBIS_PAD_ERROR_INVALID_BUFFER_LENGTH = 0x80920102;
constexpr int ORBIS_PAD_ERROR_INVALID_REPORT_LENGTH = 0x80920103;
constexpr int ORBIS_PAD_ERROR_INVALID_REPORT_ID = 0x80920104;
constexpr int ORBIS_PAD_ERROR_SEND_AGAIN = 0x80920105;

constexpr s32 ORBIS_DEVICE_SERVICE_ERROR_INVALID_USER = 0x809b0001;
constexpr s32 ORBIS_DEVICE_SERVICE_ERROR_USER_NOT_LOGIN = 0x809b0081;
// Not confirmed against real hardware/SDK docs - inferred from the module's error code
// range (0x809bXXXX) and the "no queued event" idiom used by sceUserServiceGetEvent
// (ORBIS_USER_SERVICE_ERROR_NO_EVENT, userservice_error.h) and other Get*Event PS4 APIs:
// OK when an event was dequeued, a distinct non-OK code otherwise. Used to stop
// sceDeviceServiceGetEventState (currently only a generic aerolib stub, see
// core/aerolib/stubs.cpp) from always reporting "event ready" and sending callers into a
// tight poll loop. See GT7-NOTES.md for the investigation that led here.
constexpr s32 ORBIS_DEVICE_SERVICE_ERROR_NO_EVENT = 0x809b0002;
