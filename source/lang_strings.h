// Every text shown on the DSi screens: X(id, width, console colour, English).
//
// width > 0: columns available on screen for the text (printf specifiers
//            excluded). Longer translations are clipped with "~".
// width < 0: wrapped text (dialogs, console): at most -width lines of 31
//            columns.
// Colour: used when the text is a console message (ui_msg()).
//
// Translations live in lang_xx.c. tools/check_lang.py checks that they keep
// the same printf specifiers, fit their width and have glyphs in the font.
// The FTP protocol replies and log.txt stay in English on purpose.

// clang-format off
#define LANG_STRINGS(X) \
    /* Common */ \
    X(S_YES,                 18, LC_N,  "yes") \
    X(S_BACK,                 8, LC_N,  "Back") \
    X(S_CHANGE,               9, LC_N,  "Change") \
    X(S_EDIT,                 9, LC_N,  "Edit") \
    X(S_OPEN,                 9, LC_N,  "Open") \
    X(S_TURN_ON,              9, LC_N,  "Turn on") \
    X(S_TURN_OFF,             9, LC_N,  "Turn off") \
    X(S_ALLOW,                9, LC_N,  "Allow") \
    X(S_DENY,                 9, LC_N,  "Deny") \
    X(S_NEW,                  9, LC_N,  "New") \
    X(S_USE,                  9, LC_N,  "Use") \
    X(S_ANY,                 18, LC_N,  "any") \
    X(S_NONE,                18, LC_N,  "none") \
    X(S_B_CANCEL,            31, LC_N,  "B: cancel") \
    X(S_FOOT_MENU,           31, LC_N,  "Up/Down, A or touch   B: back") \
    X(S_FOOT_TABS,           31, LC_N,  "Up/Down, A or touch   L/R: tabs") \
    X(S_FOOT_CONSOLE,        31, LC_N,  "L/R or touch the tabs") \
    X(S_SAVE_FAILED,         30, LC_ER, "Saving failed") \
    /* Top screen */ \
    X(S_BATT_LABEL,           6, LC_N,  "Batt.") \
    X(S_BATT_CHARGING,       18, LC_N,  "charging") \
    X(S_BATT_LOW,            18, LC_N,  "low") \
    X(S_BATT_OK,             18, LC_N,  "ok") \
    X(S_HDR_BRICK,           16, LC_N,  "!! BRICK RISK !!") \
    X(S_HDR_NAND_WRITE,      16, LC_N,  "NAND WRITE") \
    X(S_SD_NOT_FOUND,        22, LC_N,  "not found") \
    X(S_HIDDEN_PERMS,        22, LC_N,  "hidden (permissions)") \
    X(S_READ_WRITE,          22, LC_N,  "read/write") \
    X(S_READ_ONLY,           22, LC_N,  "read only") \
    X(S_NAND_NOT_AVAIL,      22, LC_N,  "not available") \
    X(S_NAND_FULL_ON,        22, LC_N,  "FULL WRITE!") \
    X(S_NAND_WRITE_ON,       22, LC_N,  "WRITE ENABLED") \
    X(S_EXPLORER,            28, LC_N,  "File Explorer:  ftp://") \
    X(S_USER_LABEL,           7, LC_N,  "user:") \
    X(S_CUSTOM_PASSWORD,     29, LC_N,  "custom password") \
    X(S_USER_FREE,           16, LC_N,  "user: any") \
    X(S_PASSWORD_PIN,        16, LC_N,  "password = PIN") \
    X(S_FOOT_FULL,           31, LC_N,  "EXPERIMENTAL: no protection!") \
    X(S_FOOT_MAIN,           31, LC_N,  "START quit SEL perms Y networks") \
    X(S_FOOT_START,          31, LC_N,  "START: quit") \
    /* Tabs */ \
    X(S_TAB_CONSOLE,          8, LC_N,  "Console") \
    X(S_TAB_SERVER,           8, LC_N,  "Server") \
    X(S_TAB_NET,              8, LC_N,  "Network") \
    X(S_TAB_ACCESS,           8, LC_N,  "Access") \
    /* On-screen keyboard */ \
    X(S_MIN_CHARS,           31, LC_N,  "(at least %u characters)") \
    X(S_KBD_HELP,            -4, LC_N,  "Touch the keys on the lower screen. Enter: confirm\nB: cancel") \
    /* Wi-Fi state */ \
    X(S_ST_SCANNING,         17, LC_N,  "searching...") \
    X(S_ST_CHOOSE,           17, LC_N,  "choose a network") \
    X(S_ST_CONNECTED,        17, LC_N,  "connected") \
    X(S_FOOT_SCANNING,       31, LC_N,  "Searching for networks...") \
    X(S_FOOT_CONNECTING,     31, LC_N,  "Connecting...   B: cancel") \
    X(S_SEC_OPEN,             6, LC_N,  "open") \
    X(S_AS_DISCONNECTED,     20, LC_N,  "Disconnected") \
    X(S_AS_SEARCHING,        20, LC_N,  "Searching") \
    X(S_AS_AUTHENTICATING,   20, LC_N,  "Authenticating") \
    X(S_AS_ASSOCIATING,      20, LC_N,  "Associating") \
    X(S_AS_ACQUIRINGDHCP,    20, LC_N,  "Getting address") \
    X(S_AS_ASSOCIATED,       20, LC_N,  "Connected") \
    X(S_AS_CANNOTCONNECT,    20, LC_N,  "Cannot connect") \
    /* Wi-Fi console messages */ \
    X(S_LOG_NO_NETWORKS,     -3, LC_WA, "No networks found: if it persists, turn the DSi off and on") \
    X(S_LOG_CONNECTING,      -2, LC_N,  "Connecting to %s") \
    X(S_LOG_ATTEMPT,         -2, LC_WA, "Attempt %d of %d") \
    X(S_LOG_STATE,           -2, LC_N,  " %s (%d.%d s)") \
    X(S_LOG_DHCP_OK,         -2, LC_OK, " DHCP: %s (%d.%d s)") \
    X(S_LOG_IPV6_WAIT,       -2, LC_N,  " IPv6 ready, waiting for IPv4...") \
    X(S_LOG_STUCK,           -2, LC_ER, "Connection stuck") \
    X(S_LOG_NO_DHCP,         -2, LC_ER, "The router gives no address") \
    X(S_LOG_REFUSED,         -2, LC_ER, "Connection refused") \
    X(S_LOG_STUCK_FINAL,     -3, LC_ER, "Wi-Fi stuck: turn the DSi off and on (POWER button)") \
    X(S_LOG_NO_DHCP_FINAL,   -3, LC_ER, "No DHCP answer: set a static IP in _nds/dsiftp/networks.ini") \
    X(S_LOG_WPA1,            -2, LC_ER, "WPA1 is not supported: use WPA2") \
    X(S_LOG_WPA3,            -3, LC_ER, "WPA3-only network: the DSi stops at WPA2. Use WPA2/WPA3 mixed mode.") \
    X(S_LOG_REENTER_PASS,    -2, LC_WA, "Enter the password again") \
    X(S_NET_SAVED,           -2, LC_OK, "Network saved") \
    /* Network list */ \
    X(S_NETLIST_TITLE,       15, LC_N,  "Wi-Fi networks") \
    X(S_NETLIST_FOUND,       14, LC_N,  "%d found") \
    X(S_NETLIST_NONE,        30, LC_N,  "No networks found.") \
    X(S_NETLIST_24GHZ,       30, LC_N,  "The DSi only sees 2.4 GHz.") \
    X(S_NETLIST_LEGEND,      31, LC_N,  "* default   + saved") \
    X(S_BTN_CONNECT,          7, LC_N,  "Connect") \
    X(S_BTN_DEFAULT,          7, LC_N,  "Default") \
    X(S_BTN_FORGET,           7, LC_N,  "Forget") \
    X(S_BTN_SCAN,             7, LC_N,  "Scan") \
    X(S_NET_SAVE_FIRST,      31, LC_N,  "Connect and save it first") \
    X(S_NET_DEFAULT_SET,     31, LC_OK, "Default network set") \
    X(S_NET_FORGOTTEN,       31, LC_OK, "Network removed") \
    X(S_WIFI_PASS_TITLE,     30, LC_N,  "Wi-Fi password") \
    X(S_WIFI_PASS_LABEL,     30, LC_N,  "Network password:") \
    X(S_SAVE_CONNECTED_TO,   30, LC_N,  "Connected to:") \
    X(S_SAVE_THIS,           26, LC_N,  "Save this network") \
    X(S_SAVE_AS_DEFAULT,     26, LC_N,  "Use it as default") \
    X(S_CONTINUE,            14, LC_N,  "Continue") \
    X(S_SAVE_PLAIN_NOTE,     -2, LC_N,  "The password is saved as plain text in _nds/dsiftp (SD).") \
    X(S_FOOT_SAVE,           31, LC_N,  "Up/Down: choose  A or touch: ok") \
    /* Start-up errors */ \
    X(S_DSI_MODE_TITLE,      30, LC_N,  "DSi mode required") \
    X(S_DSI_MODE_TEXT,      -12, LC_N,  "DSi FTP only works in DSi mode.\n\nStart it from Unlaunch, TWiLight Menu++ or the DSi Menu.\n\nSTART: quit") \
    X(S_WIFI_ERR_TITLE,      30, LC_N,  "Wi-Fi error") \
    X(S_WIFI_ERR_TEXT,      -12, LC_N,  "Wi-Fi could not be started.\n\nSTART: quit") \
    X(S_FTP_ERR_TITLE,       30, LC_N,  "FTP server error") \
    X(S_PORT21_TEXT,        -12, LC_N,  "Port 21 is not available.\n\nSTART: quit") \
    /* NAND write confirmations */ \
    X(S_NAND_W_UNAVAIL,      30, LC_N,  "NAND writing unavailable") \
    X(S_NAND_W_NEED_SD,     -12, LC_N,  "A working SD card is needed: every NAND file is copied to the SD before it is changed.\n\nA/B: back") \
    X(S_NAND_W_LOW_BATT,    -12, LC_N,  "Battery too low.\n\nConnect the charger: if the DSi switched off in the middle of a write, the NAND could be damaged.\n\nA/B: back") \
    X(S_CONFIRM_W_TITLE,     30, LC_N,  "ENABLE NAND WRITING?") \
    X(S_CONFIRM_W_TEXT,     -14, LC_N,  "The PC will be able to change files in the internal memory.\n\nAlways protected: launcher, Unlaunch, sys/, shared1/, system titles, DSi FTP itself, top-level folders.\n\nEvery file is backed up to _nds/dsiftp/nand_backup first.\n\nL+R+A: confirm   B: cancel") \
    X(S_CONFIRM_FULL_TITLE,  30, LC_N,  "!! FULL NAND WRITE !!") \
    X(S_CONFIRM_FULL_TEXT,  -14, LC_N,  "EXPERIMENTAL, HIGH RISK. Removes ALL protections: launcher, Unlaunch, sys/, shared1/, system titles, top-level folders.\n\nA mistake can make the DSi unusable (brick) with no software recovery. The backups on the SD remain.\n\nL+R+X: continue   B: cancel") \
    X(S_COUNTDOWN,           31, LC_N,  "Enabling in %d seconds...") \
    /* Permissions menu */ \
    X(S_PERMS_TITLE,         20, LC_N,  "Permissions") \
    X(S_PERMS_TITLE_FULL,    20, LC_N,  "Permissions: FULL!") \
    X(S_P_SD_READ,           18, LC_N,  "SD: read") \
    X(S_P_SD_WRITE,          18, LC_N,  "SD: write") \
    X(S_P_NAND_READ,         18, LC_N,  "NAND: read") \
    X(S_P_NAND_WRITE,        18, LC_N,  "NAND: write") \
    X(S_P_NAND_FULL,         18, LC_N,  "NAND: FULL write") \
    X(S_V_NO_HIDDEN,         18, LC_N,  "no (hidden)") \
    X(S_V_NO_READ_ONLY,      18, LC_N,  "no (read only)") \
    X(S_V_YES_INACTIVE,      18, LC_N,  "yes (no read: off)") \
    X(S_V_NAND_UNAVAIL,      18, LC_N,  "NAND not available") \
    X(S_V_W_OFF,             18, LC_N,  "off") \
    X(S_V_W_ON,              18, LC_N,  "ON (protected)") \
    X(S_V_W_INACTIVE,        18, LC_N,  "on (no read: off)") \
    X(S_V_FULL_OFF,          18, LC_N,  "experimental, off") \
    X(S_V_FULL_ON,           18, LC_N,  "ON - BRICK RISK") \
    X(S_V_FULL_INACTIVE,     18, LC_N,  "on (needs write)") \
    /* FTP access menu */ \
    X(S_FTP_ACCESS,          20, LC_N,  "FTP access") \
    X(S_PASS_SET,            18, LC_N,  "set (%u chars)") \
    X(S_PASS_NOT_SET,        18, LC_N,  "not set") \
    X(S_ACCESS_MODE,         18, LC_N,  "Login") \
    X(S_V_USER_PASS,         18, LC_N,  "user and password") \
    X(S_V_RANDOM_PIN,        18, LC_N,  "random PIN") \
    X(S_PIN_LABEL,           20, LC_N,  "PIN (new at start)") \
    X(S_V_NOT_USED,          18, LC_N,  "not used") \
    X(S_USERNAME,            18, LC_N,  "User name") \
    X(S_PASSWORD,            18, LC_N,  "Password") \
    X(S_PORT,                18, LC_N,  "Port") \
    X(S_FOOT_PLAIN_PASS,     31, LC_N,  "Password stored as plain text") \
    X(S_FTP_PASS_TITLE,      30, LC_N,  "FTP password") \
    X(S_FTP_PASS_LABEL,      30, LC_N,  "New FTP password:") \
    X(S_SAVED_PLAIN,         30, LC_N,  "(stored as plain text on SD)") \
    X(S_FTP_USER_TITLE,      30, LC_N,  "FTP user name") \
    X(S_FTP_USER_LABEL,      30, LC_N,  "User name (empty = any):") \
    X(S_FTP_PORT_TITLE,      30, LC_N,  "FTP port") \
    X(S_FTP_PORT_LABEL,      30, LC_N,  "Port (21 = standard):") \
    X(S_FTP_PORT_NOTE,       30, LC_N,  "not 50000-50999 (data)") \
    X(S_LOG_LOGIN_USER,      -2, LC_N,  "Login with user and password") \
    X(S_LOG_LOGIN_PIN,       -2, LC_N,  "Login with PIN") \
    X(S_LOG_NEW_PIN,         -2, LC_OK, "New PIN generated") \
    X(S_LOG_PORT_INVALID,    -2, LC_ER, "Invalid port") \
    X(S_LOG_PORT_SET,        -2, LC_OK, "Server on port %d") \
    X(S_LOG_PORT_BUSY,       -2, LC_ER, "Port %d not available") \
    X(S_LOG_PORT_FALLBACK,   -2, LC_ER, "Port %d not available: using 21") \
    /* IP address menu */ \
    X(S_IP_ADDRESS,          20, LC_N,  "IP address") \
    X(S_IN_USE,              18, LC_N,  "in use") \
    X(S_IN_USE_FMT,          18, LC_N,  "%s (in use)") \
    X(S_FROM_ROUTER,         18, LC_N,  "from the router") \
    X(S_IP_AUTO,             20, LC_N,  "Automatic (DHCP)") \
    X(S_IP_FIX_CURRENT,      20, LC_N,  "Keep current address") \
    X(S_FIX,                  9, LC_N,  "Keep") \
    X(S_IP_TYPE,             20, LC_N,  "Type an address") \
    X(S_TYPE_BTN,             9, LC_N,  "Type") \
    X(S_IP_EXAMPLE,          18, LC_N,  "e.g. 192.168.1.50") \
    X(S_FOOT_RECONNECT_AFTER, 31, LC_N, "Reconnects after a change") \
    X(S_IP_INPUT_LABEL,      30, LC_N,  "DSi IP address:") \
    X(S_IP_INPUT_NOTE,       30, LC_N,  "same network as the router") \
    X(S_LOG_SAVE_NET_FIRST,  -2, LC_WA, "Network not saved: save it first") \
    X(S_LOG_IP_INVALID,      -3, LC_ER, "Address not valid for this network (router %s)") \
    X(S_LOG_IP_FIXED,        -2, LC_N,  "Static IP: reconnecting...") \
    X(S_LOG_IP_DHCP,         -2, LC_N,  "DHCP: reconnecting...") \
    /* Server tab */ \
    X(S_PERMS_SUMMARY,       18, LC_N,  "SD %s  NAND %s") \
    X(S_SHORT_HIDDEN,         6, LC_N,  "hidden") \
    X(S_SHORT_R,              4, LC_N,  "R") \
    X(S_SHORT_RW,             4, LC_N,  "R/W") \
    X(S_SHORT_RW_FULL,        8, LC_N,  "R/W FULL") \
    X(S_LOGIN_USER_FMT,      18, LC_N,  "user %s, port %d") \
    X(S_LOGIN_PIN_FMT,       18, LC_N,  "PIN %s, port %d") \
    X(S_ROW_PERMS,           20, LC_N,  "Permissions (SEL)") \
    X(S_ROW_LOG,             20, LC_N,  "Diagnostic log") \
    X(S_V_LOG_ON,            18, LC_N,  "on: log.txt") \
    X(S_V_LOG_OFF,           18, LC_N,  "off") \
    X(S_ROW_LANG,            20, LC_N,  "Language") \
    X(S_ROW_EXIT,            20, LC_N,  "Quit (START)") \
    X(S_V_EXIT,              18, LC_N,  "stops the server") \
    X(S_EXIT_BTN,             9, LC_N,  "Quit") \
    X(S_LOG_LOG_ON,          -2, LC_OK, "Diagnostic log on") \
    X(S_LOG_LOG_OFF,         -2, LC_N,  "Diagnostic log off") \
    /* Network tab */ \
    X(S_V_STATIC,             8, LC_N,  "static") \
    X(S_ROW_NETWORK,         20, LC_N,  "Network (Y)") \
    X(S_ROW_ROUTER_DNS,      22, LC_N,  "Router and DNS") \
    X(S_ROW_CHANNEL,         22, LC_N,  "Channel, security, MAC") \
    X(S_ROW_DEFAULT_NET,     18, LC_N,  "Default network") \
    X(S_NONE_NET,            18, LC_N,  "none") \
    X(S_USE_THIS,            10, LC_N,  "Use this") \
    X(S_ROW_RECONNECT,       18, LC_N,  "Reconnect") \
    X(S_V_RECONNECT,         18, LC_N,  "to this network") \
    X(S_RECONNECT_BTN,       10, LC_N,  "Reconnect") \
    /* Access tab */ \
    X(S_V_LOCK_ON,           18, LC_N,  "on") \
    X(S_V_LOCK_OFF,          18, LC_N,  "off: no limit") \
    X(S_N_FAILS,             18, LC_N,  "%d failures") \
    X(S_WRONG,                8, LC_N,  "wrong") \
    X(S_LAST_FMT,            18, LC_N,  "%s %s, %ld s ago") \
    X(S_ROW_LOCK,            18, LC_N,  "Login lock") \
    X(S_ROW_LOCK_AFTER,      18, LC_N,  "Lock after") \
    X(S_ROW_LOCK_TIME,       18, LC_N,  "Lock duration") \
    X(S_ROW_CLIENTS,         18, LC_N,  "Connected clients") \
    X(S_DISCONNECT,          11, LC_N,  "Disconnect") \
    X(S_ROW_BLOCKED,         18, LC_N,  "Blocked addresses") \
    X(S_UNBLOCK,              9, LC_N,  "Unblock") \
    X(S_ROW_LAST,            22, LC_N,  "Last login") \
    X(S_LOG_LOCK_ON,         -2, LC_OK, "Login lock on") \
    X(S_LOG_LOCK_OFF,        -2, LC_WA, "Login lock OFF") \
    /* Main loop */ \
    X(S_LOG_SERVER_READY,    -2, LC_OK, "Server ready: %s") \
    X(S_LOG_CHANGE_NET,      -2, LC_N,  "Changing network...") \
    X(S_LOG_NET_NOT_SAVED,   -2, LC_WA, "Network not saved") \
    X(S_LOG_RECONNECTING,    -2, LC_N,  "Reconnecting...") \
    X(S_LOG_BATT_CUTOFF,     -2, LC_ER, "Battery low: NAND writing OFF") \
    X(S_LOG_IP_LOST,         -2, LC_ER, "IP address lost, reconnecting") \
    X(S_LOG_NEW_IP,          -2, LC_WA, "New IP: %s") \
    X(S_LOG_WIFI_LOST,       -2, LC_ER, "Wi-Fi lost, reconnecting...") \
    /* Language menu */ \
    X(S_LANGUAGE,            20, LC_N,  "Language") \
    /* FTP server activity */ \
    X(S_LOG_CLIENT_CONNECTED, -2, LC_OK, "Connected: %s") \
    X(S_LOG_CLIENT_GONE,     -2, LC_N,  "Disconnected") \
    X(S_LOG_LOGIN_OK,        -2, LC_OK, "Login OK: %s") \
    X(S_LOG_LOGIN_BAD,       -2, LC_ER, "Wrong login: %s") \
    X(S_LOG_IP_BLOCKED,      -2, LC_ER, "BLOCKED %s for %d s") \
    X(S_LOG_UNBLOCKED_ALL,   -2, LC_OK, "All addresses unblocked") \
    X(S_LOG_CLIENTS_KICKED,  -2, LC_N,  "Clients disconnected") \
    X(S_LOG_NAND_FULL_ON,    -2, LC_ER, "FULL NAND WRITE ON") \
    X(S_LOG_NAND_W_ON,       -2, LC_WA, "Protected NAND writing on") \
    X(S_LOG_NAND_RO,         -2, LC_OK, "NAND read only") \
    X(S_LOG_BACKUP_DIR_FAIL, -2, LC_ER, "BACKUP FAILED: folder") \
    X(S_LOG_BACKUP_FAIL,     -3, LC_ER, "BACKUP FAILED: %s") \
    X(S_LOG_BACKUP,          -3, LC_N,  "Backup: %s") \
    X(S_LOG_NAND_WRITING,    -3, LC_WA, "Writing NAND: %s") \
    X(S_LOG_RESTORE_WARN,    -3, LC_ER, "WARNING: restore %s from the backup") \
    X(S_LOG_NAND_WRITTEN,    -3, LC_OK, "NAND written: %s") \
    X(S_LOG_NAND_NOT_WRITTEN, -3, LC_ER, "NAND NOT written: %s") \
    X(S_LOG_NAND_DELETED,    -3, LC_OK, "NAND deleted: %s") \
    X(S_LOG_NAND_NOT_DELETED, -3, LC_ER, "NAND NOT deleted: %s") \
    X(S_LOG_NAND_MKDIR,      -3, LC_OK, "NAND folder created: %s") \
    X(S_LOG_NAND_MKDIR_FAIL, -3, LC_ER, "NAND folder NOT created: %s") \
    X(S_LOG_NAND_RMDIR,      -3, LC_OK, "NAND folder deleted: %s") \
    X(S_LOG_NAND_RMDIR_FAIL, -3, LC_ER, "NAND folder NOT deleted: %s") \
    X(S_LOG_NAND_RENAMED,    -4, LC_OK, "NAND renamed: %s -> %s") \
    X(S_LOG_NAND_RENAME_FAIL, -4, LC_ER, "NAND NOT renamed: %s -> %s")
// clang-format on
