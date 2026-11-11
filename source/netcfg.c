// Saved Wi-Fi networks (see netcfg.h for the file format).

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "app.h"
#include "netcfg.h"

#define NET_PATH     CFG_DIR "/networks.ini"
#define OLD_CFG      CFG_DIR "/wifi.cfg"
#define OLD_CFG_PI   CFG_DIR "/wifi-pi.cfg"

saved_net_t netcfg[NETCFG_MAX];
int netcfg_count;

static void copy_trim(char *dst, size_t sz, const char *src)
{
    size_t l = strnlen(src, sz - 1);
    memcpy(dst, src, l);
    dst[l] = '\0';
    while (l > 0 && (dst[l - 1] == ' ' || dst[l - 1] == '\t'))
        dst[--l] = '\0';
}

// Parses "key=value" lines; a new entry starts at every "ssid=" line.
// Returns the number of entries read (appended to netcfg[]).
static int parse_file(const char *path, bool first_is_default)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;

    int added = 0;
    saved_net_t *cur = NULL;
    char line[160];
    while (fgets(line, sizeof(line), f))
    {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '#' || line[0] == ';')
            continue;
        char *v = strchr(line, '=');
        if (!v)
            continue;
        *v++ = '\0';

        if (strcmp(line, "ssid") == 0)
        {
            cur = NULL;
            if (v[0] == '\0' || netcfg_count >= NETCFG_MAX || netcfg_find(v))
                continue;
            cur = &netcfg[netcfg_count++];
            memset(cur, 0, sizeof(*cur));
            copy_trim(cur->ssid, sizeof(cur->ssid), v);
            cur->is_default = first_is_default && added == 0;
            added++;
            continue;
        }
        if (!cur)
            continue;
        if (strcmp(line, "pass") == 0)
            copy_trim(cur->pass, sizeof(cur->pass), v);
        else if (strcmp(line, "default") == 0)
            cur->is_default = (v[0] == '1');
        else if (strcmp(line, "ip") == 0)
            copy_trim(cur->ip, sizeof(cur->ip), v);
        else if (strcmp(line, "gw") == 0)
            copy_trim(cur->gw, sizeof(cur->gw), v);
        else if (strcmp(line, "mask") == 0)
            copy_trim(cur->mask, sizeof(cur->mask), v);
        else if (strcmp(line, "dns") == 0)
            copy_trim(cur->dns, sizeof(cur->dns), v);
    }
    fclose(f);
    return added;
}

void netcfg_load(void)
{
    netcfg_count = 0;
    if (!g_sd_ok)
        return;

    struct stat st;
    if (stat(NET_PATH, &st) == 0)
    {
        parse_file(NET_PATH, false);
    }
    else
    {
        // First run of 0.9: import the single-network files of older versions.
        int a = parse_file(OLD_CFG, true);
        int b = parse_file(OLD_CFG_PI, false);
        if (a + b > 0)
        {
            flog("imported %d networks from the old configuration files", a + b);
            netcfg_save();
        }
    }

    // At most one default network.
    bool seen = false;
    for (int i = 0; i < netcfg_count; i++)
    {
        if (netcfg[i].is_default && seen)
            netcfg[i].is_default = false;
        seen |= netcfg[i].is_default;
        flog("saved network: '%s'%s, password %s, ip %s", netcfg[i].ssid,
             netcfg[i].is_default ? " (default)" : "",
             netcfg[i].pass[0] ? "set" : "none",
             netcfg[i].ip[0] ? netcfg[i].ip : "DHCP");
    }
}

bool netcfg_save(void)
{
    if (!g_sd_ok)
        return false;
    mkdir("sd:/_nds", 0777);
    mkdir(CFG_DIR, 0777);
    FILE *f = fopen(NET_PATH, "w");
    if (!f)
        return false;
    fprintf(f, "# DSi FTP - saved Wi-Fi networks. Passwords are stored as plain text.\n");
    for (int i = 0; i < netcfg_count; i++)
    {
        const saved_net_t *n = &netcfg[i];
        fprintf(f, "\nssid=%s\npass=%s\ndefault=%d\n", n->ssid, n->pass, n->is_default ? 1 : 0);
        if (n->ip[0])
            fprintf(f, "ip=%s\ngw=%s\nmask=%s\ndns=%s\n", n->ip, n->gw, n->mask, n->dns);
    }
    return fclose(f) == 0;
}

saved_net_t *netcfg_find(const char *ssid)
{
    for (int i = 0; i < netcfg_count; i++)
        if (strcmp(netcfg[i].ssid, ssid) == 0)
            return &netcfg[i];
    return NULL;
}

saved_net_t *netcfg_default(void)
{
    for (int i = 0; i < netcfg_count; i++)
        if (netcfg[i].is_default)
            return &netcfg[i];
    return NULL;
}

saved_net_t *netcfg_upsert(const char *ssid, const char *pass)
{
    saved_net_t *n = netcfg_find(ssid);
    if (!n)
    {
        if (netcfg_count >= NETCFG_MAX)
            return NULL;
        n = &netcfg[netcfg_count++];
        memset(n, 0, sizeof(*n));
        copy_trim(n->ssid, sizeof(n->ssid), ssid);
    }
    copy_trim(n->pass, sizeof(n->pass), pass ? pass : "");
    return n;
}

void netcfg_set_default(const char *ssid)
{
    for (int i = 0; i < netcfg_count; i++)
        netcfg[i].is_default = (strcmp(netcfg[i].ssid, ssid) == 0);
}

void netcfg_remove(const char *ssid)
{
    for (int i = 0; i < netcfg_count; i++)
    {
        if (strcmp(netcfg[i].ssid, ssid) == 0)
        {
            memmove(&netcfg[i], &netcfg[i + 1], (netcfg_count - i - 1) * sizeof(netcfg[0]));
            netcfg_count--;
            return;
        }
    }
}
