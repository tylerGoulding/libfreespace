/*
 * This file is part of libfreespace.
 *
 * Copyright (c) 2009 Hillcrest Laboratories, Inc.
 *
 * libfreespace is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "freespace_device.h"
#include "freespace_deviceMgr.h"
#include <strsafe.h>
#include <malloc.h>

const int SEND_TIMEOUT = 1000;
const unsigned long HID_NUM_INPUT_BUFFERS = 128;

/**
 * Initialize the send structure.
 *
 * @param send The structure to initialize.
 * @return FREESPACE_SUCCESS if ok.
 */
int initializeSendStruct(struct FreespaceSendStruct* send);

/**
 * Finalize the send structure.
 * For asynchronous transactions, the structure is assumed to be recycled and the
 * associated event is kept alive.
 *
 * @param send The structure to finalize.
 * @param isExit True to close all open handles, false to recycle.
 * @return FREESPACE_SUCCESS
 */
int finalizeSendStruct(struct FreespaceSendStruct* send, BOOL isClose);


int convertGetLastError() {
    int rc = FREESPACE_ERROR_UNEXPECTED;
    int lastError = GetLastError();
    if (lastError == ERROR_DEVICE_NOT_CONNECTED) {
        rc = FREESPACE_ERROR_NOT_FOUND;
    }
    return rc;
}


LIBFREESPACE_API int freespace_getDeviceInfo(FreespaceDeviceId id, struct FreespaceDeviceInfo* info) {
    struct FreespaceDeviceStruct* device = freespace_private_getDeviceById(id);
    if (device == NULL) {
        return FREESPACE_ERROR_NO_DEVICE;
    }
    info->name      = device->name_;
    info->product   = device->handle_[0].info_.idProduct_;
    info->vendor    = device->handle_[0].info_.idVendor_;

    return FREESPACE_SUCCESS;
}

struct FreespaceDeviceStruct* freespace_private_createDevice(const char* name) {
    struct FreespaceDeviceStruct* device = (struct FreespaceDeviceStruct*) malloc(sizeof(struct FreespaceDeviceStruct));
    if (device == NULL) {
        return NULL;
    }
    memset(device, 0, sizeof(struct FreespaceDeviceStruct));

    // Assign the ID.
    device->id_ = freespace_instance_->nextDeviceId_;
    freespace_instance_->nextDeviceId_++;

    // Initialize the rest of the struct.
    device->status_ = FREESPACE_DISCOVERY_STATUS_UNKNOWN;
    device->name_ = name;

    return device;
}

int freespace_private_freeDevice(struct FreespaceDeviceStruct* device) {
    int idx;

    // Try to close the device if it's still open.
    if (device->isOpened_) {
        freespace_closeDevice(device->id_);
    }

    // Free up everything allocated by addNewDevice.
    for (idx = 0; idx < device->handleCount_; idx++) {
        struct FreespaceSubStruct* s = &device->handle_[idx];
        if (s->devicePath != NULL) {
            free(s->devicePath);
        }
    }

    // Free up everything allocated by freespace_private_createDevice
    free(device);
    return FREESPACE_SUCCESS;
}

struct FreespaceSendStruct* getNextSendBuffer(struct FreespaceDeviceStruct* device) {
    int i;
    struct FreespaceSendStruct* s;
    for (i = 0; i < FREESPACE_MAXIMUM_SEND_MESSAGE_COUNT; i++) {
        s = &device->send_[i];
        if (s->interface_ == NULL) {
            return s;
        }
    }
    return NULL;
}

static int initiateAsyncReceives(struct FreespaceDeviceStruct* device) {
    int idx;
    int funcRc = FREESPACE_SUCCESS;
    int rc;

    // If no callback or not opened, then don't need to request to receive anything.
    if (!device->isOpened_ || device->receiveCallback_ == NULL) {
        return FREESPACE_SUCCESS;
    }

    // Initialize a new read operation on all handles that need it.
    for (idx = 0; idx < device->handleCount_; idx++) {
        struct FreespaceSubStruct* s = &device->handle_[idx];
        if (!s->readStatus_) {
            for (;;) {
                BOOL bResult = ReadFile(
					s->handle_,                 /* handle to device */
					s->readBuffer,              /* IN report buffer to fill */
					s->info_.inputReportByteLength_,  /* input buffer size */
					&s->readBufferSize,         /* returned buffer size */
					&s->readOverlapped_ );      /* long pointer to an OVERLAPPED structure */
                if (bResult) {
                    // Got something, so report it.
                    if (device->receiveCallback_) {
                        device->receiveCallback_(device->id_, (char *) (s->readBuffer), s->readBufferSize, device->receiveCookie_, FREESPACE_SUCCESS);
                    } else {
                        // If no receiveCallback, then freespace_setReceiveCallback was called to stop
                        // receives from within the receiveCallback. Bail out to let it do its thing.
                        return FREESPACE_SUCCESS;
                    }
                } else {
                    // Error or would block - check below.
                    break;
                }
            }

            rc = GetLastError();
            if (rc == ERROR_IO_PENDING) {
                // We got a ReadFile to block, so mark it.
                s->readStatus_ = TRUE;
            } else {
                // Something severe happened to our device!  Wait to ensure processing occurs before retrying.
                int rc = convertGetLastError();
                device->receiveCallback_(device->id_, NULL, 0, device->receiveCookie_, rc);
                DEBUG_PRINTF("initiateAsyncReceives : Error on %d : %d\n", idx, GetLastError());
                Sleep(0);
                funcRc = FREESPACE_ERROR_INTERRUPTED;
            }
        }
    }

    return funcRc;
}

int freespace_private_devicePerform(struct FreespaceDeviceStruct* device) {
    int idx;
    BOOL overlappedResult;
    struct FreespaceSendStruct* send;

    // Handle the send messages
    for (idx = 0; idx < FREESPACE_MAXIMUM_SEND_MESSAGE_COUNT; idx++) {
        send = &device->send_[idx];
        if (send->interface_ == NULL) {
            continue;
        }
        overlappedResult = GetOverlappedResult(
                                               send->interface_->handle_,
                                               &send->overlapped_,
                                               &send->numBytes_,
                                               FALSE);

        if (!overlappedResult) {
            // No message available yet.
            continue;
        } else if (send->numBytes_ != send->interface_->info_.outputReportByteLength_) {
            // Unexpected error on the sent message.
            DEBUG_PRINTF("freespace_send_async: error on message size: %d != %d\n",
                         send->numBytes_, send->interface_->info_.outputReportByteLength_);
            if (send->callback_ != NULL) {
                send->callback_(device->id_, send->cookie_, FREESPACE_ERROR_IO);
            }
        } else {
            // successfully sent message
            if (send->callback_ != NULL) {
                send->callback_(device->id_, send->cookie_, FREESPACE_SUCCESS);
            }
        }
        if (finalizeSendStruct(send, FALSE) != FREESPACE_SUCCESS) {
            DEBUG_PRINTF("freespace_private_devicePerform: error while sending message");
        }
    }

    // Call GetOverlappedResult() on everything to check what
    // messages were received.
    for (idx = 0; idx < device->handleCount_; idx++) {
        struct FreespaceSubStruct* s = &device->handle_[idx];

        if (s->readStatus_) {
            BOOL bResult = GetOverlappedResult(
                                               s->handle_,                 /* handle to device */
                                               &s->readOverlapped_,        /* long pointer to an OVERLAPPED structure */
                                               &s->readBufferSize,         /* returned buffer size */
                                               FALSE);
            if (bResult) {
                // Got something, so report it.
                if (device->receiveCallback_) {
                    device->receiveCallback_(device->id_, (char *) (s->readBuffer), s->readBufferSize, device->receiveCookie_, FREESPACE_SUCCESS);
                }
                s->readStatus_ = FALSE;
            } else if (GetLastError() != ERROR_IO_INCOMPLETE) {
                // Something severe happened to our device!  Wait to ensure processing occurs before retrying.
                // @TODO handle this case.
                DEBUG_PRINTF("freespace_private_devicePerform : Error on %d : %d\n", idx, GetLastError());
                device->receiveCallback_(device->id_, NULL, 0, device->receiveCookie_, FREESPACE_ERROR_NO_DATA);
                Sleep(0);
                s->readStatus_ = FALSE;
                // return FREESPACE_ERROR_INTERRUPTED;
            }
        }
    }

    // Re-initiate the ReadFile calls for the next go around.
    return initiateAsyncReceives(device);
}

static int terminateAsyncReceives(struct FreespaceDeviceStruct* device) {
    int idx;

    // Initialize a new read operation on all handles that need it.
    for (idx = 0; idx < device->handleCount_; idx++) {
        struct FreespaceSubStruct* s = &device->handle_[idx];
        if (s->readStatus_) {
            CancelIo(s->handle_);
            s->readStatus_ = FALSE;
        }
    }

    return FREESPACE_SUCCESS;
}

LIBFREESPACE_API int freespace_openDevice(FreespaceDeviceId id) {
    int idx;
    struct FreespaceDeviceStruct* device = freespace_private_getDeviceById(id);
    if (device == NULL) {
        return FREESPACE_ERROR_NO_DEVICE;
    }

    if (device->isOpened_) {
        // Each device can only be opened once.
        return FREESPACE_ERROR_BUSY;
    }


    for (idx = 0; idx < device->handleCount_; idx++) {
        struct FreespaceSubStruct* s = &device->handle_[idx];
        if (s->handle_ != NULL) {
            return FREESPACE_ERROR_BUSY;
        }
        if (s->devicePath == NULL) {
            return FREESPACE_ERROR_NO_DEVICE;
        }
        DEBUG_WPRINTF(L"Open %s\n", s->devicePath);
        s->handle_ = CreateFile(s->devicePath,
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL,
                                OPEN_EXISTING,
                                FILE_FLAG_OVERLAPPED,
                                NULL);

        if (s->handle_ == INVALID_HANDLE_VALUE) {
            return FREESPACE_ERROR_NO_DEVICE;
        }

        if (!HidD_SetNumInputBuffers(s->handle_, HID_NUM_INPUT_BUFFERS)) {
            CloseHandle(s->handle_);
            s->handle_ = NULL;
            return FREESPACE_ERROR_NO_DEVICE;
        }

        // Create the read event.
        s->readOverlapped_.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        if (s->readOverlapped_.hEvent == NULL) {
            return FREESPACE_ERROR_UNEXPECTED;
        }
        s->readOverlapped_.Offset = 0;
        s->readOverlapped_.OffsetHigh = 0;
        s->readStatus_ = FALSE;

        // Register the read event.
        if (freespace_instance_->fdAddedCallback_) {
            freespace_instance_->fdAddedCallback_(s->readOverlapped_.hEvent, 1);
        }
    }

    device->isOpened_ = TRUE;

    // Enable send by initializing all send events.
    for (idx = 0; idx < FREESPACE_MAXIMUM_SEND_MESSAGE_COUNT; idx++) {
        device->send_[idx].overlapped_.hEvent = NULL;
        if (initializeSendStruct(&device->send_[idx]) != FREESPACE_SUCCESS) {
            return FREESPACE_ERROR_UNEXPECTED;
        }
        if (freespace_instance_->fdAddedCallback_) {
            freespace_instance_->fdAddedCallback_(device->send_[idx].overlapped_.hEvent, 1);
        }
    }

    // If async mode has been enabled already, then start the receive
    // process going.
    if (freespace_instance_->fdAddedCallback_) {
        return initiateAsyncReceives(device);
    }

    return FREESPACE_SUCCESS;
}

LIBFREESPACE_API void freespace_closeDevice(FreespaceDeviceId id) {
    int idx;

    struct FreespaceDeviceStruct* device = freespace_private_getDeviceById(id);
    if (device == NULL) {
        return;
    }

    // Don't bother if the device isn't open.
    if (!device->isOpened_) {
        return;
    }

    // Free all send events.
    for (idx = 0; idx < FREESPACE_MAXIMUM_SEND_MESSAGE_COUNT; idx++) {
        if (freespace_instance_->fdRemovedCallback_) {
            freespace_instance_->fdRemovedCallback_(device->send_[idx].overlapped_.hEvent);
        }
        finalizeSendStruct(&device->send_[idx], TRUE);
    }

    // Free all read events
    for (idx = 0; idx < device->handleCount_; idx++) {
        struct FreespaceSubStruct* s = &device->handle_[idx];
        if (s->handle_ != NULL) {
            CloseHandle(s->handle_);
            s->handle_ = NULL;
        }
        if (freespace_instance_->fdRemovedCallback_) {
            freespace_instance_->fdRemovedCallback_(s->readOverlapped_.hEvent);
        }

        if (s->readOverlapped_.hEvent != NULL) {
            s->readStatus_ = FALSE;
            CloseHandle(s->readOverlapped_.hEvent);
            s->readOverlapped_.hEvent = NULL;
        }
    }
    device->isOpened_ = FALSE;
}

int prepareSend(FreespaceDeviceId id, struct FreespaceSendStruct** sendOut, const char* report, int length) {
    int idx;
    int retVal;
    struct FreespaceSubStruct* s;
    struct FreespaceDeviceStruct* device;
    struct FreespaceSendStruct* send;

    // Get the device
    device = freespace_private_getDeviceById(id);
    if (device == NULL) {
        return FREESPACE_ERROR_NO_DEVICE;
    }

    if (!device->isOpened_) {
        // The device must be opened!
        return FREESPACE_ERROR_IO;
    }

    // Get the send buffer and initialize
    send = getNextSendBuffer(device);
    *sendOut = send;
    if (send == NULL) {
        return FREESPACE_ERROR_BUSY;
    }
    retVal = initializeSendStruct(send);
    if (retVal != FREESPACE_SUCCESS) {
        return retVal;
    }

    // Find the desired output port.
    s = &device->handle_[0];
    for (idx = 0; idx < device->handleCount_; idx++) {
        if (device->handle_[idx].info_.outputReportByteLength_ > s->info_.outputReportByteLength_) {
            s = &device->handle_[idx];
        }
    }
    send->interface_ = s;

    if (length > s->info_.outputReportByteLength_) {
        send->rc_ = FREESPACE_ERROR_SEND_TOO_LARGE;
        return send->rc_;
    }
    if (s->info_.outputReportByteLength_ > FREESPACE_MAX_OUTPUT_MESSAGE_SIZE) {
        send->rc_ = FREESPACE_ERROR_UNEXPECTED;
        return send->rc_;
    }

    // Copy over the report to the local buffer.
    for (idx = 0; idx < length; idx++) {
        send->report_[idx] = report[idx];
    }

    // Stuff with trailing zeros as needed.
    for (idx = length; idx < s->info_.outputReportByteLength_; idx++) {
        send->report_[idx] = 0;
    }

    send->rc_ = FREESPACE_SUCCESS;
    return send->rc_;
}

int initializeSendStruct(struct FreespaceSendStruct* send) {
    if (send == NULL) {
        return FREESPACE_ERROR_BUSY;
    }
    send->interface_ = NULL;
    send->error_     = FREESPACE_SUCCESS;

    // initialize the report
    send->overlapped_.Offset = 0;
    send->overlapped_.OffsetHigh = 0;

    if (send->overlapped_.hEvent != NULL) {
        return FREESPACE_SUCCESS;
    }

    // create an overlapped report event.
    send->overlapped_.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (send->overlapped_.hEvent == NULL) {
        return FREESPACE_ERROR_UNEXPECTED;
    }
    return FREESPACE_SUCCESS;
}

int finalizeSendStruct(struct FreespaceSendStruct* send, BOOL doClose) {
    send->interface_ = NULL;
    if (doClose) {
        // Close the overlapped report event.
        CloseHandle(send->overlapped_.hEvent);
        send->overlapped_.hEvent = NULL;
    }
    return send->rc_;
}

int freespace_send_activate(struct FreespaceSendStruct* send) {
    int retVal = 0;
    DWORD lastError = 0;

    // Write to the file to send out the message
    retVal = WriteFile(
                       send->interface_->handle_,
                       send->report_,
                       send->interface_->info_.outputReportByteLength_,
                       &send->numBytes_,
                       &send->overlapped_);

    if (retVal) {
        // Completed as synchronous I/O
        DEBUG_PRINTF("freespace_send: completed synchronously\n");
        send->rc_ = FREESPACE_SUCCESS;
        return finalizeSendStruct(send, FALSE);
    }

    // check for errors
    lastError = GetLastError();
    if (lastError != ERROR_IO_PENDING) {
        // Abort any pending messages and return
        // WARNING: CancelIo will also affect READ!
        DEBUG_PRINTF("freespace_send: GetLastError = %d\n", lastError);
        CancelIo(send->interface_->handle_);
        send->interface_->readStatus_ = FALSE;
        send->rc_ = FREESPACE_ERROR_UNEXPECTED; //FREESPACE_OS_ERROR_BASE - lastError;
        return finalizeSendStruct(send, FALSE);
    }

    return FREESPACE_ERROR_IO;
}

LIBFREESPACE_API int freespace_send(FreespaceDeviceId id,
                                    const char* report,
                                    int length) {

    struct FreespaceSendStruct* send;
    int retVal = 0;
    DWORD lastError = 0;

    retVal = prepareSend(id, &send, report, length);
    if (retVal != FREESPACE_SUCCESS) {
        return retVal;
    }

    // Send the message
    retVal = freespace_send_activate(send);
    if (retVal != FREESPACE_ERROR_IO) {
        return retVal;
    }

    // Wait for the message to be sent
    lastError = WaitForSingleObject(send->overlapped_.hEvent, SEND_TIMEOUT);

    if (lastError != WAIT_OBJECT_0) {
        // timed out
        BOOL overlappedResult = GetOverlappedResult(
                                                    send->interface_->handle_,
                                                    &send->overlapped_,
                                                    &send->numBytes_,
                                                    FALSE);

        // Abort any pending messages and return
        // WARNING: CancelIo will also affect READ!
        DEBUG_PRINTF("freespace_send: error on WaitForSingleObject = %d\n", lastError);
        CancelIo(send->interface_->handle_);
        send->interface_->readStatus_ = FALSE;

        if (overlappedResult) {
            send->rc_ = FREESPACE_ERROR_TIMEOUT;
        } else {
            send->rc_ = FREESPACE_ERROR_IO; //FREESPACE_OS_ERROR_BASE - lastError;
        }
    } else {
        // success
        BOOL overlappedResult = GetOverlappedResult(
                                                    send->interface_->handle_,
                                                    &send->overlapped_,
                                                    &send->numBytes_,
                                                    TRUE);

        if (!overlappedResult) {
            DEBUG_PRINTF("freespace_send: error on GetOverlappedResult\n");
            send->rc_ = FREESPACE_ERROR_IO;
        } else if (send->numBytes_ != send->interface_->info_.outputReportByteLength_) {
            DEBUG_PRINTF("freespace_send: error on message size: %d != %d\n",
                         send->numBytes_, send->interface_->info_.outputReportByteLength_);
            send->rc_ = FREESPACE_ERROR_IO;
        } else {
            // successfully sent message
            send->rc_ = FREESPACE_SUCCESS;
        }
    }

    return finalizeSendStruct(send, FALSE);
}

LIBFREESPACE_API int freespace_sendAsync(FreespaceDeviceId id,
                                         const char* report,
                                         int length,
                                         unsigned int timeoutMs,
                                         freespace_sendCallback callback,
                                         void* cookie) {
    struct FreespaceSendStruct* send;
    int retVal = 0;
    DWORD lastError = 0;

    retVal = prepareSend(id, &send, report, length);
    if (retVal != FREESPACE_SUCCESS) {
        return retVal;
    }
    send->callback_ = callback;
    send->cookie_ = cookie;
    send->timeoutMs_ = timeoutMs;

    // Send the message
    retVal = freespace_send_activate(send);
    if (retVal != FREESPACE_ERROR_IO) {
        return retVal;
    }
    return FREESPACE_SUCCESS;
}

LIBFREESPACE_API int freespace_read(FreespaceDeviceId id,
                                    char* message,
                                    int maxLength,
                                    unsigned int timeoutMs,
                                    int* actualLength) {
    HANDLE waitEvents[FREESPACE_HANDLE_COUNT_MAX];
    int idx;
    DWORD bResult;

    struct FreespaceDeviceStruct* device = freespace_private_getDeviceById(id);
    if (device == NULL) {
        return FREESPACE_ERROR_NO_DEVICE;
    }

    // Start the reads going.
    for (idx = 0; idx < device->handleCount_; idx++) {
        BOOL bResult;
        struct FreespaceSubStruct* s = &device->handle_[idx];
        waitEvents[idx] = s->readOverlapped_.hEvent;

        // Initiate a ReadFile on anything that doesn't already have
        // a ReadFile op pending.
        if (!s->readStatus_) {
            bResult = ReadFile(
                               s->handle_,                 /* handle to device */
                               s->readBuffer,              /* IN report buffer to fill */
                               s->info_.inputReportByteLength_,  /* input buffer size */
                               &s->readBufferSize,         /* returned buffer size */
                               &s->readOverlapped_ );      /* long pointer to an OVERLAPPED structure */
            if (bResult) {
                // Got something immediately, so return it.
                *actualLength = min(s->readBufferSize, (unsigned long) maxLength);
                memcpy(message, s->readBuffer, *actualLength);
                return FREESPACE_SUCCESS;
            } if (GetLastError() != ERROR_IO_PENDING) {
                // Something severe happened to our device!  Wait to ensure processing occurs before retrying.
                int rc = convertGetLastError();
                DEBUG_PRINTF("freespace_read 1: Error on %d : %d\n", idx, GetLastError());
                Sleep(0);
                return FREESPACE_ERROR_INTERRUPTED;
            }
            s->readStatus_ = TRUE;
        }
    }

    // Wait.
    bResult = WaitForMultipleObjects(device->handleCount_, waitEvents, FALSE, timeoutMs);
    if (bResult == WAIT_FAILED) {
        DEBUG_PRINTF("Error from WaitForMultipleObjects\n");
        return FREESPACE_ERROR_INTERRUPTED;
    } else if (bResult == WAIT_TIMEOUT) {
        return FREESPACE_ERROR_TIMEOUT;
    }

    // Check which read worked.
    for (idx = 0; idx < device->handleCount_; idx++) {
        struct FreespaceSubStruct* s = &device->handle_[idx];
        BOOL bResult = GetOverlappedResult(
                                           s->handle_,                 /* handle to device */
                                           &s->readOverlapped_,        /* long pointer to an OVERLAPPED structure */
                                           &s->readBufferSize,         /* returned buffer size */
                                           FALSE);
        if (bResult) {
            // Got something, so report it.
            *actualLength = min(s->readBufferSize, (unsigned long) maxLength);
            memcpy(message, s->readBuffer, *actualLength);
            s->readStatus_ = FALSE;
            return FREESPACE_SUCCESS;
        } else if (GetLastError() != ERROR_IO_INCOMPLETE) {
            // Something severe happened to our device!  Wait to ensure processing occurs before retrying.
            int rc = convertGetLastError();
            DEBUG_PRINTF("freespace_read 2 : Error on %d : %d\n", idx, GetLastError());
            Sleep(0);
            s->readStatus_ = FALSE;
            return FREESPACE_ERROR_INTERRUPTED;
        }
    }

    return FREESPACE_ERROR_INTERRUPTED;
}


LIBFREESPACE_API int freespace_flush(FreespaceDeviceId id) {
    int idx;

    struct FreespaceDeviceStruct* device = freespace_private_getDeviceById(id);
    if (device == NULL) {
        return FREESPACE_ERROR_NO_DEVICE;
    }

    for (idx = 0; idx < device->handleCount_; idx++) {
        struct FreespaceSubStruct* s = &device->handle_[idx];
        CancelIo(s->handle_);
        s->readStatus_ = FALSE;
    }
    return FREESPACE_SUCCESS;
}

BOOL freespace_private_fdSyncAddFilter(struct FreespaceDeviceStruct* device) {
    if (device->receiveCallback_ && device->isOpened_) {
        int idx;
        for (idx = 0; idx < device->handleCount_; idx++) {
            freespace_instance_->fdAddedCallback_(device->handle_[idx].readOverlapped_.hEvent, 1);
        }
    }
    return FALSE;
}

static BOOL fdSyncRemoveFilter(struct FreespaceDeviceStruct* device) {
    int idx;
    for (idx = 0; idx < device->handleCount_; idx++) {
        freespace_instance_->fdRemovedCallback_(device->handle_[idx].readOverlapped_.hEvent);
    }
    return FALSE;
}

LIBFREESPACE_API int freespace_setReceiveCallback(FreespaceDeviceId id,
                                                  freespace_receiveCallback callback,
                                                  void* cookie) {
    struct FreespaceDeviceStruct* device = freespace_private_getDeviceById(id);
    if (device == NULL) {
        return FREESPACE_ERROR_NO_DEVICE;
    }

    if (device->isOpened_) {
        if (device->receiveCallback_ != NULL && callback == NULL) {
            // Deregistering callback, so stop any pending receives.
            device->receiveCallback_ = NULL;
            device->receiveCookie_ = NULL;

            fdSyncRemoveFilter(device);

            return terminateAsyncReceives(device);
        } else if (device->receiveCallback_ == NULL && callback != NULL) {
            // Registering a callback, so initiate a receive
            device->receiveCookie_ = cookie;
            device->receiveCallback_ = callback;

            freespace_private_fdSyncAddFilter(device);

            return initiateAsyncReceives(device);
        }
    }
    // Just update the cookie and callback.
    device->receiveCookie_ = cookie;
    device->receiveCallback_ = callback;

    return FREESPACE_SUCCESS;
}