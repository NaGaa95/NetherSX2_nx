/*
 * Copyright (c) 2026 Dolphin-NX contributors.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#pragma once

#ifndef __USBHSFS_UASP_H__
#define __USBHSFS_UASP_H__

#include "usbhsfs_drive.h"

typedef struct {
    u8 scsi_status;
    bool sense_valid;
    u8 sense_key;
    u8 additional_sense_code;
    u8 additional_sense_code_qualifier;
    u32 data_transferred;
} UsbHsFsUaspCommandResult;

bool usbHsFsUaspInitialize(UsbHsFsDriveContext *drive_ctx);
void usbHsFsUaspCleanup(UsbHsFsDriveContext *drive_ctx);

u8 usbHsFsUaspGetMaxLogicalUnits(UsbHsFsDriveContext *drive_ctx);

bool usbHsFsUaspTransferCommand(UsbHsFsDriveContext *drive_ctx, u32 data_size,
                                bool data_in, u8 lun, const u8 *cdb, u8 cdb_size,
                                void *buf, UsbHsFsUaspCommandResult *out_result);

/* Switch a dual-interface/alternate-setting device back to BOT. */
bool usbHsFsUaspFallbackToBulkOnlyTransport(UsbHsFsDriveContext *drive_ctx);

#endif /* __USBHSFS_UASP_H__ */


