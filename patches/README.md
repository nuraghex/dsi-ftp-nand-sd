# Patches to DSWiFi

DSi FTP is built against the DSWiFi of BlocksDS 1.25.0 with one patch applied.
`tools/build_dswifi.sh` (called by `build.sh`) downloads the BlocksDS source,
applies it and builds the library and the ARM7 cores, without touching the
installed BlocksDS.

## dswifi-1.25.0-dsiftp.patch (used)

- TCP receive window from 4 to 16 segments (5840 to 23360 bytes), with the
  ARM7 to ARM9 receive buffer from 12 to 64 KB and larger lwIP mailboxes to
  hold it. Uploads to the DSi go from about 460 to about 700-800 KB/s.
- 32 lwIP mailboxes instead of 10, and an off-by-one in their index checks.
- A data packet with nothing but a trailer is no longer put in the receive
  queue, where a packet of size 0 reads as the end marker of the queue.

## dswifi-1.25.0-flow-control.patch (not used)

The patch above plus HTC credit flow control in the DSi driver: packets wait
in the TX queue until the Wi-Fi chip reports free buffers (it has 10, of 1544
bytes). Written to find out why the console freezes when it sends faster than
one lwIP send buffer per frame; it did not prevent that freeze, so it is kept
only as a starting point for whoever looks into it next.
