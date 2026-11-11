# DSi FTP - FTP server for the Nintendo DSi (SD read/write, NAND read-only).
# Built on top of the default BlocksDS ARM9 ROM makefile.

NAME		:= dsiftp
GAME_TITLE	:= DSi FTP
GAME_SUBTITLE	:= NAND + SD via Wi-Fi
GAME_AUTHOR	:= nuraghex
GAME_ICON	:= icon.png

SOURCEDIRS	:= source

# ARM7 core with DSWiFi (needed for Wi-Fi, including DSi mode / WPA2).
# WIFI_DEBUG=1 uses the debug core: with the log setting on, the messages of
# the Wi-Fi chip driver (connect/disconnect events, WPA2 handshake) are
# written to log.txt too. Releases: make WIFI_DEBUG=0.
WIFI_DEBUG	?= 1
ifeq ($(WIFI_DEBUG),1)
ARM7ELF		:= $(BLOCKSDS)/sys/arm7/main_core/arm7_dswifi_debug.elf
else
ARM7ELF		:= $(BLOCKSDS)/sys/arm7/main_core/arm7_dswifi.elf
endif

LIBS		:= -ldswifi9 -lnds9
LIBDIRS		:= $(BLOCKSDS)/libs/dswifi


# DSi-only title with its own title ID (00030004/HFTA), so it can also be
# installed to the DSi Menu with NTM later.
NDSTOOL_ARGS	:= -g HFTA 00 "DSIFTP" -u 00030004 -uc 3

include $(BLOCKSDS)/sys/default_makefiles/rom_arm9/Makefile
