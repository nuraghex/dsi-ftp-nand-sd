// Keeps the Wi-Fi from transmitting while the SD card or the NAND is busy.
//
// On the DSi the ARM7 runs both the SD/NAND driver and the Wi-Fi driver. When
// lwIP hands it a packet to send while it is in the middle of a storage
// operation, the ARM7 can lock up for good: the console freezes in the middle
// of a download (seen with BlocksDS 1.25 / DSWiFi 0.4.2).
//
// Both kinds of request start on the ARM9, so they are kept apart here with
// the linker's --wrap option (see the Makefile):
//  - libnds takes the FIFO_STORAGE mutex around every storage request;
//  - DSWiFi calls Wifi_TransmitFunctionLink() for every outgoing frame.
// A frame that would go out during a storage request is refused, which lwIP
// treats as "no memory right now": TCP keeps the data and sends it later.

#include <stdbool.h>
#include <stddef.h>

#include <nds.h>
#include <nds/fifocommon.h>

static volatile bool storage_busy;

void __real_fifoMutexAcquire(u32 channel);
void __real_fifoMutexRelease(u32 channel);
int __real_Wifi_TransmitFunctionLink(const void *src, size_t size);

void __wrap_fifoMutexAcquire(u32 channel)
{
    __real_fifoMutexAcquire(channel);
    if (channel == FIFO_STORAGE)
        storage_busy = true;
}

void __wrap_fifoMutexRelease(u32 channel)
{
    if (channel == FIFO_STORAGE)
        storage_busy = false;
    __real_fifoMutexRelease(channel);
}

int __wrap_Wifi_TransmitFunctionLink(const void *src, size_t size)
{
    if (storage_busy)
        return -1;
    return __real_Wifi_TransmitFunctionLink(src, size);
}
