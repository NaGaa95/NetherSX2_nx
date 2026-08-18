/*
 * USB Attached SCSI transport for libusbhsfs on Horizon OS.
 *
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
 *
 * The wire layouts below follow USB-IF USB Attached SCSI (UAS) revision 1.0
 * and INCITS SAM/SPC. They are protocol definitions, implemented here from
 * scratch. This file deliberately remains ISC licensed like libusbhsfs.
 */

#include "usbhsfs_utils.h"
#include "usbhsfs_request.h"
#include "usbhsfs_uasp.h"

#define UASP_MAX_CONTEXTS              0x20
#define UASP_IU_BUFFER_SIZE            USB_XFER_BUF_ALIGNMENT
#define UASP_STATUS_TIMEOUT            USB_POSTBUFFER_TIMEOUT
#define UASP_REPORT_LUNS_SIZE          0x100

#define SCSI_STATUS_GOOD               0x00
#define SCSI_STATUS_CHECK_CONDITION    0x02

#define SCSI_OPCODE_REPORT_LUNS        0xA0

typedef enum {
    UaspInformationUnitId_Command    = 0x01,
    UaspInformationUnitId_Sense      = 0x03,
    UaspInformationUnitId_Response   = 0x04,
    UaspInformationUnitId_TaskMgmt   = 0x05,
    UaspInformationUnitId_ReadReady  = 0x06,
    UaspInformationUnitId_WriteReady = 0x07
} UaspInformationUnitId;

typedef enum {
    UaspTaskManagementFunction_AbortTask        = 0x01,
    UaspTaskManagementFunction_LogicalUnitReset = 0x08
} UaspTaskManagementFunction;

typedef enum {
    UaspResponseCode_TaskManagementComplete  = 0x00,
    UaspResponseCode_TaskManagementSucceeded = 0x08
} UaspResponseCode;

typedef enum {
    UaspEndpointRole_Status = 0,
    UaspEndpointRole_Command,
    UaspEndpointRole_DataIn,
    UaspEndpointRole_DataOut,
    UaspEndpointRole_Count
} UaspEndpointRole;

#pragma pack(push, 1)
typedef struct {
    u8 iu_id;
    u8 reserved_1;
    u16 tag;
} UaspInformationUnitHeader;

typedef struct {
    UaspInformationUnitHeader header;
    u8 priority_and_task_attribute;
    u8 reserved_5;
    u8 additional_cdb_length;
    u8 reserved_7;
    u8 lun[8];
    u8 cdb[16];
} UaspCommandInformationUnit;

typedef struct {
    UaspInformationUnitHeader header;
    u8 function;
    u8 reserved_5;
    u16 task_tag;
    u8 lun[8];
} UaspTaskManagementInformationUnit;

typedef struct {
    UaspInformationUnitHeader header;
    u16 status_qualifier;
    u8 status;
    u8 reserved_7[7];
    u16 sense_length;
    u8 sense_data[];
} UaspSenseInformationUnit;

typedef struct {
    UaspInformationUnitHeader header;
    u8 additional_response_information[3];
    u8 response_code;
} UaspResponseInformationUnit;
#pragma pack(pop)

LIB_ASSERT(UaspInformationUnitHeader, 4);
LIB_ASSERT(UaspCommandInformationUnit, 32);
LIB_ASSERT(UaspTaskManagementInformationUnit, 16);
LIB_ASSERT(UaspResponseInformationUnit, 8);

typedef struct {
    UsbHsClientEpSession *endpoint;
    u32 transfer_id;
    u64 request_id;
    bool pending;
} UaspAsyncTransfer;

typedef struct {
    UsbHsFsDriveContext *drive_ctx;
    Mutex io_mutex;
    u8 *command_buffer;
    u8 *status_buffer;
    u16 next_tag;
    u16 advertised_stream_count;
    struct usb_interface_descriptor interface_descriptor;
    struct usb_endpoint_descriptor endpoint_descriptors[UaspEndpointRole_Count];
    bool streams_enabled;
} UsbHsFsUaspContext;

static Mutex g_uasp_contexts_mutex = 0;
static UsbHsFsUaspContext *g_uasp_contexts[UASP_MAX_CONTEXTS] = {0};

static u16 uaspGetBe16(const void *ptr)
{
    u16 value = 0;
    memcpy(&value, ptr, sizeof(value));
    return __builtin_bswap16(value);
}

static u32 uaspGetBe32(const void *ptr)
{
    u32 value = 0;
    memcpy(&value, ptr, sizeof(value));
    return __builtin_bswap32(value);
}

static void uaspPutBe16(void *ptr, u16 value)
{
    value = __builtin_bswap16(value);
    memcpy(ptr, &value, sizeof(value));
}

static void uaspPutBe32(void *ptr, u32 value)
{
    value = __builtin_bswap32(value);
    memcpy(ptr, &value, sizeof(value));
}

static void uaspSetLun(u8 out_lun[8], u8 lun)
{
    memset(out_lun, 0, 8);
    out_lun[1] = lun;
}

static UsbHsFsUaspContext *uaspFindContext(UsbHsFsDriveContext *drive_ctx)
{
    for (u32 i = 0; i < UASP_MAX_CONTEXTS; i++)
    {
        if (g_uasp_contexts[i] && g_uasp_contexts[i]->drive_ctx == drive_ctx)
            return g_uasp_contexts[i];
    }

    return NULL;
}

static UsbHsFsUaspContext *uaspLockContext(UsbHsFsDriveContext *drive_ctx)
{
    UsbHsFsUaspContext *ctx = NULL;

    mutexLock(&g_uasp_contexts_mutex);
    ctx = uaspFindContext(drive_ctx);
    if (ctx) mutexLock(&ctx->io_mutex);
    mutexUnlock(&g_uasp_contexts_mutex);

    return ctx;
}

static u16 uaspAllocateTag(UsbHsFsUaspContext *ctx)
{
    u16 tag = ctx->next_tag++;
    if (!tag || tag == UINT16_MAX)
    {
        tag = 1;
        ctx->next_tag = 2;
    }
    return tag;
}

static u8 uaspGetEndpointStreamExponent(UsbHsClientIfSession *interface,
                                        const UsbHsClientEpSession *endpoint)
{
    const bool input = ((endpoint->desc.bEndpointAddress & USB_ENDPOINT_IN) != 0);

    for (u8 i = 0; i < 15; i++)
    {
        const struct usb_endpoint_descriptor *descriptor =
            input ? &interface->inf.inf.input_endpoint_descs[i] :
                    &interface->inf.inf.output_endpoint_descs[i];
        if (descriptor->bLength &&
            descriptor->bEndpointAddress == endpoint->desc.bEndpointAddress)
        {
            struct usb_ss_endpoint_companion_descriptor companion = {0};
            const void *companion_source = input ?
                (const void *)&interface->inf.inf.input_ss_endpoint_companion_descs[i] :
                (const void *)&interface->inf.inf.output_ss_endpoint_companion_descs[i];
            memcpy(&companion, companion_source, sizeof(companion));
            return (companion.bmAttributes & 0x1F);
        }
    }

    return 0;
}

static u16 uaspDetectAdvertisedStreams(UsbHsFsDriveContext *drive_ctx)
{
    UsbHsClientIfSession *interface = &drive_ctx->usb_if_session;
    u8 exponent = UINT8_MAX;

    const UsbHsClientEpSession *stream_endpoints[] = {
        &drive_ctx->usb_in_ep_session[0],
        &drive_ctx->usb_in_ep_session[1],
        &drive_ctx->usb_out_ep_session[1]
    };

    for (u32 i = 0; i < (sizeof(stream_endpoints) / sizeof(stream_endpoints[0])); i++)
    {
        const u8 current = uaspGetEndpointStreamExponent(interface, stream_endpoints[i]);
        if (!current) return 0;
        if (current < exponent) exponent = current;
    }

    if (exponent > 8) exponent = 8;
    return (u16)(1U << exponent);
}

static Result uaspBeginStatusTransfer(UsbHsFsUaspContext *ctx, u16 tag, u32 phase,
                                      UaspAsyncTransfer *transfer)
{
    UsbHsClientEpSession *endpoint = &ctx->drive_ctx->usb_in_ep_session[0];
    const u64 request_id = ((u64)tag << 32) | phase;

    memset(ctx->status_buffer, 0, UASP_IU_BUFFER_SIZE);
    armDCacheFlush(ctx->status_buffer, UASP_IU_BUFFER_SIZE);

    Result rc = usbHsEpPostBufferAsync(endpoint, ctx->status_buffer,
                                       UASP_IU_BUFFER_SIZE, request_id,
                                       &transfer->transfer_id);
    if (R_SUCCEEDED(rc))
    {
        transfer->endpoint = endpoint;
        transfer->request_id = request_id;
        transfer->pending = true;
    }

    return rc;
}

static Result uaspFinishAsyncTransfer(UaspAsyncTransfer *transfer, u32 *out_size)
{
    if (!transfer || !transfer->pending || !out_size)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    Event *event = usbHsEpGetXferEvent(transfer->endpoint);
    Result rc = eventWait(event, UASP_STATUS_TIMEOUT);
    if (R_FAILED(rc)) return rc;
    eventClear(event);

    UsbHsXferReport reports[4] = {0};
    u32 report_count = 0;
    rc = usbHsEpGetXferReport(transfer->endpoint, reports,
                              sizeof(reports) / sizeof(reports[0]), &report_count);
    if (R_FAILED(rc)) return rc;

    for (u32 i = 0; i < report_count; i++)
    {
        if (reports[i].xferId != transfer->transfer_id ||
            reports[i].id != transfer->request_id)
            continue;

        transfer->pending = false;
        *out_size = reports[i].transferredSize;
        return reports[i].res;
    }

    return MAKERESULT(Module_Libnx, LibnxError_BadUsbCommsRead);
}

static UsbHsClientEpSession *uaspGetEndpoint(UsbHsFsUaspContext *ctx,
                                             UaspEndpointRole role)
{
    if (!ctx || role >= UaspEndpointRole_Count) return NULL;

    switch(role)
    {
        case UaspEndpointRole_Status:
            return &ctx->drive_ctx->usb_in_ep_session[0];
        case UaspEndpointRole_Command:
            return &ctx->drive_ctx->usb_out_ep_session[0];
        case UaspEndpointRole_DataIn:
            return &ctx->drive_ctx->usb_in_ep_session[1];
        case UaspEndpointRole_DataOut:
            return &ctx->drive_ctx->usb_out_ep_session[1];
        default:
            return NULL;
    }
}

static void uaspCloseEndpoint(UsbHsFsUaspContext *ctx, UaspEndpointRole role)
{
    UsbHsClientEpSession *endpoint = uaspGetEndpoint(ctx, role);
    if (endpoint && serviceIsActive(&endpoint->s)) usbHsEpClose(endpoint);
}

static bool uaspOpenEndpoint(UsbHsFsUaspContext *ctx, UaspEndpointRole role)
{
    UsbHsClientEpSession *endpoint = uaspGetEndpoint(ctx, role);
    struct usb_endpoint_descriptor *descriptor =
        (ctx && role < UaspEndpointRole_Count) ? &ctx->endpoint_descriptors[role] : NULL;
    if (!endpoint || !descriptor || !descriptor->bLength ||
        (descriptor->bmAttributes & USB_TRANSFER_TYPE_MASK) != USB_TRANSFER_TYPE_BULK)
        return false;

    Result rc = usbHsIfOpenUsbEp(&ctx->drive_ctx->usb_if_session, endpoint, 1,
                                 descriptor->wMaxPacketSize, descriptor);
    if (R_FAILED(rc))
    {
        USBHSFS_LOG_MSG("Unable to reopen UASP endpoint 0x%02X! (0x%X) (interface %d).",
                        descriptor->bEndpointAddress, rc, ctx->drive_ctx->usb_if_id);
        return false;
    }
    return true;
}

static bool uaspReopenEndpoint(UsbHsFsUaspContext *ctx, UaspEndpointRole role)
{
    uaspCloseEndpoint(ctx, role);
    return uaspOpenEndpoint(ctx, role);
}

static void uaspCloseAllEndpoints(UsbHsFsUaspContext *ctx)
{
    for (u32 role = 0; role < UaspEndpointRole_Count; role++)
        uaspCloseEndpoint(ctx, (UaspEndpointRole)role);
}

static bool uaspOpenAllEndpoints(UsbHsFsUaspContext *ctx)
{
    for (u32 role = 0; role < UaspEndpointRole_Count; role++)
    {
        if (uaspOpenEndpoint(ctx, (UaspEndpointRole)role)) continue;
        uaspCloseAllEndpoints(ctx);
        return false;
    }
    return true;
}

static bool uaspReopenAllEndpoints(UsbHsFsUaspContext *ctx)
{
    /* Closing an endpoint is the only public usb:hs operation that reliably
     * cancels an async URB. Do this before reusing either IU buffer or the
     * drive-wide data buffer after any transport failure. */
    uaspCloseAllEndpoints(ctx);
    return uaspOpenAllEndpoints(ctx);
}

static bool uaspResetAndReinitializeTransport(UsbHsFsUaspContext *ctx)
{
    UsbHsClientIfSession *interface = &ctx->drive_ctx->usb_if_session;
    uaspCloseAllEndpoints(ctx);

    Result rc = usbHsIfResetDevice(interface);
    if (R_FAILED(rc))
    {
        USBHSFS_LOG_MSG("Unable to reset UASP device! (0x%X) (interface %d).",
                        rc, ctx->drive_ctx->usb_if_id);
        return false;
    }

    /* A USB reset returns the device to its default interface/alternate
     * setting. Explicitly select the cached UASP interface again before
     * opening fresh endpoint sessions. */
    svcSleepThread(100000000ULL);
    rc = usbHsIfSetInterface(interface, NULL,
                             ctx->interface_descriptor.bInterfaceNumber);
    if (R_SUCCEEDED(rc) && ctx->interface_descriptor.bAlternateSetting)
        rc = usbHsIfGetAlternateInterface(interface, NULL,
                                           ctx->interface_descriptor.bAlternateSetting);
    if (R_FAILED(rc) ||
        interface->inf.inf.interface_desc.bInterfaceProtocol !=
            USB_PROTOCOL_USB_ATTACHED_SCSI ||
        !uaspOpenAllEndpoints(ctx))
    {
        uaspCloseAllEndpoints(ctx);
        USBHSFS_LOG_MSG("Unable to reinitialize UASP after USB reset! (0x%X) (interface %d).",
                        rc, ctx->drive_ctx->usb_if_id);
        return false;
    }

    ctx->next_tag = 1;
    memset(ctx->command_buffer, 0, UASP_IU_BUFFER_SIZE);
    memset(ctx->status_buffer, 0, UASP_IU_BUFFER_SIZE);
    return true;
}

static void uaspCancelStatusTransfer(UsbHsFsUaspContext *ctx,
                                     UaspAsyncTransfer *transfer)
{
    if (!transfer || !transfer->pending) return;
    transfer->pending = false;
    uaspReopenEndpoint(ctx, UaspEndpointRole_Status);
}

static bool uaspSendCommandInformationUnit(UsbHsFsUaspContext *ctx, u16 tag,
                                           u8 lun, const u8 *cdb, u8 cdb_size)
{
    if (!cdb || !cdb_size || cdb_size > 16) return false;

    UaspCommandInformationUnit *command =
        (UaspCommandInformationUnit *)ctx->command_buffer;
    memset(command, 0, sizeof(*command));

    command->header.iu_id = UaspInformationUnitId_Command;
    uaspPutBe16(&command->header.tag, tag);
    uaspSetLun(command->lun, lun);
    memcpy(command->cdb, cdb, cdb_size);

    u32 transferred = 0;
    Result rc = usbHsFsRequestPostBuffer(&ctx->drive_ctx->usb_if_session,
                                         &ctx->drive_ctx->usb_out_ep_session[0],
                                         command, sizeof(*command), &transferred, true);
    return (R_SUCCEEDED(rc) && transferred == sizeof(*command));
}

static bool uaspParseSenseData(const u8 *sense, u32 sense_length,
                               UsbHsFsUaspCommandResult *result)
{
    if (!sense || !sense_length || !result) return false;

    const u8 response_code = (sense[0] & 0x7F);
    if ((response_code == 0x70 || response_code == 0x71) && sense_length >= 14)
    {
        result->sense_key = (sense[2] & 0x0F);
        result->additional_sense_code = sense[12];
        result->additional_sense_code_qualifier = sense[13];
    }
    else if ((response_code == 0x72 || response_code == 0x73) && sense_length >= 4)
    {
        result->sense_key = (sense[1] & 0x0F);
        result->additional_sense_code = sense[2];
        result->additional_sense_code_qualifier = sense[3];
    }
    else
    {
        return false;
    }

    result->sense_valid = true;
    return true;
}

static bool uaspParseCommandStatus(UsbHsFsUaspContext *ctx, u16 expected_tag,
                                   u32 size, UsbHsFsUaspCommandResult *result)
{
    if (size < sizeof(UaspInformationUnitHeader) || !result) return false;

    armDCacheFlush(ctx->status_buffer, UASP_IU_BUFFER_SIZE);
    const UaspInformationUnitHeader *header =
        (const UaspInformationUnitHeader *)ctx->status_buffer;

    if (uaspGetBe16(&header->tag) != expected_tag) return false;

    /* The data stage has already accumulated its byte count in result. */
    result->scsi_status = 0;
    result->sense_valid = false;
    result->sense_key = 0;
    result->additional_sense_code = 0;
    result->additional_sense_code_qualifier = 0;

    if (header->iu_id == UaspInformationUnitId_Response)
    {
        if (size < sizeof(UaspResponseInformationUnit)) return false;
        const UaspResponseInformationUnit *response =
            (const UaspResponseInformationUnit *)ctx->status_buffer;
        USBHSFS_LOG_MSG("Unexpected UASP Response IU 0x%02X for command tag %u (interface %d).",
                        response->response_code, expected_tag, ctx->drive_ctx->usb_if_id);
        return false;
    }

    if (header->iu_id != UaspInformationUnitId_Sense) return false;

    /* Pre-r02 UAS firmware used an 8-byte header. Keep compatibility with it. */
    const u8 *sense = NULL;
    u32 sense_length = 0;
    if (size < sizeof(UaspSenseInformationUnit))
    {
        if (size < 8) return false;
        sense_length = uaspGetBe16(ctx->status_buffer + 4);
        result->scsi_status = ctx->status_buffer[6];
        sense = ctx->status_buffer + 8;
        if (sense_length >= 2) sense_length -= 2;
        if (sense_length > (size - 8)) sense_length = (size - 8);
    }
    else
    {
        const UaspSenseInformationUnit *status =
            (const UaspSenseInformationUnit *)ctx->status_buffer;
        result->scsi_status = status->status;
        sense_length = uaspGetBe16(&status->sense_length);
        sense = status->sense_data;
        if (sense_length > (size - sizeof(*status)))
            sense_length = (size - sizeof(*status));
    }

    if (sense_length) uaspParseSenseData(sense, sense_length, result);
    return true;
}

static bool uaspParseReadyInformationUnit(UsbHsFsUaspContext *ctx, u16 expected_tag,
                                          bool data_in, u32 size)
{
    if (size < sizeof(UaspInformationUnitHeader)) return false;

    armDCacheFlush(ctx->status_buffer, UASP_IU_BUFFER_SIZE);
    const UaspInformationUnitHeader *header =
        (const UaspInformationUnitHeader *)ctx->status_buffer;
    const u8 expected_id = data_in ? UaspInformationUnitId_ReadReady :
                                    UaspInformationUnitId_WriteReady;
    return (header->iu_id == expected_id &&
            uaspGetBe16(&header->tag) == expected_tag);
}

static bool uaspSendTaskManagement(UsbHsFsUaspContext *ctx, u8 lun, u8 function,
                                   u16 command_tag)
{
    const u16 tag = uaspAllocateTag(ctx);
    UaspAsyncTransfer status_transfer = {0};

    Result rc = uaspBeginStatusTransfer(ctx, tag, 3, &status_transfer);
    if (R_FAILED(rc)) return false;

    UaspTaskManagementInformationUnit *task =
        (UaspTaskManagementInformationUnit *)ctx->command_buffer;
    memset(task, 0, sizeof(*task));
    task->header.iu_id = UaspInformationUnitId_TaskMgmt;
    uaspPutBe16(&task->header.tag, tag);
    task->function = function;
    uaspPutBe16(&task->task_tag, command_tag);
    uaspSetLun(task->lun, lun);

    u32 transferred = 0;
    rc = usbHsFsRequestPostBuffer(&ctx->drive_ctx->usb_if_session,
                                  &ctx->drive_ctx->usb_out_ep_session[0], task,
                                  sizeof(*task), &transferred, false);
    if (R_FAILED(rc) || transferred != sizeof(*task))
    {
        uaspCancelStatusTransfer(ctx, &status_transfer);
        return false;
    }

    u32 status_size = 0;
    rc = uaspFinishAsyncTransfer(&status_transfer, &status_size);
    if (R_FAILED(rc))
    {
        uaspCancelStatusTransfer(ctx, &status_transfer);
        return false;
    }

    armDCacheFlush(ctx->status_buffer, UASP_IU_BUFFER_SIZE);
    if (status_size < sizeof(UaspResponseInformationUnit)) return false;

    const UaspResponseInformationUnit *response =
        (const UaspResponseInformationUnit *)ctx->status_buffer;
    return (response->header.iu_id == UaspInformationUnitId_Response &&
            uaspGetBe16(&response->header.tag) == tag &&
            (response->response_code == UaspResponseCode_TaskManagementComplete ||
             response->response_code == UaspResponseCode_TaskManagementSucceeded));
}

static void uaspRecoverTransport(UsbHsFsUaspContext *ctx, u8 lun, u16 command_tag,
                                 UaspAsyncTransfer *pending_status)
{
    if (pending_status) pending_status->pending = false;

    /* Either the status or data URB may still be owned by usb:hs after a
     * timeout. Reopen every pipe before a task-management IU can reuse the
     * shared buffers. */
    if (uaspReopenAllEndpoints(ctx))
    {
        usbHsFsDriveClearStallStatus(ctx->drive_ctx);
        if (uaspSendTaskManagement(ctx, lun, UaspTaskManagementFunction_AbortTask,
                                   command_tag))
            return;
    }

    /* A failed task IU may itself have left an async command/status URB. Give
     * logical-unit reset a completely fresh endpoint set as well. */
    if (uaspReopenAllEndpoints(ctx))
    {
        usbHsFsDriveClearStallStatus(ctx->drive_ctx);
        if (uaspSendTaskManagement(ctx, lun,
                                   UaspTaskManagementFunction_LogicalUnitReset, 0))
            return;
    }

    USBHSFS_LOG_MSG("UASP task recovery failed; resetting and reinitializing USB transport (interface %d).",
                    ctx->drive_ctx->usb_if_id);
    uaspResetAndReinitializeTransport(ctx);
}

static bool uaspTransferCommandLocked(UsbHsFsUaspContext *ctx, u32 data_size,
                                      bool data_in, u8 lun, const u8 *cdb,
                                      u8 cdb_size, void *buf,
                                      UsbHsFsUaspCommandResult *result,
                                      bool recover)
{
    const u16 tag = uaspAllocateTag(ctx);
    UaspAsyncTransfer status_transfer = {0};
    u32 status_size = 0;
    bool transport_ok = false;
    bool data_transport_failed = false;

    memset(result, 0, sizeof(*result));

    Result rc = uaspBeginStatusTransfer(ctx, tag, 1, &status_transfer);
    if (R_FAILED(rc)) goto end;

    if (!uaspSendCommandInformationUnit(ctx, tag, lun, cdb, cdb_size)) goto end;

    rc = uaspFinishAsyncTransfer(&status_transfer, &status_size);
    if (R_FAILED(rc)) goto end;

    if (!data_size)
    {
        transport_ok = uaspParseCommandStatus(ctx, tag, status_size, result);
        goto end;
    }

    /* An early Sense/Response IU means the command was rejected before data. */
    if (!uaspParseReadyInformationUnit(ctx, tag, data_in, status_size))
    {
        transport_ok = uaspParseCommandStatus(ctx, tag, status_size, result);
        goto end;
    }

    rc = uaspBeginStatusTransfer(ctx, tag, 2, &status_transfer);
    if (R_FAILED(rc)) goto end;

    u8 *data = (u8 *)buf;
    UsbHsClientEpSession *data_endpoint = data_in ?
        &ctx->drive_ctx->usb_in_ep_session[1] :
        &ctx->drive_ctx->usb_out_ep_session[1];

    while (result->data_transferred < data_size)
    {
        const u32 remaining = data_size - result->data_transferred;
        const u32 requested = remaining > USB_XFER_BUF_SIZE ?
                              USB_XFER_BUF_SIZE : remaining;
        u32 transferred = 0;

        if (!data_in)
            memcpy(ctx->drive_ctx->xfer_buf, data + result->data_transferred,
                   requested);

        rc = usbHsFsRequestPostBuffer(&ctx->drive_ctx->usb_if_session,
                                      data_endpoint, ctx->drive_ctx->xfer_buf,
                                      requested, &transferred, false);
        if (transferred > requested)
        {
            rc = MAKERESULT(Module_Libnx, LibnxError_BadUsbCommsRead);
            data_transport_failed = true;
            break;
        }

        if (data_in && transferred)
            memcpy(data + result->data_transferred, ctx->drive_ctx->xfer_buf,
                   transferred);
        result->data_transferred += transferred;

        if (R_FAILED(rc))
        {
            data_transport_failed = true;
            break;
        }
        /* A short DATA IN is valid when a variable-length SCSI response is
         * smaller than its allocation length (REPORT LUNS is a common case).
         * The following status IU decides command success. DATA OUT, and
         * fixed-size block reads in the SCSI bridge, remain exact-length. */
        if (transferred != requested) break;
    }

    if (data_transport_failed) goto end;

    status_size = 0;
    Result status_rc = uaspFinishAsyncTransfer(&status_transfer, &status_size);
    if (R_FAILED(status_rc)) goto end;

    transport_ok = uaspParseCommandStatus(ctx, tag, status_size, result);
    if (data_transport_failed || (!data_in && result->data_transferred != data_size))
        transport_ok = false;

end:
    if (!transport_ok && recover)
        uaspRecoverTransport(ctx, lun, tag, &status_transfer);
    else
        uaspCancelStatusTransfer(ctx, &status_transfer);

    return transport_ok;
}

bool usbHsFsUaspInitialize(UsbHsFsDriveContext *drive_ctx)
{
    if (!drive_ctx || !drive_ctx->uasp ||
        !serviceIsActive(&drive_ctx->usb_in_ep_session[0].s) ||
        !serviceIsActive(&drive_ctx->usb_out_ep_session[0].s) ||
        !serviceIsActive(&drive_ctx->usb_in_ep_session[1].s) ||
        !serviceIsActive(&drive_ctx->usb_out_ep_session[1].s))
        return false;

    mutexLock(&g_uasp_contexts_mutex);
    const bool already_initialized = (uaspFindContext(drive_ctx) != NULL);
    mutexUnlock(&g_uasp_contexts_mutex);
    if (already_initialized) return true;

    UsbHsFsUaspContext *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return false;

    ctx->command_buffer = memalign(USB_XFER_BUF_ALIGNMENT, UASP_IU_BUFFER_SIZE);
    ctx->status_buffer = memalign(USB_XFER_BUF_ALIGNMENT, UASP_IU_BUFFER_SIZE);
    if (!ctx->command_buffer || !ctx->status_buffer)
    {
        if (ctx->command_buffer) free(ctx->command_buffer);
        if (ctx->status_buffer) free(ctx->status_buffer);
        free(ctx);
        return false;
    }

    ctx->drive_ctx = drive_ctx;
    ctx->next_tag = 1;
    ctx->interface_descriptor = drive_ctx->usb_if_session.inf.inf.interface_desc;
    ctx->endpoint_descriptors[UaspEndpointRole_Status] =
        drive_ctx->usb_in_ep_session[0].desc;
    ctx->endpoint_descriptors[UaspEndpointRole_Command] =
        drive_ctx->usb_out_ep_session[0].desc;
    ctx->endpoint_descriptors[UaspEndpointRole_DataIn] =
        drive_ctx->usb_in_ep_session[1].desc;
    ctx->endpoint_descriptors[UaspEndpointRole_DataOut] =
        drive_ctx->usb_out_ep_session[1].desc;
    ctx->advertised_stream_count = uaspDetectAdvertisedStreams(drive_ctx);

    /*
     * Horizon's public usb:hs endpoint API has no way to assign a USB stream
     * ID to an URB. UAS is therefore run in its standards-defined non-stream
     * mode (Read/Write Ready IUs), with one command in flight per drive.
     */
    ctx->streams_enabled = false;

    bool inserted = false;
    bool duplicate = false;
    mutexLock(&g_uasp_contexts_mutex);
    duplicate = (uaspFindContext(drive_ctx) != NULL);
    for (u32 i = 0; !duplicate && i < UASP_MAX_CONTEXTS; i++)
    {
        if (!g_uasp_contexts[i])
        {
            g_uasp_contexts[i] = ctx;
            inserted = true;
            break;
        }
    }
    mutexUnlock(&g_uasp_contexts_mutex);

    if (duplicate)
    {
        free(ctx->command_buffer);
        free(ctx->status_buffer);
        free(ctx);
        return true;
    }

    if (!inserted)
    {
        free(ctx->command_buffer);
        free(ctx->status_buffer);
        free(ctx);
        return false;
    }

    USBHSFS_LOG_MSG("UASP initialized in serialized Ready-IU mode (interface %d, advertised streams %u).",
                    drive_ctx->usb_if_id, ctx->advertised_stream_count);
    return true;
}

void usbHsFsUaspCleanup(UsbHsFsDriveContext *drive_ctx)
{
    if (!drive_ctx) return;

    UsbHsFsUaspContext *ctx = NULL;
    mutexLock(&g_uasp_contexts_mutex);
    for (u32 i = 0; i < UASP_MAX_CONTEXTS; i++)
    {
        if (g_uasp_contexts[i] && g_uasp_contexts[i]->drive_ctx == drive_ctx)
        {
            ctx = g_uasp_contexts[i];
            mutexLock(&ctx->io_mutex);
            g_uasp_contexts[i] = NULL;
            break;
        }
    }
    mutexUnlock(&g_uasp_contexts_mutex);

    if (!ctx) return;
    mutexUnlock(&ctx->io_mutex);
    free(ctx->command_buffer);
    free(ctx->status_buffer);
    free(ctx);
}

bool usbHsFsUaspTransferCommand(UsbHsFsDriveContext *drive_ctx, u32 data_size,
                                bool data_in, u8 lun, const u8 *cdb, u8 cdb_size,
                                void *buf, UsbHsFsUaspCommandResult *out_result)
{
    if (!drive_ctx || !drive_ctx->uasp || !cdb || !cdb_size || cdb_size > 16 ||
        lun >= UMS_MAX_LUN || (data_size && !buf) || !out_result)
        return false;

    UsbHsFsUaspContext *ctx = uaspLockContext(drive_ctx);
    if (!ctx) return false;

    const bool success = uaspTransferCommandLocked(ctx, data_size, data_in, lun,
                                                    cdb, cdb_size, buf, out_result,
                                                    true);
    mutexUnlock(&ctx->io_mutex);
    return success;
}

u8 usbHsFsUaspGetMaxLogicalUnits(UsbHsFsDriveContext *drive_ctx)
{
    UsbHsFsUaspContext *ctx = uaspLockContext(drive_ctx);
    if (!ctx) return 1;

    u8 cdb[12] = {0};
    u8 report[UASP_REPORT_LUNS_SIZE] = {0};
    UsbHsFsUaspCommandResult result = {0};

    cdb[0] = SCSI_OPCODE_REPORT_LUNS;
    uaspPutBe32(&cdb[6], sizeof(report));

    u8 lun_count = 1;
    if (uaspTransferCommandLocked(ctx, sizeof(report), true, 0, cdb, sizeof(cdb),
                                  report, &result, true) &&
        result.scsi_status == SCSI_STATUS_GOOD && result.data_transferred >= 8)
    {
        u32 list_size = uaspGetBe32(report);
        if (list_size > (result.data_transferred - 8))
            list_size = result.data_transferred - 8;

        u8 highest_lun = 0;
        for (u32 offset = 8; offset + 8 <= list_size + 8; offset += 8)
        {
            /* libusbhsfs currently represents only the flat, 0..15 LUN form. */
            if ((report[offset] & 0xC0) == 0 && report[offset] == 0 &&
                report[offset + 1] < UMS_MAX_LUN &&
                report[offset + 1] > highest_lun)
                highest_lun = report[offset + 1];
        }
        lun_count = highest_lun + 1;
    }

    mutexUnlock(&ctx->io_mutex);
    return lun_count;
}

static bool uaspOpenBulkEndpoint(UsbHsFsDriveContext *drive_ctx,
                                 UsbHsClientEpSession *out, bool input)
{
    UsbHsInterfaceInfo *info = &drive_ctx->usb_if_session.inf.inf;
    for (u8 i = 0; i < 15; i++)
    {
        struct usb_endpoint_descriptor *endpoint = input ?
            &info->input_endpoint_descs[i] : &info->output_endpoint_descs[i];
        if (!endpoint->bLength ||
            (endpoint->bmAttributes & USB_TRANSFER_TYPE_MASK) != USB_TRANSFER_TYPE_BULK)
            continue;

        Result rc = usbHsIfOpenUsbEp(&drive_ctx->usb_if_session, out, 1,
                                     endpoint->wMaxPacketSize, endpoint);
        return R_SUCCEEDED(rc);
    }
    return false;
}

bool usbHsFsUaspFallbackToBulkOnlyTransport(UsbHsFsDriveContext *drive_ctx)
{
    if (!drive_ctx || !drive_ctx->uasp) return false;

    u8 *configuration = NULL;
    u32 configuration_size = 0;
    Result rc = usbHsFsRequestGetConfigurationDescriptor(&drive_ctx->usb_if_session,
                                                          0, &configuration,
                                                          &configuration_size);
    if (R_FAILED(rc)) return false;

    struct usb_interface_descriptor selected = {0};
    bool found = false;
    const u8 current_number =
        drive_ctx->usb_if_session.inf.inf.interface_desc.bInterfaceNumber;

    for (u32 offset = 0; offset + 2 <= configuration_size;)
    {
        const u8 length = configuration[offset];
        const u8 type = configuration[offset + 1];
        if (!length || offset + length > configuration_size) break;

        if (type == USB_DT_INTERFACE &&
            length == sizeof(struct usb_interface_descriptor))
        {
            const struct usb_interface_descriptor *candidate =
                (const struct usb_interface_descriptor *)(configuration + offset);
            if (candidate->bInterfaceClass == USB_CLASS_MASS_STORAGE &&
                candidate->bInterfaceSubClass == USB_SUBCLASS_SCSI_TRANSPARENT_CMD_SET &&
                candidate->bInterfaceProtocol == USB_PROTOCOL_BULK_ONLY_TRANSPORT &&
                candidate->bNumEndpoints >= 2 &&
                (!found || candidate->bInterfaceNumber == current_number))
            {
                selected = *candidate;
                found = true;
                if (candidate->bInterfaceNumber == current_number) break;
            }
        }
        offset += length;
    }
    free(configuration);
    if (!found) return false;

    usbHsFsUaspCleanup(drive_ctx);
    for (u32 i = 0; i < 2; i++)
    {
        if (serviceIsActive(&drive_ctx->usb_in_ep_session[i].s))
            usbHsEpClose(&drive_ctx->usb_in_ep_session[i]);
        if (serviceIsActive(&drive_ctx->usb_out_ep_session[i].s))
            usbHsEpClose(&drive_ctx->usb_out_ep_session[i]);
    }

    UsbHsClientIfSession *interface = &drive_ctx->usb_if_session;
    const u8 active_number = interface->inf.inf.interface_desc.bInterfaceNumber;

    if (selected.bInterfaceNumber != active_number)
        rc = usbHsIfSetInterface(interface, NULL, selected.bInterfaceNumber);
    else
        rc = 0;

    if (R_SUCCEEDED(rc) &&
        selected.bAlternateSetting !=
            interface->inf.inf.interface_desc.bAlternateSetting)
        rc = usbHsIfGetAlternateInterface(interface, NULL,
                                           selected.bAlternateSetting);

    if (R_FAILED(rc) ||
        !uaspOpenBulkEndpoint(drive_ctx, &drive_ctx->usb_in_ep_session[0], true) ||
        !uaspOpenBulkEndpoint(drive_ctx, &drive_ctx->usb_out_ep_session[0], false))
    {
        if (serviceIsActive(&drive_ctx->usb_in_ep_session[0].s))
            usbHsEpClose(&drive_ctx->usb_in_ep_session[0]);
        if (serviceIsActive(&drive_ctx->usb_out_ep_session[0].s))
            usbHsEpClose(&drive_ctx->usb_out_ep_session[0]);
        USBHSFS_LOG_MSG("Unable to activate BOT fallback (interface %d).",
                        drive_ctx->usb_if_id);
        return false;
    }

    drive_ctx->uasp = false;
    USBHSFS_LOG_MSG("UASP initialization failed; activated BOT fallback (interface %d).",
                    drive_ctx->usb_if_id);
    return true;
}


