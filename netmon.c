/*
 * NetMon  -  Lightweight System-Tray Network Interface Monitor
 * Real-time updates via NotifyIpInterfaceChange (zero polling, OS-pushed events)
 *
 * MinGW/MSYS2 build:
 *   gcc -O2 -mwindows -o netmon.exe netmon.c -lws2_32 -liphlpapi -lshell32 -lgdi32 -ldwmapi -luser32 -ladvapi32
 *
 * MSVC build: run compile_msvc.bat or compile.ps1
 */

#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <dwmapi.h>
#include <wlanapi.h>
#include <winhttp.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef _MSC_VER
#  pragma comment(lib, "ws2_32.lib")
#  pragma comment(lib, "iphlpapi.lib")
#  pragma comment(lib, "shell32.lib")
#  pragma comment(lib, "gdi32.lib")
#  pragma comment(lib, "dwmapi.lib")
#  pragma comment(lib, "user32.lib")
#  pragma comment(lib, "advapi32.lib")
#  pragma comment(lib, "wlanapi.lib")
#  pragma comment(lib, "ole32.lib")
#  pragma comment(lib, "winhttp.lib")
#endif

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#  define DWMWA_WINDOW_CORNER_PREFERENCE 33
#  define DWMWCP_ROUND 2
#endif
#ifndef DWMWA_BORDER_COLOR
#  define DWMWA_BORDER_COLOR 34
#endif

/* ── Message / ID constants ─────────────────────────────────────────────── */
#define WM_TRAYICON   (WM_USER + 1)
#define WM_NETCHANGED (WM_USER + 2)
#define WM_SCANDONE   (WM_APP  + 2)   /* posted from WLAN notif thread when a scan finishes */
#define WM_CONNDONE   (WM_APP  + 3)   /* posted from WLAN notif thread when a connect attempt finishes; wParam = reason code */
#define TIMER_TPUT    4001
#define TIMER_SCAN    4002            /* fallback fetch if the scan-complete notif is missed */
#define TIMER_CONNCLR 4003            /* clears a transient "couldn't connect" message */
#define IDM_STARTUP   3001
#define IDM_EXIT      3002
#define TRAY_UID      1

/* ── Registry ───────────────────────────────────────────────────────────── */
#define REG_RUN_KEY  L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define REG_APP_NAME L"NetMon"

/* ── Dark theme palette ─────────────────────────────────────────────────── */
#define C_BG    RGB(15,  15,  20 )
#define C_CARD  RGB(24,  24,  34 )
#define C_EDGE  RGB(48,  50,  68 )
#define C_HDR   RGB(100, 158, 244)
#define C_TXT   RGB(228, 230, 243)
#define C_SUB   RGB(128, 130, 152)
#define C_UP    RGB(70,  198, 116)
#define C_DOWN  RGB(216, 66,  66 )
#define C_DHCP  RGB(100, 180, 240)
#define C_STAT  RGB(200, 160,  60)

/* ── Layout ──────────────────────────────────────────────────────────────────
   All geometry is authored at 96 DPI (100% scale). DPS() scales any pixel
   value to the widget's current DPI (g_dpi) so the layout grows in lockstep
   with the fonts on high-DPI / scaled displays — no clipping, stays crisp.   */
#define DPS(x)    MulDiv((x), g_dpi, 96)
#define WW        DPS(400)
#define HPAD      DPS(14)
#define HDR_H     DPS(56)
#define CARD_H    DPS(150)   /* fits icon + 7 rows (name, type, ip, mask, gw, dns, mac) */
#define CARD_GAP  DPS(8)
#define BPAD      DPS(8)
#define ICON_SZ   DPS(18)    /* connection type icon size */
#define BTN_SZ    DPS(26)    /* minimize button (square) */
#define TOGGLE_H  DPS(34)    /* show/hide-disabled button height */
#define SCAN_BAR_H DPS(26)   /* Wi-Fi "scan nearby networks" toggle bar */
#define SCAN_ROW_H DPS(32)   /* one scanned network row = 2 text lines */
#define SCAN_PAD   DPS(6)    /* padding around the results dropdown */

/* ── Adapter state ──────────────────────────────────────────────────────── */
#define STATE_UP       0   /* connected, operational */
#define STATE_DOWN     1   /* enabled, no link / media disconnected */
#define STATE_DISABLED 2   /* administratively disabled */

/* ── Adapter record ─────────────────────────────────────────────────────── */
typedef struct {
    WCHAR name [256];
    WCHAR desc [128];
    WCHAR ipv4 [ 48];
    WCHAR ipv6 [ 80];
    WCHAR mac  [ 20];
    WCHAR speed[ 32];
    WCHAR dns1 [ 48];
    WCHAR dns2 [ 48];
    WCHAR mask[ 20];
    WCHAR gw  [ 48];
    WCHAR ssid[ 34];
    BOOL  up;
    BOOL  dhcp;
    DWORD type;
    int   prefix;  /* CIDR prefix length, e.g. 24 for /24 */
    int   state;   /* STATE_UP / STATE_DOWN / STATE_DISABLED */
    int   signal;  /* Wi-Fi signal quality 0-100 (0 = N/A) */
    int   channel; /* Wi-Fi channel number (0 = N/A) */
    NET_LUID luid; /* used for throughput tracking */
    UINT64 inBytes, outBytes;       /* last-seen cumulative counters */
    UINT64 rxBps, txBps;            /* live throughput (bytes/sec) */
    GUID  guid;    /* interface GUID — used to match WLAN data */
} Adapter;

/* ── Globals ────────────────────────────────────────────────────────────── */
static HWND            g_hMain        = NULL;
static HWND            g_hWid         = NULL;
static HINSTANCE       g_hInst        = NULL;
static HANDLE          g_hNotify      = NULL;
static Adapter        *g_ad           = NULL;
static int             g_nAd          = 0;
static int             g_nDisabled    = 0;   /* count of STATE_DISABLED adapters */
static BOOL            g_vis          = FALSE;
static BOOL            g_showDisabled = FALSE;
static BOOL            g_btnHover     = FALSE;
static BOOL            g_togHover     = FALSE;
static int             g_scrollY      = 0;
static NOTIFYICONDATAW g_nid;
static UINT            g_wmTaskbar;
static WCHAR           g_publicIp[64] = {0};
static volatile LONG   g_pubIpFetching = 0;
static ULONGLONG       g_lastSampleMs = 0;
static UINT            g_dpi          = 96;   /* widget render DPI; set on create + WM_DPICHANGED */

/* ── Wi-Fi scanner state ────────────────────────────────────────────────── */
/* Security type of a scanned network — drives the lock icon and which profile
   XML we generate (WPA3 needs SAE, not PSK). */
#define SEC_OPEN 0   /* open, or OWE "enhanced open" — no password */
#define SEC_PSK  1   /* WPA / WPA2-Personal (PSK) */
#define SEC_SAE  2   /* WPA3-Personal (SAE) */
typedef struct {
    WCHAR ssid[34];     /* empty = hidden SSID */
    BYTE  bssid[6];
    int   rssi;         /* signal strength in dBm */
    int   quality;      /* link quality 0-100 */
    int   band;         /* 24 / 50 / 60 (GHz×10); 0 = unknown */
    int   channel;
    int   sec;          /* SEC_OPEN / SEC_PSK / SEC_SAE */
} ScanNet;
static ScanNet        *g_scan         = NULL;
static int             g_scanCount    = 0;
static int             g_scanCap      = 0;
static BOOL            g_scanOpen     = FALSE;  /* results dropdown expanded */
static volatile LONG   g_scanning     = 0;      /* a scan is in flight */
static BOOL            g_scanHover    = FALSE;
static HANDLE          g_wlan         = NULL;   /* persistent WLAN handle for scans + notifs */
static GUID            g_scanGuid;              /* interface currently being scanned */
static BOOL            g_haveScanGuid = FALSE;

/* ── Wi-Fi connect state ────────────────────────────────────────────────── */
static int             g_connState    = 0;      /* 0 idle, 1 connecting, 2 failed (transient) */
static WCHAR           g_connSsid[34] = {0};    /* SSID shown in the connect status line */
static HWND            g_pwdEdit      = NULL;    /* password dialog edit control */
static WCHAR           g_pwdBuf[128]  = {0};
static BOOL            g_pwdOk        = FALSE;
static int             g_scanRowHover = -1;     /* scan-result row under the cursor (-1 = none) */
static int             g_scanRowPress = -1;     /* scan-result row being pressed */

/* ── Prototypes ─────────────────────────────────────────────────────────── */
static LRESULT CALLBACK MainProc  (HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK WidgetProc(HWND, UINT, WPARAM, LPARAM);
static void  RefreshAdapters(void);
static void  ShowWidget(void);
static void  HideWidget(void);
static void  ToggleWidget(void);
static void  ShowMenu(HWND hwnd);
static BOOL  GetStartup(void);
static void  SetStartup(BOOL on);
static int   CalcWidgetH(void);
static void  PositionWidget(void);
static void  PaintWidget(HWND hwnd);
static void  DrawConnectionIcon(HDC dc, int ix, int iy, DWORD type, BOOL up, int signal);
static HICON MakeIcon(BOOL anyUp);
static void  AddTrayIcon(void);
static void  UpdateTip(void);
static HFONT MakeFont(int pt10, int weight, const WCHAR *face);
static const WCHAR *TypeStr(DWORD t);
static RECT  GetBtnRect(HWND hwnd);
static RECT  GetToggleRect(HWND hwnd);
static void  FmtBps(UINT64 bps, WCHAR *out, int cap);
static void  FmtThroughput(UINT64 rx, UINT64 tx, WCHAR *out, int cap);
static void  SampleThroughput(void);
static void  CopyToClipboard(const WCHAR *s);

/* ══════════════════════════════════════════════════════════════════════════
   ADAPTER ENUMERATION
   ══════════════════════════════════════════════════════════════════════════ */

/* ── Adapter visibility (match ncpa.cpl exactly) ─────────────────────────
 *
 *  ncpa.cpl hides adapters via two mechanisms stored in the driver Class key
 *  HKLM\SYSTEM\CCS\Control\Class\{4D36E972-...}\<idx>:
 *
 *   1) NCF_HIDDEN (0x8) in the Characteristics DWORD
 *   2) *NdisDeviceType = 1
 *
 *  We build a set of hidden GUIDs once per refresh (single Class-key scan),
 *  then do fast lookups.  Additionally, adapters that lack a Connection
 *  subkey in the Network registry are ghosts (stale/removed hardware).
 */
#define NCF_HIDDEN  0x0008
#define NET_CLASS_KEY L"SYSTEM\\CurrentControlSet\\Control\\Class\\" \
                      L"{4D36E972-E325-11CE-BFC1-08002BE10318}"
#define NET_CONN_KEY  L"SYSTEM\\CurrentControlSet\\Control\\Network\\" \
                      L"{4D36E972-E325-11CE-BFC1-08002BE10318}"

#define MAX_HIDDEN  256
static WCHAR g_hiddenGuids[MAX_HIDDEN][48];
static int   g_nHidden = 0;

/* Scan the Class key once and collect GUIDs of adapters hidden from ncpa.cpl */
static void BuildHiddenSet(void)
{
    g_nHidden = 0;
    HKEY hClass;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, NET_CLASS_KEY,
                      0, KEY_READ, &hClass) != ERROR_SUCCESS)
        return;

    WCHAR subName[16];
    DWORD idx = 0, nameLen;
    while (g_nHidden < MAX_HIDDEN) {
        nameLen = 16;
        if (RegEnumKeyExW(hClass, idx++, subName, &nameLen,
                          NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        HKEY hSub;
        if (RegOpenKeyExW(hClass, subName, 0, KEY_READ, &hSub) != ERROR_SUCCESS)
            continue;

        BOOL hide = FALSE;
        DWORD val, sz;

        sz = sizeof(val);
        if (RegQueryValueExW(hSub, L"Characteristics", NULL, NULL,
                             (BYTE *)&val, &sz) == ERROR_SUCCESS)
            if (val & NCF_HIDDEN) hide = TRUE;

        sz = sizeof(val);
        if (RegQueryValueExW(hSub, L"*NdisDeviceType", NULL, NULL,
                             (BYTE *)&val, &sz) == ERROR_SUCCESS)
            if (val == 1) hide = TRUE;

        if (hide) {
            WCHAR instId[48] = {0};
            sz = sizeof(instId);
            if (RegQueryValueExW(hSub, L"NetCfgInstanceId", NULL, NULL,
                                 (BYTE *)instId, &sz) == ERROR_SUCCESS)
                wcscpy(g_hiddenGuids[g_nHidden++], instId);
        }
        RegCloseKey(hSub);
    }
    RegCloseKey(hClass);
}

/* Fast lookup: is this GUID in the hidden set? */
static BOOL IsAdapterHidden(const WCHAR *guidW)
{
    for (int i = 0; i < g_nHidden; i++)
        if (_wcsicmp(g_hiddenGuids[i], guidW) == 0) return TRUE;
    return FALSE;
}

/* Check if the adapter is registered as a network connection (shown in ncpa.cpl).
   Ghost adapters (removed hardware, stale drivers) lack this registry entry. */
static BOOL IsAdapterRegistered(const WCHAR *guidW)
{
    WCHAR path[300];
    swprintf(path, 300, L"%s\\%s\\Connection", NET_CONN_KEY, guidW);
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return TRUE;
    }
    return FALSE;
}

static int CmpAdapters(const void *a, const void *b)
{
    const Adapter *x = (const Adapter *)a, *y = (const Adapter *)b;
    if (x->state != y->state) return x->state - y->state;        /* UP → DOWN → DISABLED */
    int xw = (x->type == IF_TYPE_IEEE80211) ? 0 : 1;
    int yw = (y->type == IF_TYPE_IEEE80211) ? 0 : 1;
    return xw - yw;   /* within a state group, Wi-Fi sorts first (connected Wi-Fi on top) */
}

/* Query Wi-Fi SSID + signal strength for all connected WLAN interfaces
   and fill matching entries in g_ad[] (matched by interface GUID). */
static void QueryWlanInfo(void)
{
    HANDLE h = NULL;
    DWORD  neg;
    if (WlanOpenHandle(2, NULL, &neg, &h) != ERROR_SUCCESS) return;

    PWLAN_INTERFACE_INFO_LIST ifList = NULL;
    if (WlanEnumInterfaces(h, NULL, &ifList) != ERROR_SUCCESS) {
        WlanCloseHandle(h, NULL);
        return;
    }

    for (DWORD i = 0; i < ifList->dwNumberOfItems; i++) {
        WLAN_INTERFACE_INFO *ii = &ifList->InterfaceInfo[i];
        if (ii->isState != wlan_interface_state_connected) continue;

        PWLAN_CONNECTION_ATTRIBUTES attr = NULL;
        DWORD sz = 0;
        if (WlanQueryInterface(h, &ii->InterfaceGuid,
                               wlan_intf_opcode_current_connection,
                               NULL, &sz, (PVOID *)&attr, NULL) != ERROR_SUCCESS)
            continue;

        /* Current channel number (to derive 2.4 / 5 / 6 GHz band) */
        ULONG *chan = NULL;
        DWORD chSz = 0;
        WlanQueryInterface(h, &ii->InterfaceGuid,
                           wlan_intf_opcode_channel_number,
                           NULL, &chSz, (PVOID *)&chan, NULL);

        /* Match to adapter by GUID */
        for (int k = 0; k < g_nAd; k++) {
            if (memcmp(&g_ad[k].guid, &ii->InterfaceGuid, sizeof(GUID)) != 0) continue;

            /* SSID bytes are UTF-8 / ASCII — convert to wide */
            DOT11_SSID *s = &attr->wlanAssociationAttributes.dot11Ssid;
            int n = (int)s->uSSIDLength;
            if (n > 32) n = 32;
            MultiByteToWideChar(CP_UTF8, 0, (const char *)s->ucSSID, n,
                                g_ad[k].ssid, 33);
            g_ad[k].ssid[n] = 0;
            g_ad[k].signal  = (int)attr->wlanAssociationAttributes.wlanSignalQuality;
            g_ad[k].channel = chan ? (int)*chan : 0;
            break;
        }
        if (chan) WlanFreeMemory(chan);
        WlanFreeMemory(attr);
    }

    WlanFreeMemory(ifList);
    WlanCloseHandle(h, NULL);
}

/* ══════════════════════════════════════════════════════════════════════════
   WI-FI SCANNER  —  lists nearby APs (SSID, BSSID, band, RSSI in dBm) via the
   WLAN BSS list. The scan is async: WlanScan kicks it off, a notification (or a
   fallback timer) marshals back to the UI thread, which pulls the BSS list.
   ══════════════════════════════════════════════════════════════════════════ */

/* The scanner is hosted by the FIRST connected Wi-Fi card only, so a second
   connected Wi-Fi adapter can't render a duplicate dropdown from the shared
   (single, global) scan state. */
static BOOL IsScanCard(const Adapter *a)
{
    if (a->type != IF_TYPE_IEEE80211 || a->state != STATE_UP) return FALSE;
    for (int i = 0; i < g_nAd; i++) {
        const Adapter *c = &g_ad[i];
        if (c->type == IF_TYPE_IEEE80211 && c->state == STATE_UP)
            return c == a;          /* TRUE only if a is the first connected Wi-Fi */
    }
    return FALSE;
}

/* Extra height a card adds below the standard 7 rows (scan bar + dropdown). */
static int CardExtraH(const Adapter *a)
{
    if (!IsScanCard(a)) return 0;
    int h = SCAN_BAR_H;                          /* always-visible scan toggle */
    if (g_scanOpen && !(g_scanning && g_scanCount == 0)) {
        int rows = g_scanCount > 0 ? g_scanCount : 1;   /* 1 row = "No networks found." */
        h += SCAN_PAD + rows * SCAN_ROW_H + SCAN_PAD;
    }
    return h;
}

static int CardTotalH(const Adapter *a) { return CARD_H + CardExtraH(a); }

static const WCHAR *BandStr(int band)
{
    return band == 24 ? L"2.4 GHz" : band == 50 ? L"5 GHz" :
           band == 60 ? L"6 GHz"   : L"?";
}

/* Derive band (24/50/60) and channel from a BSS centre frequency in kHz. */
static void FreqToBand(ULONG khz, int *band, int *ch)
{
    int mhz = (int)(khz / 1000);
    if (mhz >= 2400 && mhz <= 2500)      { *band = 24; *ch = (mhz == 2484) ? 14 : (mhz - 2407) / 5; }
    else if (mhz >= 5000 && mhz < 5925)  { *band = 50; *ch = (mhz - 5000) / 5; }
    else if (mhz >= 5925 && mhz <= 7125) { *band = 60; *ch = (mhz - 5950) / 5; }
    else                                 { *band = 0;  *ch = 0; }
}

static void FmtBssid(const BYTE *b, WCHAR *out, int cap)
{
    swprintf(out, cap, L"%02X:%02X:%02X:%02X:%02X:%02X",
             b[0], b[1], b[2], b[3], b[4], b[5]);
}

/* Sort scanned networks strongest-signal first. */
static int CmpScan(const void *a, const void *b)
{
    return ((const ScanNet *)b)->rssi - ((const ScanNet *)a)->rssi;
}

/* WLAN notification — runs on a WLAN thread; only marshals to the UI thread. */
static void WINAPI WlanNotifyCb(PWLAN_NOTIFICATION_DATA data, PVOID ctx)
{
    (void)ctx;
    if (!data || data->NotificationSource != WLAN_NOTIFICATION_SOURCE_ACM) return;
    switch (data->NotificationCode) {
        case wlan_notification_acm_scan_complete:
        case wlan_notification_acm_scan_fail:
            PostMessageW(g_hWid, WM_SCANDONE, 0, 0);
            break;
        case wlan_notification_acm_connection_complete:
        case wlan_notification_acm_connection_attempt_fail: {
            WLAN_CONNECTION_NOTIFICATION_DATA *cd = (WLAN_CONNECTION_NOTIFICATION_DATA *)data->pData;
            PostMessageW(g_hWid, WM_CONNDONE, (WPARAM)(cd ? cd->wlanReasonCode : 1), 0);
            break;
        }
    }
}

/* Lazily open a persistent WLAN handle and register for scan notifications. */
static BOOL EnsureWlan(void)
{
    if (g_wlan) return TRUE;
    DWORD neg;
    if (WlanOpenHandle(2, NULL, &neg, &g_wlan) != ERROR_SUCCESS) { g_wlan = NULL; return FALSE; }
    DWORD prev;
    WlanRegisterNotification(g_wlan, WLAN_NOTIFICATION_SOURCE_ACM, TRUE,
                             WlanNotifyCb, NULL, NULL, &prev);
    return TRUE;
}

/* Map a DOT11_AUTH_ALGORITHM (taken as int, so it builds on SDKs predating the
   WPA3 enumerators) to our SEC_* type. */
static int AuthToSec(int authAlgo, BOOL secEnabled)
{
    switch (authAlgo) {
        case 9:  return SEC_SAE;     /* DOT11_AUTH_ALGO_WPA3_SAE  → WPA3-Personal */
        case 7:                      /* DOT11_AUTH_ALGO_RSNA_PSK  → WPA2-Personal */
        case 4:  return SEC_PSK;     /* DOT11_AUTH_ALGO_WPA_PSK   → WPA-Personal  */
        case 1:                      /* DOT11_AUTH_ALGO_80211_OPEN → open         */
        case 10: return SEC_OPEN;    /* DOT11_AUTH_ALGO_OWE       → enhanced open */
        default: return secEnabled ? SEC_PSK : SEC_OPEN;
    }
}

/* Pull the latest BSS list into g_scan[] (UI thread, on scan completion). */
static void FetchScanResults(void)
{
    KillTimer(g_hWid, TIMER_SCAN);
    g_scanCount = 0;
    if (g_wlan && g_haveScanGuid) {
        PWLAN_BSS_LIST list = NULL;
        if (WlanGetNetworkBssList(g_wlan, &g_scanGuid, NULL,
                                  dot11_BSS_type_any, FALSE, NULL, &list) == ERROR_SUCCESS && list) {
            for (DWORD i = 0; i < list->dwNumberOfItems; i++) {
                WLAN_BSS_ENTRY *e = &list->wlanBssEntries[i];
                if (g_scanCount >= g_scanCap) {
                    int ncap = g_scanCap ? g_scanCap * 2 : 32;
                    ScanNet *np = (ScanNet *)realloc(g_scan, (size_t)ncap * sizeof(ScanNet));
                    if (!np) break;
                    g_scan = np; g_scanCap = ncap;
                }
                ScanNet *n = &g_scan[g_scanCount];
                int len = (int)e->dot11Ssid.uSSIDLength; if (len > 32) len = 32;
                if (len > 0) {
                    int w = MultiByteToWideChar(CP_UTF8, 0, (const char *)e->dot11Ssid.ucSSID,
                                                len, n->ssid, 33);
                    n->ssid[(w > 0 && w <= 32) ? w : 0] = 0;
                } else {
                    n->ssid[0] = 0;                 /* hidden network */
                }
                memcpy(n->bssid, e->dot11Bssid, 6);
                n->rssi    = (int)e->lRssi;
                n->quality = (int)e->uLinkQuality;
                n->sec = (e->usCapabilityInformation & 0x0010) ? SEC_PSK : SEC_OPEN;  /* default; refined below */
                FreqToBand(e->ulChCenterFrequency, &n->band, &n->channel);
                g_scanCount++;
            }
            WlanFreeMemory(list);
        }
    }

    /* Refine the security type per SSID from the available-network list, which
       reports the auth algorithm — so we can tell WPA3 (SAE) from WPA2 (PSK).
       The BSS Privacy bit only says secured/open. */
    if (g_wlan && g_haveScanGuid && g_scanCount > 0) {
        PWLAN_AVAILABLE_NETWORK_LIST avail = NULL;
        if (WlanGetAvailableNetworkList(g_wlan, &g_scanGuid, 0, NULL, &avail) == ERROR_SUCCESS && avail) {
            for (DWORD i = 0; i < avail->dwNumberOfItems; i++) {
                WLAN_AVAILABLE_NETWORK *an = &avail->Network[i];
                WCHAR aw[34];
                int al = (int)an->dot11Ssid.uSSIDLength; if (al > 32) al = 32;
                int w  = al > 0 ? MultiByteToWideChar(CP_UTF8, 0, (const char *)an->dot11Ssid.ucSSID, al, aw, 33) : 0;
                aw[(w > 0 && w <= 32) ? w : 0] = 0;
                if (!aw[0]) continue;
                int sec = AuthToSec((int)an->dot11DefaultAuthAlgorithm, an->bSecurityEnabled);
                for (int k = 0; k < g_scanCount; k++)
                    if (wcscmp(g_scan[k].ssid, aw) == 0) g_scan[k].sec = sec;
            }
            WlanFreeMemory(avail);
        }
    }

    if (g_scanCount > 1) qsort(g_scan, g_scanCount, sizeof(ScanNet), CmpScan);
    g_scanning = 0;
}

/* Begin an async scan of the given Wi-Fi interface. */
static void StartWifiScan(const GUID *guid)
{
    if (InterlockedExchange(&g_scanning, 1) == 1) return;   /* one scan at a time */
    if (!EnsureWlan()) { g_scanning = 0; return; }
    g_scanGuid     = *guid;
    g_haveScanGuid = TRUE;
    g_scanCount    = 0;
    /* Arm the safety-net timer FIRST so g_scanning is always cleared eventually,
       even if both the scan-complete notification and the post below are lost. */
    SetTimer(g_hWid, TIMER_SCAN, 5000, NULL);
    if (WlanScan(g_wlan, &g_scanGuid, NULL, NULL, NULL) != ERROR_SUCCESS)
        PostMessageW(g_hWid, WM_SCANDONE, 0, 0);            /* try the cached BSS list sooner */
}

/* ══════════════════════════════════════════════════════════════════════════
   WI-FI CONNECT  —  click a scanned network to join it. If Windows already has
   a saved profile we connect straight away; otherwise we prompt for a password,
   persist a profile (so it's instant next time), then connect. Result arrives
   asynchronously via WM_CONNDONE.
   ══════════════════════════════════════════════════════════════════════════ */

/* XML-escape a string into out (NUL-terminated, bounded by cap). */
static void XmlEsc(const WCHAR *in, WCHAR *out, int cap)
{
    int o = 0;
    for (int i = 0; in[i] && o < cap - 7; i++) {
        const WCHAR *r = NULL;
        switch (in[i]) {
            case L'&':  r = L"&amp;";  break;
            case L'<':  r = L"&lt;";   break;
            case L'>':  r = L"&gt;";   break;
            case L'"':  r = L"&quot;"; break;
            case L'\'': r = L"&apos;"; break;
        }
        if (r) while (*r) out[o++] = *r++;
        else   out[o++] = in[i];
    }
    out[o] = 0;
}

/* Build a WLAN profile XML for SSID: WPA3-SAE, WPA2-PSK, or open per `sec`. */
static void BuildProfileXml(const WCHAR *ssid, const WCHAR *pwd, int sec, WCHAR *out, int cap)
{
    WCHAR es[80], ep[260];
    XmlEsc(ssid, es, 80);
    if (sec == SEC_OPEN) {
        swprintf(out, cap,
            L"<?xml version=\"1.0\"?>"
            L"<WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">"
            L"<name>%s</name>"
            L"<SSIDConfig><SSID><name>%s</name></SSID></SSIDConfig>"
            L"<connectionType>ESS</connectionType><connectionMode>auto</connectionMode>"
            L"<MSM><security><authEncryption><authentication>open</authentication>"
            L"<encryption>none</encryption><useOneX>false</useOneX></authEncryption>"
            L"</security></MSM></WLANProfile>", es, es);
    } else {
        /* WPA3-Personal uses SAE; WPA2-Personal uses PSK. Both take a passphrase. */
        const WCHAR *auth = (sec == SEC_SAE) ? L"WPA3SAE" : L"WPA2PSK";
        XmlEsc(pwd, ep, 260);
        swprintf(out, cap,
            L"<?xml version=\"1.0\"?>"
            L"<WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">"
            L"<name>%s</name>"
            L"<SSIDConfig><SSID><name>%s</name></SSID></SSIDConfig>"
            L"<connectionType>ESS</connectionType><connectionMode>auto</connectionMode>"
            L"<MSM><security>"
            L"<authEncryption><authentication>%s</authentication>"
            L"<encryption>AES</encryption><useOneX>false</useOneX></authEncryption>"
            L"<sharedKey><keyType>passPhrase</keyType><protected>false</protected>"
            L"<keyMaterial>%s</keyMaterial></sharedKey>"
            L"</security></MSM></WLANProfile>", es, es, auth, ep);
    }
}

/* TRUE if Windows already has a saved profile for this SSID on the scan interface. */
static BOOL HasSavedProfile(const WCHAR *ssid)
{
    if (!g_wlan || !g_haveScanGuid) return FALSE;
    LPWSTR xml = NULL; DWORD flags = 0, access = 0;
    if (WlanGetProfile(g_wlan, &g_scanGuid, ssid, NULL, &xml, &flags, &access) == ERROR_SUCCESS) {
        if (xml) WlanFreeMemory(xml);
        return TRUE;
    }
    return FALSE;
}

/* Connect the scan interface using a saved profile named profileName (== SSID). */
static DWORD DoWlanConnect(const WCHAR *profileName)
{
    WLAN_CONNECTION_PARAMETERS cp = {0};
    cp.wlanConnectionMode = wlan_connection_mode_profile;
    cp.strProfile         = profileName;
    cp.dot11BssType       = dot11_BSS_type_infrastructure;
    return WlanConnect(g_wlan, &g_scanGuid, &cp, NULL);
}

/* ── modal password prompt ──────────────────────────────────────────────── */
static LRESULT CALLBACK PwdProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDOK:
                    GetWindowTextW(g_pwdEdit, g_pwdBuf, 128);
                    g_pwdOk = TRUE;  DestroyWindow(hwnd); return 0;
                case IDCANCEL:
                    g_pwdOk = FALSE; DestroyWindow(hwnd); return 0;
                case 101:   /* show-password checkbox */
                    if (HIWORD(wp) == BN_CLICKED) {
                        BOOL show = SendDlgItemMessageW(hwnd, 101, BM_GETCHECK, 0, 0) == BST_CHECKED;
                        SendMessageW(g_pwdEdit, EM_SETPASSWORDCHAR, show ? 0 : (WPARAM)0x25CF, 0);
                        InvalidateRect(g_pwdEdit, NULL, TRUE);
                    }
                    return 0;
            }
            break;
        case WM_CLOSE: g_pwdOk = FALSE; DestroyWindow(hwnd); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* Show a modal password dialog for ssid; returns TRUE and fills out on Connect. */
static BOOL PromptPassword(const WCHAR *ssid, WCHAR *out, int cap)
{
    static BOOL reg = FALSE;
    if (!reg) {
        WNDCLASSEXW wc = {0};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = PwdProc;
        wc.hInstance     = g_hInst;
        wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"NetMon_PwdDlg";
        RegisterClassExW(&wc);
        reg = TRUE;
    }

    int W = DPS(380), H = DPS(200);
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = wa.left + ((wa.right - wa.left) - W) / 2;
    int y = wa.top  + ((wa.bottom - wa.top) - H) / 2;
    WCHAR title[80]; swprintf(title, 80, L"Connect to %s", ssid);

    HWND dlg = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
        L"NetMon_PwdDlg", title, WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, W, H, g_hWid, NULL, g_hInst, NULL);
    if (!dlg) return FALSE;

    HFONT df = MakeFont(95, FW_NORMAL, L"Segoe UI");
    HWND lbl = CreateWindowExW(0, L"STATIC", L"Enter the network password:",
        WS_CHILD | WS_VISIBLE, DPS(16), DPS(12), DPS(340), DPS(20), dlg, NULL, g_hInst, NULL);
    g_pwdEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL,
        DPS(16), DPS(38), DPS(348), DPS(26), dlg, (HMENU)(INT_PTR)100, g_hInst, NULL);
    HWND chk = CreateWindowExW(0, L"BUTTON", L"Show password",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        DPS(16), DPS(72), DPS(160), DPS(22), dlg, (HMENU)(INT_PTR)101, g_hInst, NULL);
    HWND ok = CreateWindowExW(0, L"BUTTON", L"Connect",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        W - DPS(196), DPS(106), DPS(86), DPS(30), dlg, (HMENU)(INT_PTR)IDOK, g_hInst, NULL);
    HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        W - DPS(102), DPS(106), DPS(86), DPS(30), dlg, (HMENU)(INT_PTR)IDCANCEL, g_hInst, NULL);

    SendMessageW(lbl,       WM_SETFONT, (WPARAM)df, TRUE);
    SendMessageW(g_pwdEdit, WM_SETFONT, (WPARAM)df, TRUE);
    SendMessageW(chk,       WM_SETFONT, (WPARAM)df, TRUE);
    SendMessageW(ok,        WM_SETFONT, (WPARAM)df, TRUE);
    SendMessageW(cancel,    WM_SETFONT, (WPARAM)df, TRUE);
    SendMessageW(g_pwdEdit, EM_SETPASSWORDCHAR, (WPARAM)0x25CF, 0);   /* bullet char */

    g_pwdOk = FALSE; g_pwdBuf[0] = 0;
    EnableWindow(g_hWid, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
    SetFocus(g_pwdEdit);

    MSG m;
    for (;;) {
        if (!IsWindow(dlg)) break;
        int gm = GetMessageW(&m, NULL, 0, 0);
        if (gm == 0)  { PostQuitMessage((int)m.wParam); break; }   /* re-post WM_QUIT for the outer loop */
        if (gm == -1) break;
        if (m.message == WM_KEYDOWN && m.wParam == VK_RETURN) { SendMessageW(dlg, WM_COMMAND, IDOK, 0);     continue; }
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE) { SendMessageW(dlg, WM_COMMAND, IDCANCEL, 0); continue; }
        if (!IsDialogMessageW(dlg, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }

    EnableWindow(g_hWid, TRUE);
    DeleteObject(df);
    g_pwdEdit = NULL;

    BOOL ok2 = g_pwdOk;
    if (ok2) { wcsncpy(out, g_pwdBuf, cap - 1); out[cap - 1] = 0; }
    SecureZeroMemory(g_pwdBuf, sizeof(g_pwdBuf));
    return ok2;
}

/* Orchestrate a click-to-connect on scan row idx. */
static void ConnectToScan(int idx)
{
    if (idx < 0 || idx >= g_scanCount || !g_wlan || !g_haveScanGuid) return;
    ScanNet *n = &g_scan[idx];
    if (!n->ssid[0]) return;                       /* can't join a hidden SSID by click */

    if (!HasSavedProfile(n->ssid)) {
        WCHAR pwd[128] = {0};
        if (n->sec != SEC_OPEN && !PromptPassword(n->ssid, pwd, 128)) return;   /* cancelled */

        WCHAR xml[1300];
        BuildProfileXml(n->ssid, pwd, n->sec, xml, 1300);
        SecureZeroMemory(pwd, sizeof(pwd));
        DWORD reason = 0;
        DWORD r = WlanSetProfile(g_wlan, &g_scanGuid, 0x00000002 /* WLAN_PROFILE_USER */,
                                 xml, NULL, TRUE, NULL, &reason);
        SecureZeroMemory(xml, sizeof(xml));
        if (r != ERROR_SUCCESS) {
            g_connState = 2; wcsncpy(g_connSsid, n->ssid, 33); g_connSsid[33] = 0;
            SetTimer(g_hWid, TIMER_CONNCLR, 4000, NULL);
            InvalidateRect(g_hWid, NULL, TRUE);
            return;
        }
    }

    g_connState = 1;
    wcsncpy(g_connSsid, n->ssid, 33); g_connSsid[33] = 0;
    InvalidateRect(g_hWid, NULL, TRUE);
    if (DoWlanConnect(n->ssid) != ERROR_SUCCESS) {       /* couldn't even start the attempt */
        g_connState = 2;
        SetTimer(g_hWid, TIMER_CONNCLR, 4000, NULL);
        InvalidateRect(g_hWid, NULL, TRUE);
    }
    /* the actual association result arrives asynchronously via WM_CONNDONE */
}

static void RefreshAdapters(void)
{
    BuildHiddenSet();   /* one-time Class-key scan per refresh */

    ULONG flags = GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_INCLUDE_GATEWAYS
                | 0x0100 /* GAA_FLAG_INCLUDE_ALL_INTERFACES — needed for disabled adapters */;
    ULONG sz    = 0;
    GetAdaptersAddresses(AF_UNSPEC, flags, NULL, NULL, &sz);

    PIP_ADAPTER_ADDRESSES buf = (PIP_ADAPTER_ADDRESSES)malloc(sz + 512);
    if (!buf) return;

    if (GetAdaptersAddresses(AF_UNSPEC, flags, NULL, buf, &sz) != NO_ERROR) {
        free(buf); return;
    }

    int n = 0;
    for (PIP_ADAPTER_ADDRESSES p = buf; p; p = p->Next)
        if (p->IfType != IF_TYPE_SOFTWARE_LOOPBACK) n++;

    free(g_ad);
    g_ad  = (Adapter *)calloc(n > 0 ? n : 1, sizeof(Adapter));
    g_nAd = 0;

    for (PIP_ADAPTER_ADDRESSES p = buf; p; p = p->Next) {
        if (p->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (p->IfType == IF_TYPE_TUNNEL)            continue;

        /* Skip NDIS filter binding layers (QoS, Npcap, WFP, etc.) */
        MIB_IF_ROW2 row = {0};
        row.InterfaceLuid = p->Luid;
        BOOL ifOk = (GetIfEntry2(&row) == NO_ERROR);
        if (ifOk && row.InterfaceAndOperStatusFlags.FilterInterface)
            continue;

        /* Convert AdapterName (GUID) to wide string for registry lookups */
        WCHAR guidW[48] = {0};
        MultiByteToWideChar(CP_ACP, 0, p->AdapterName, -1, guidW, 48);

        /* Skip adapters hidden from ncpa.cpl via NCF_HIDDEN or *NdisDeviceType=1 */
        if (IsAdapterHidden(guidW))
            continue;

        /* Ghost check: adapter not registered in Network Connections → skip */
        if (!IsAdapterRegistered(guidW))
            continue;

        Adapter *a = &g_ad[g_nAd++];
        wcsncpy(a->name, p->FriendlyName, 255);
        wcsncpy(a->desc, p->Description,  127);
        a->up   = (p->OperStatus == IfOperStatusUp);
        a->type = p->IfType;
        a->dhcp = (p->Flags & IP_ADAPTER_DHCP_ENABLED) != 0;

        /* Parse GUID string (e.g. "{F039AFCB-...}") into binary GUID */
        {
            WCHAR gbuf[48];
            wcscpy(gbuf, guidW);
            IIDFromString(gbuf, &a->guid);
        }

        /* Determine adapter state:
         *  AdminStatus DOWN          → explicitly disabled (ncpa.cpl / Device Mgr)
         *  OperStatus  != Up         → enabled but no link  (cable out, WiFi off)
         *  MediaConnectState Unknown → no physical medium (virtual adapter)
         *  Otherwise                 → fully connected
         */
        if (ifOk && row.AdminStatus == NET_IF_ADMIN_STATUS_DOWN) {
            a->state = STATE_DISABLED;
        } else if (p->OperStatus != IfOperStatusUp) {
            a->state = STATE_DOWN;
        } else if (ifOk && row.MediaConnectState == MediaConnectStateUnknown) {
            a->state = STATE_DISABLED;
        } else {
            a->state = STATE_UP;
        }
        /* MAC */
        if (p->PhysicalAddressLength >= 6)
            swprintf(a->mac, 20, L"%02X:%02X:%02X:%02X:%02X:%02X",
                p->PhysicalAddress[0], p->PhysicalAddress[1],
                p->PhysicalAddress[2], p->PhysicalAddress[3],
                p->PhysicalAddress[4], p->PhysicalAddress[5]);
        else
            wcscpy(a->mac, L"\u2014");

        /* Speed */
        UINT64 bps = p->TransmitLinkSpeed;
        if (bps == 0 || bps == (UINT64)-1)
            wcscpy(a->speed, L"\u2014");
        else if (bps >= 1000000000ULL)
            swprintf(a->speed, 32, L"%llu Gbps", bps / 1000000000ULL);
        else
            swprintf(a->speed, 32, L"%llu Mbps", bps / 1000000ULL);

        /* IPv4 + subnet prefix length + mask */
        for (PIP_ADAPTER_UNICAST_ADDRESS ua = p->FirstUnicastAddress; ua; ua = ua->Next) {
            if (ua->Address.lpSockaddr->sa_family == AF_INET) {
                struct sockaddr_in *s = (struct sockaddr_in *)ua->Address.lpSockaddr;
                InetNtopW(AF_INET, &s->sin_addr, a->ipv4, 48);
                a->prefix = ua->OnLinkPrefixLength;
                /* Convert prefix to dotted mask (e.g. 24 → 255.255.255.0) */
                UINT32 m = a->prefix ? (~0U << (32 - a->prefix)) : 0;
                swprintf(a->mask, 20, L"%u.%u.%u.%u",
                         (m >> 24) & 0xFF, (m >> 16) & 0xFF,
                         (m >>  8) & 0xFF,  m        & 0xFF);
                break;
            }
        }

        /* IPv6 - prefer global over link-local */
        for (PIP_ADAPTER_UNICAST_ADDRESS ua = p->FirstUnicastAddress; ua; ua = ua->Next) {
            if (ua->Address.lpSockaddr->sa_family == AF_INET6) {
                struct sockaddr_in6 *s = (struct sockaddr_in6 *)ua->Address.lpSockaddr;
                WCHAR tmp[80] = {0};
                InetNtopW(AF_INET6, &s->sin6_addr, tmp, 80);
                if (a->ipv6[0] == 0) wcscpy(a->ipv6, tmp);
                if (wcsncmp(tmp, L"fe80", 4) != 0) { wcscpy(a->ipv6, tmp); break; }
            }
        }

        /* Default gateway (first IPv4 or IPv6) */
        for (PIP_ADAPTER_GATEWAY_ADDRESS g = p->FirstGatewayAddress; g; g = g->Next) {
            if (g->Address.lpSockaddr->sa_family == AF_INET) {
                struct sockaddr_in *s = (struct sockaddr_in *)g->Address.lpSockaddr;
                InetNtopW(AF_INET, &s->sin_addr, a->gw, 48);
                break;
            }
        }
        if (a->gw[0] == 0) {
            for (PIP_ADAPTER_GATEWAY_ADDRESS g = p->FirstGatewayAddress; g; g = g->Next) {
                if (g->Address.lpSockaddr->sa_family == AF_INET6) {
                    struct sockaddr_in6 *s = (struct sockaddr_in6 *)g->Address.lpSockaddr;
                    InetNtopW(AF_INET6, &s->sin6_addr, a->gw, 48);
                    break;
                }
            }
        }

        /* Save LUID for throughput tracking */
        a->luid = p->Luid;

        /* DNS servers (up to 2) */
        int di = 0;
        for (PIP_ADAPTER_DNS_SERVER_ADDRESS ds = p->FirstDnsServerAddress;
             ds && di < 2; ds = ds->Next, di++)
        {
            WCHAR *tgt = (di == 0) ? a->dns1 : a->dns2;
            if (ds->Address.lpSockaddr->sa_family == AF_INET) {
                struct sockaddr_in *s = (struct sockaddr_in *)ds->Address.lpSockaddr;
                InetNtopW(AF_INET, &s->sin_addr, tgt, 48);
            } else if (ds->Address.lpSockaddr->sa_family == AF_INET6) {
                struct sockaddr_in6 *s = (struct sockaddr_in6 *)ds->Address.lpSockaddr;
                InetNtopW(AF_INET6, &s->sin6_addr, tgt, 48);
            }
        }

        /* Disconnected + DHCP → clear APIPA 169.254.x.x and stale DNS.
           Only static IPs are meaningful when the cable is unplugged. */
        if (a->state == STATE_DOWN && a->dhcp) {
            if (wcsncmp(a->ipv4, L"169.254.", 8) == 0) a->ipv4[0] = 0;
            if (wcsncmp(a->ipv6, L"fe80:", 5) == 0)    a->ipv6[0] = 0;
            a->dns1[0] = 0;
            a->dns2[0] = 0;
            a->gw[0]   = 0;
        }

        /* Safety net: UP with no IPv4 AND no IPv6 → virtual/parasitic adapter
           that slipped past the FilterInterface check. */
        if (a->state == STATE_UP && a->ipv4[0] == 0 && a->ipv6[0] == 0)
            a->state = STATE_DISABLED;

        a->up = (a->state == STATE_UP);
    }

    free(buf);

    /* Attach Wi-Fi SSID + signal strength for connected WLAN adapters */
    QueryWlanInfo();

    /* Sort: UP → DOWN → DISABLED */
    qsort(g_ad, g_nAd, sizeof(Adapter), CmpAdapters);

    /* Count disabled */
    g_nDisabled = 0;
    for (int i = 0; i < g_nAd; i++)
        if (g_ad[i].state == STATE_DISABLED) g_nDisabled++;

    /* Drop stale scan results if the scanned Wi-Fi interface is no longer a
       connected adapter (e.g. disconnect/reconnect) — otherwise the dropdown
       would re-show the old network's BSSIDs as if current. */
    if (g_haveScanGuid) {
        BOOL ownerUp = FALSE;
        for (int i = 0; i < g_nAd; i++)
            if (g_ad[i].state == STATE_UP && g_ad[i].type == IF_TYPE_IEEE80211 &&
                memcmp(&g_ad[i].guid, &g_scanGuid, sizeof(GUID)) == 0) { ownerUp = TRUE; break; }
        if (!ownerUp) { g_scanOpen = FALSE; g_scanCount = 0; g_haveScanGuid = FALSE; }
    }
}

/* ══════════════════════════════════════════════════════════════════════════
   TRAY ICON
   ══════════════════════════════════════════════════════════════════════════ */
static HICON MakeIcon(BOOL anyUp)
{
    int W = 16;
    HDC scr = GetDC(NULL);
    HDC dc  = CreateCompatibleDC(scr);
    HBITMAP bmp = CreateCompatibleBitmap(scr, W, W);
    ReleaseDC(NULL, scr);
    HBITMAP oldBmp = (HBITMAP)SelectObject(dc, bmp);

    RECT r = {0, 0, W, W};
    FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));

    HBRUSH barBr = CreateSolidBrush(anyUp ? C_UP : RGB(90, 90, 110));
    SelectObject(dc, (HPEN)GetStockObject(NULL_PEN));
    SelectObject(dc, barBr);
    RECT b1 = {2, 12, 5,  16};
    RECT b2 = {6,  8, 9,  16};
    RECT b3 = {10, 4, 13, 16};
    FillRect(dc, &b1, barBr);
    FillRect(dc, &b2, barBr);
    FillRect(dc, &b3, barBr);
    DeleteObject(barBr);

    HBRUSH dotBr = CreateSolidBrush(anyUp ? C_UP : C_DOWN);
    RECT dot = {14, 0, 16, 2};
    FillRect(dc, &dot, dotBr);
    DeleteObject(dotBr);

    SelectObject(dc, oldBmp);
    DeleteDC(dc);

    HBITMAP mask   = CreateBitmap(W, W, 1, 1, NULL);
    HDC     maskDC = CreateCompatibleDC(NULL);
    HBITMAP oldM   = (HBITMAP)SelectObject(maskDC, mask);
    FillRect(maskDC, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SelectObject(maskDC, oldM);
    DeleteDC(maskDC);

    ICONINFO ii = {TRUE, 0, 0, mask, bmp};
    HICON icon  = CreateIconIndirect(&ii);
    DeleteObject(bmp);
    DeleteObject(mask);
    return icon;
}

/* Fetch public IP from api.ipify.org in a background thread so the
   UI thread never blocks on network I/O.  Result is stored in
   g_publicIp and a repaint is posted to the widget. */
static DWORD WINAPI PublicIpThread(LPVOID arg)
{
    (void)arg;
    HINTERNET hSession = WinHttpOpen(L"NetMon/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) goto done;

    HINTERNET hConn = WinHttpConnect(hSession, L"api.ipify.org", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConn) { WinHttpCloseHandle(hSession); goto done; }

    HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", L"/",
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!hReq) { WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSession); goto done; }

    /* 5-second timeouts so we never hang if the server is slow */
    WinHttpSetTimeouts(hReq, 5000, 5000, 5000, 5000);

    if (WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        && WinHttpReceiveResponse(hReq, NULL))
    {
        char buf[64] = {0};
        DWORD got = 0;
        WinHttpReadData(hReq, buf, sizeof(buf) - 1, &got);
        buf[got] = 0;
        /* api.ipify.org returns the raw IP, no JSON */
        MultiByteToWideChar(CP_UTF8, 0, buf, -1, g_publicIp, 64);
        if (g_hWid) PostMessageW(g_hWid, WM_APP + 1, 0, 0);
    }

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSession);

done:
    InterlockedExchange(&g_pubIpFetching, 0);
    return 0;
}

static void FetchPublicIpAsync(void)
{
    /* Guard against duplicate concurrent fetches */
    if (InterlockedCompareExchange(&g_pubIpFetching, 1, 0) != 0) return;
    HANDLE h = CreateThread(NULL, 0, PublicIpThread, NULL, 0, NULL);
    if (h) CloseHandle(h);
    else InterlockedExchange(&g_pubIpFetching, 0);
}

static void UpdateTip(void)
{
    /* Pick the "primary" connected adapter: first STATE_UP non-virtual.
       Prefer Wi-Fi or Ethernet over everything else. */
    const Adapter *best = NULL;
    for (int i = 0; i < g_nAd; i++) {
        if (g_ad[i].state != STATE_UP) continue;
        if (g_ad[i].ipv4[0] == 0) continue;
        if (!best ||
            (g_ad[i].type == IF_TYPE_IEEE80211 || g_ad[i].type == IF_TYPE_ETHERNET_CSMACD))
        {
            best = &g_ad[i];
            if (best->type == IF_TYPE_IEEE80211 ||
                best->type == IF_TYPE_ETHERNET_CSMACD) break;
        }
    }

    int up = 0, visible = 0;
    for (int i = 0; i < g_nAd; i++) {
        if (g_ad[i].state == STATE_UP)       up++;
        if (g_ad[i].state != STATE_DISABLED) visible++;
    }

    if (best) {
        if (best->type == IF_TYPE_IEEE80211 && best->ssid[0])
            swprintf(g_nid.szTip, 128, L"NetMon  \u2013  Wi-Fi: %s (%d%%)\n%s",
                     best->ssid, best->signal, best->ipv4);
        else
            swprintf(g_nid.szTip, 128, L"NetMon  \u2013  %s\n%s",
                     TypeStr(best->type), best->ipv4);
    } else {
        swprintf(g_nid.szTip, 128, L"NetMon  \u2013  %d / %d up", up, visible);
    }
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void AddTrayIcon(void)
{
    BOOL anyUp = FALSE;
    for (int i = 0; i < g_nAd; i++) if (g_ad[i].up) { anyUp = TRUE; break; }

    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = g_hMain;
    g_nid.uID              = TRAY_UID;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = MakeIcon(anyUp);
    wcscpy(g_nid.szTip, L"NetMon");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    UpdateTip();
}

/* ══════════════════════════════════════════════════════════════════════════
   STARTUP REGISTRY
   ══════════════════════════════════════════════════════════════════════════ */
static BOOL GetStartup(void)
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return FALSE;
    BOOL found = (RegQueryValueExW(key, REG_APP_NAME, NULL, NULL, NULL, NULL) == ERROR_SUCCESS);
    RegCloseKey(key);
    return found;
}

static void SetStartup(BOOL on)
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;
    if (on) {
        WCHAR path[MAX_PATH];
        GetModuleFileNameW(NULL, path, MAX_PATH);
        WCHAR val[MAX_PATH + 4];
        swprintf(val, MAX_PATH + 3, L"\"%s\"", path);
        RegSetValueExW(key, REG_APP_NAME, 0, REG_SZ,
                       (BYTE *)val, (DWORD)((wcslen(val) + 1) * sizeof(WCHAR)));
    } else {
        RegDeleteValueW(key, REG_APP_NAME);
    }
    RegCloseKey(key);
}

/* ══════════════════════════════════════════════════════════════════════════
   CONTEXT MENU
   ══════════════════════════════════════════════════════════════════════════ */
static void ShowMenu(HWND hwnd)
{
    HMENU menu = CreatePopupMenu();
    UINT  flag = GetStartup() ? MF_CHECKED : MF_UNCHECKED;
    AppendMenuW(menu, MF_STRING | flag, IDM_STARTUP, L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    SetForegroundWindow(hwnd);
    POINT pt;
    GetCursorPos(&pt);
    TrackPopupMenu(menu, TPM_BOTTOMALIGN | TPM_RIGHTALIGN | TPM_LEFTBUTTON,
                   pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(menu);
}

/* ══════════════════════════════════════════════════════════════════════════
   WIDGET WINDOW
   ══════════════════════════════════════════════════════════════════════════ */
static int CountVisible(void)
{
    int n = 0;
    for (int i = 0; i < g_nAd; i++)
        if (g_ad[i].state != STATE_DISABLED || g_showDisabled) n++;
    return n > 0 ? n : 1;
}

/* Height of the scrollable card area (excludes header and toggle).
   Cards are no longer uniform — the Wi-Fi card grows with its scan dropdown. */
static int CardsContentH(void)
{
    int total = 0, vis = 0;
    for (int i = 0; i < g_nAd; i++) {
        if (g_ad[i].state == STATE_DISABLED && !g_showDisabled) continue;
        total += CardTotalH(&g_ad[i]) + CARD_GAP;
        vis++;
    }
    if (vis == 0) total = CARD_H + CARD_GAP;   /* "no adapters" placeholder */
    return total + BPAD;
}

/* Height of the fixed bottom bar (toggle button) */
static int ToggleBarH(void)
{
    return (g_nDisabled > 0) ? TOGGLE_H + CARD_GAP : 0;
}

/* Total content height */
static int ContentHeight(void)
{
    return HDR_H + CardsContentH() + ToggleBarH();
}

/* Visible widget height (capped to fit work area) */
static int CalcWidgetH(void)
{
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int maxH    = (work.bottom - work.top) - DPS(40);
    int content = ContentHeight();
    return content < maxH ? content : maxH;
}

static void ClampScroll(void)
{
    /* Scrollable area = widget height minus header minus toggle bar */
    int visH      = CalcWidgetH();
    int scrollVis = visH - HDR_H - ToggleBarH();
    int maxS      = CardsContentH() - scrollVis;
    if (maxS < 0) maxS = 0;
    if (g_scrollY > maxS) g_scrollY = maxS;
    if (g_scrollY < 0)    g_scrollY = 0;
}

static void PositionWidget(void)
{
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int wh = CalcWidgetH();
    ClampScroll();
    int x  = work.right  - WW - DPS(12);
    int y  = work.bottom - wh - DPS(8);
    if (x < work.left) x = work.left + DPS(12);
    if (y < work.top)  y = work.top  + DPS(8);
    SetWindowPos(g_hWid, HWND_TOPMOST, x, y, WW, wh, SWP_NOACTIVATE);
}

static void HideWidget(void)
{
    if (!g_vis) return;
    KillTimer(g_hWid, TIMER_TPUT);
    ShowWindow(g_hWid, SW_HIDE);
    g_vis     = FALSE;
    g_btnHover = FALSE;
}

static void ShowWidget(void)
{
    RefreshAdapters();
    g_scrollY = 0;
    /* Prime the throughput counters so the first tick produces a real delta */
    g_lastSampleMs = 0;
    for (int i = 0; i < g_nAd; i++) {
        Adapter *a = &g_ad[i];
        if (a->state != STATE_UP) continue;
        MIB_IF_ROW2 row = {0};
        row.InterfaceLuid = a->luid;
        if (GetIfEntry2(&row) == NO_ERROR) {
            a->inBytes  = row.InOctets;
            a->outBytes = row.OutOctets;
        }
    }
    PositionWidget();
    ShowWindow(g_hWid, SW_SHOWNOACTIVATE);   /* never steal focus */
    InvalidateRect(g_hWid, NULL, TRUE);
    SetTimer(g_hWid, TIMER_TPUT, 1000, NULL);
    FetchPublicIpAsync();
    g_vis = TRUE;
}

static void ToggleWidget(void)
{
    if (g_vis) HideWidget();
    else       ShowWidget();
}

/* ══════════════════════════════════════════════════════════════════════════
   PAINTING
   ══════════════════════════════════════════════════════════════════════════ */
static HFONT MakeFont(int pt10, int weight, const WCHAR *face)
{
    /* Scale by the same g_dpi the layout uses so fonts and geometry stay in sync. */
    int h = -MulDiv(pt10, g_dpi, 720);
    return CreateFontW(h, 0, 0, 0, weight, 0, 0, 0,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

static const WCHAR *TypeStr(DWORD t)
{
    switch (t) {
        case IF_TYPE_ETHERNET_CSMACD: return L"Ethernet";
        case IF_TYPE_IEEE80211:       return L"Wi-Fi";
        case IF_TYPE_PPP:             return L"PPP";
        case IF_TYPE_TUNNEL:          return L"Tunnel";
        case IF_TYPE_IEEE1394:        return L"FireWire";
        case IF_TYPE_ATM:             return L"ATM";
        default:                      return L"Network";
    }
}

/* Format a byte-rate as "1.2 MB/s", "340 KB/s", "72 B/s" */
static void FmtBps(UINT64 bps, WCHAR *out, int cap)
{
    if      (bps >= 1024ULL * 1024 * 1024)
        swprintf(out, cap, L"%.1f GB/s", bps / (1024.0 * 1024 * 1024));
    else if (bps >= 1024ULL * 1024)
        swprintf(out, cap, L"%.1f MB/s", bps / (1024.0 * 1024));
    else if (bps >= 1024ULL)
        swprintf(out, cap, L"%.0f KB/s", bps / 1024.0);
    else
        swprintf(out, cap, L"%llu B/s",  bps);
}

/* "↓ 1.2 MB/s  ↑ 340 KB/s" */
static void FmtThroughput(UINT64 rx, UINT64 tx, WCHAR *out, int cap)
{
    WCHAR r[24], t[24];
    FmtBps(rx, r, 24);
    FmtBps(tx, t, 24);
    swprintf(out, cap, L"↓ %s   ↑ %s", r, t);
}

/* Copy a UTF-16 string to the clipboard */
static void CopyToClipboard(const WCHAR *s)
{
    if (!s || !s[0]) return;
    if (!OpenClipboard(NULL)) return;
    EmptyClipboard();
    size_t n = (wcslen(s) + 1) * sizeof(WCHAR);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n);
    if (h) {
        WCHAR *p = (WCHAR *)GlobalLock(h);
        memcpy(p, s, n);
        GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
}

/* Sample current cumulative byte counters for every adapter and
   compute per-second throughput from the delta.  Called from the 1s timer. */
static void SampleThroughput(void)
{
    ULONGLONG now = GetTickCount64();
    double dt = g_lastSampleMs ? (now - g_lastSampleMs) / 1000.0 : 1.0;
    if (dt < 0.1) dt = 0.1;
    g_lastSampleMs = now;

    for (int i = 0; i < g_nAd; i++) {
        Adapter *a = &g_ad[i];
        if (a->state != STATE_UP) { a->rxBps = a->txBps = 0; continue; }

        MIB_IF_ROW2 row = {0};
        row.InterfaceLuid = a->luid;
        if (GetIfEntry2(&row) != NO_ERROR) continue;

        UINT64 in  = row.InOctets;
        UINT64 out = row.OutOctets;
        if (a->inBytes && in  >= a->inBytes)  a->rxBps = (UINT64)((in  - a->inBytes) / dt);
        if (a->outBytes && out >= a->outBytes) a->txBps = (UINT64)((out - a->outBytes) / dt);
        a->inBytes  = in;
        a->outBytes = out;
    }
}

/*
 * Draw a small connection-type icon (ICON_SZ x ICON_SZ) at (ix, iy).
 *
 * Ethernet : two small squares linked by a line  [■]──[■]
 * Wi-Fi    : three concentric top-half arcs + center dot
 * Other    : simple circle with crosshair
 */
static void DrawConnectionIcon(HDC dc, int ix, int iy, DWORD type, BOOL up, int signal)
{
    COLORREF col    = up ? C_UP : RGB(100, 100, 120);
    COLORREF dimCol = RGB(60, 62, 80);

    int pen = DPS(2); if (pen < 1) pen = 1;

    if (type == IF_TYPE_IEEE80211) {
        int cx = ix + ICON_SZ / 2;
        int cy = iy + ICON_SZ - DPS(2);
        int rO = DPS(9), rM = DPS(6), rI = DPS(3);

        /* Number of "lit" arcs based on signal strength (0-3).
           Unlit arcs drawn in dim colour so the icon stays readable. */
        int lit = !up ? 0 :
                  (signal >= 66 ? 3 :
                   signal >= 33 ? 2 :
                   signal >  0  ? 1 : 3);   /* no signal info → show all */

        HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));

        /* outer arc */
        HPEN pOut = CreatePen(PS_SOLID, pen, lit >= 3 ? col : dimCol);
        HPEN op   = (HPEN)SelectObject(dc, pOut);
        Arc(dc, cx-rO, cy-rO, cx+rO, cy+rO,  cx+rO, cy,  cx-rO, cy);
        SelectObject(dc, op);
        DeleteObject(pOut);

        /* middle arc */
        HPEN pMid = CreatePen(PS_SOLID, pen, lit >= 2 ? col : dimCol);
        op = (HPEN)SelectObject(dc, pMid);
        Arc(dc, cx-rM, cy-rM, cx+rM, cy+rM,  cx+rM, cy,  cx-rM, cy);
        SelectObject(dc, op);
        DeleteObject(pMid);

        /* inner arc */
        HPEN pIn = CreatePen(PS_SOLID, pen, lit >= 1 ? col : dimCol);
        op = (HPEN)SelectObject(dc, pIn);
        Arc(dc, cx-rI, cy-rI, cx+rI, cy+rI,  cx+rI, cy,  cx-rI, cy);
        SelectObject(dc, op);
        DeleteObject(pIn);

        /* filled dot at anchor */
        HBRUSH dotBr = CreateSolidBrush(col);
        SelectObject(dc, dotBr);
        HPEN np = (HPEN)GetStockObject(NULL_PEN);
        SelectObject(dc, np);
        Ellipse(dc, cx-DPS(2), cy-DPS(2), cx+DPS(3), cy+DPS(3));
        DeleteObject(dotBr);

        SelectObject(dc, ob);
    }
    else if (type == IF_TYPE_ETHERNET_CSMACD) {
        /* Ethernet: [■]──[■]  two small squares joined by a horizontal line */
        HPEN p1  = CreatePen(PS_SOLID, DPS(1) < 1 ? 1 : DPS(1), col);
        HPEN op  = (HPEN)SelectObject(dc, p1);
        HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));

        int cy = iy + ICON_SZ / 2;   /* vertical centre */

        /* left node */
        Rectangle(dc, ix + DPS(1), cy - DPS(4), ix + DPS(6), cy + DPS(4));
        /* connecting line */
        MoveToEx(dc, ix + DPS(6), cy, NULL);
        LineTo(dc, ix + DPS(12), cy);
        /* right node */
        Rectangle(dc, ix + DPS(12), cy - DPS(4), ix + DPS(17), cy + DPS(4));

        /* small tick marks on nodes (port indicators) */
        MoveToEx(dc, ix + DPS(3),  cy - DPS(1), NULL); LineTo(dc, ix + DPS(3),  cy + DPS(1));
        MoveToEx(dc, ix + DPS(14), cy - DPS(1), NULL); LineTo(dc, ix + DPS(14), cy + DPS(1));

        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(p1);
    }
    else {
        /* Generic: small globe (circle + horizontal equator) */
        HPEN p1  = CreatePen(PS_SOLID, DPS(1) < 1 ? 1 : DPS(1), col);
        HPEN op  = (HPEN)SelectObject(dc, p1);
        HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));

        int cy = iy + ICON_SZ / 2;
        Ellipse(dc, ix + DPS(2), iy + DPS(2), ix + ICON_SZ - DPS(2), iy + ICON_SZ - DPS(2));
        MoveToEx(dc, ix + DPS(2), cy, NULL);
        LineTo(dc, ix + ICON_SZ - DPS(2), cy);

        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(p1);
    }
}

static void PaintWidget(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;

    /* ── off-screen buffer (eliminates flicker) ─────────────────────────── */
    HDC     mdc  = CreateCompatibleDC(hdc);
    HBITMAP mbmp = CreateCompatibleBitmap(hdc, W, H);
    HBITMAP obmp = (HBITMAP)SelectObject(mdc, mbmp);

    SetBkMode(mdc, TRANSPARENT);

    HBRUSH bgBr = CreateSolidBrush(C_BG);
    FillRect(mdc, &rc, bgBr);
    DeleteObject(bgBr);

    /* ── fonts ──────────────────────────────────────────────────────────── */
    HFONT fHdr  = MakeFont(115, FW_BOLD,  L"Segoe UI");
    HFONT fName = MakeFont(105, 600,      L"Segoe UI");  /* 600 = SemiBold */
    HFONT fSub  = MakeFont( 88, FW_NORMAL,L"Segoe UI");
    HFONT fScan = MakeFont( 78, FW_NORMAL,L"Segoe UI");  /* BSSID / detail line in scan list */
    HFONT fIcon = MakeFont( 80, FW_NORMAL,L"Segoe MDL2 Assets");  /* lock glyph for secured nets */

    /* ── header bar ─────────────────────────────────────────────────────── */
    RECT hdrRc = {0, 0, W, HDR_H};
    HBRUSH hdrBr = CreateSolidBrush(C_CARD);
    FillRect(mdc, &hdrRc, hdrBr);
    DeleteObject(hdrBr);

    HPEN ep  = CreatePen(PS_SOLID, 1, C_EDGE);
    HPEN oep = (HPEN)SelectObject(mdc, ep);
    MoveToEx(mdc, 0, HDR_H - 1, NULL);
    LineTo(mdc, W, HDR_H - 1);
    SelectObject(mdc, oep);
    DeleteObject(ep);

    SelectObject(mdc, fHdr);
    SetTextColor(mdc, C_HDR);
    /* title on first half of header */
    RECT htRc = {HPAD + DPS(4), DPS(4), W - DPS(90), DPS(26)};
    DrawTextW(mdc, L"Network Monitor", -1, &htRc, DT_VCENTER | DT_SINGLELINE);

    /* Public IP subtitle on second half of header */
    SelectObject(mdc, fSub);
    SetTextColor(mdc, C_SUB);
    WCHAR pubStr[80];
    if (g_publicIp[0])
        swprintf(pubStr, 80, L"Public IP:  %s", g_publicIp);
    else
        wcscpy(pubStr, L"Public IP:  …");
    RECT puRc = {HPAD + DPS(4), DPS(28), W - DPS(12), HDR_H - DPS(4)};
    DrawTextW(mdc, pubStr, -1, &puRc, DT_VCENTER | DT_SINGLELINE);

    /* X/Y UP badge — sits just left of the minimize button.
       Only counts real (non-disabled) adapters. */
    int up = 0, visible = 0;
    for (int i = 0; i < g_nAd; i++) {
        if (g_ad[i].state == STATE_UP)            up++;
        if (g_ad[i].state != STATE_DISABLED) visible++;
    }
    WCHAR badge[32];
    swprintf(badge, 32, L"%d / %d UP", up, visible);
    SelectObject(mdc, fSub);
    SetTextColor(mdc, up == visible ? C_UP : (up == 0 ? C_DOWN : C_SUB));
    RECT bdRc = {0, DPS(4), W - DPS(8) - BTN_SZ - DPS(8), DPS(26)};
    DrawTextW(mdc, badge, -1, &bdRc, DT_VCENTER | DT_SINGLELINE | DT_RIGHT);

    /* ── minimize (↓) button ────────────────────────────────────────────── */
    int bx = W - DPS(8) - BTN_SZ;
    int by = (DPS(30) - BTN_SZ) / 2 + DPS(1);

    /* button background */
    COLORREF btnBg = g_btnHover ? RGB(70, 74, 100) : RGB(38, 40, 58);
    HBRUSH btnBr = CreateSolidBrush(btnBg);
    HBRUSH oBtnBr = (HBRUSH)SelectObject(mdc, btnBr);
    HPEN btnPen = CreatePen(PS_SOLID, 1, g_btnHover ? RGB(100, 106, 140) : C_EDGE);
    HPEN oBtnPen = (HPEN)SelectObject(mdc, btnPen);
    RoundRect(mdc, bx, by, bx + BTN_SZ, by + BTN_SZ, DPS(6), DPS(6));
    SelectObject(mdc, oBtnBr);
    SelectObject(mdc, oBtnPen);
    DeleteObject(btnBr);
    DeleteObject(btnPen);

    /* chevron ∨ — two lines meeting at bottom-center of button */
    COLORREF chevCol = g_btnHover ? C_TXT : C_SUB;
    HPEN chvPen = CreatePen(PS_SOLID, DPS(2) < 1 ? 1 : DPS(2), chevCol);
    HPEN oChv   = (HPEN)SelectObject(mdc, chvPen);
    int cx = bx + BTN_SZ / 2;
    int cy = by + BTN_SZ / 2;
    MoveToEx(mdc, cx - DPS(6), cy - DPS(3), NULL);
    LineTo  (mdc, cx,          cy + DPS(4));
    LineTo  (mdc, cx + DPS(6), cy - DPS(3));
    SelectObject(mdc, oChv);
    DeleteObject(chvPen);

    /* ── adapter cards (scrollable area between header and toggle) ───── */
    int clipBottom = H - ToggleBarH();
    HRGN clipRgn = CreateRectRgn(0, HDR_H, W, clipBottom);
    SelectClipRgn(mdc, clipRgn);

    int y = HDR_H + CARD_GAP - g_scrollY;

    if (g_nAd == 0) {
        SelectObject(mdc, fSub);
        SetTextColor(mdc, C_SUB);
        RECT nr = {0, HDR_H, W, H};
        DrawTextW(mdc, L"No network adapters found.", -1, &nr,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    for (int i = 0; i < g_nAd; i++) {
        const Adapter *a = &g_ad[i];

        /* skip disabled adapters unless the toggle is on */
        if (a->state == STATE_DISABLED && !g_showDisabled) continue;

        BOOL dimmed = (a->state == STATE_DISABLED);

        /* dimmed cards use a slightly different background */
        COLORREF cardBgCol = dimmed ? RGB(19, 19, 27) : C_CARD;
        COLORREF edgeCol   = dimmed ? RGB(36, 37, 52) : C_EDGE;

        RECT cr = {HPAD, y, W - HPAD, y + CardTotalH(a)};

        /* card fill */
        HBRUSH cardBr = CreateSolidBrush(cardBgCol);
        HBRUSH oBr    = (HBRUSH)SelectObject(mdc, cardBr);
        HPEN   nullP  = (HPEN)GetStockObject(NULL_PEN);
        HPEN   oP     = (HPEN)SelectObject(mdc, nullP);
        RoundRect(mdc, cr.left, cr.top, cr.right, cr.bottom, DPS(10), DPS(10));
        SelectObject(mdc, oP);
        SelectObject(mdc, oBr);
        DeleteObject(cardBr);

        /* card border */
        HPEN   bdPen = CreatePen(PS_SOLID, 1, edgeCol);
        HBRUSH nbBr  = (HBRUSH)GetStockObject(NULL_BRUSH);
        oP  = (HPEN)SelectObject(mdc, bdPen);
        oBr = (HBRUSH)SelectObject(mdc, nbBr);
        RoundRect(mdc, cr.left, cr.top, cr.right, cr.bottom, DPS(10), DPS(10));
        SelectObject(mdc, oP);
        SelectObject(mdc, oBr);
        DeleteObject(bdPen);

        /* left status stripe: green / red / grey-for-disabled */
        COLORREF stripeCol = (a->state == STATE_UP)       ? C_UP   :
                             (a->state == STATE_DOWN)     ? C_DOWN :
                                                            RGB(60, 62, 80);
        HBRUSH stBr = CreateSolidBrush(stripeCol);
        RECT   stRc = {cr.left, cr.top + DPS(10), cr.left + DPS(3), cr.bottom - DPS(10)};
        FillRect(mdc, &stRc, stBr);
        DeleteObject(stBr);

        /* text colours adjusted for disabled (dimmed) cards */
        COLORREF colTxt = dimmed ? RGB(80, 82, 100) : C_TXT;
        COLORREF colSub = dimmed ? RGB(60, 62, 80)  : C_SUB;

        /* ── row 1: connection icon + name + status pill ─────────────── */
        int iconX = cr.left + DPS(10);
        int iconY = cr.top  + DPS(8);
        DrawConnectionIcon(mdc, iconX, iconY, a->type, a->state == STATE_UP, a->signal);

        SelectObject(mdc, fName);
        SetTextColor(mdc, colTxt);
        RECT nr = {cr.left + DPS(10) + ICON_SZ + DPS(6), cr.top + DPS(9),
                   cr.right - DPS(106),                  cr.top + DPS(27)};
        DrawTextW(mdc, a->name, -1, &nr, DT_SINGLELINE | DT_END_ELLIPSIS);

        /* status pill badge */
        const WCHAR *stStr = (a->state == STATE_UP)       ? L"CONNECTED"    :
                             (a->state == STATE_DOWN)     ? L"DISCONNECTED" :
                                                            L"DISABLED";
        SelectObject(mdc, fSub);
        SIZE stSz; GetTextExtentPoint32W(mdc, stStr, (int)wcslen(stStr), &stSz);
        int pillW = stSz.cx + DPS(12);
        int pillX = cr.right - DPS(8) - pillW;
        int pillY = cr.top + DPS(11);
        COLORREF pillBg = (a->state == STATE_UP)   ? RGB(20, 60, 35)  :
                          (a->state == STATE_DOWN) ? RGB(65, 22, 22)  :
                                                     RGB(35, 36, 48);
        HBRUSH pillBr = CreateSolidBrush(pillBg);
        HBRUSH oPill  = (HBRUSH)SelectObject(mdc, pillBr);
        HPEN pillPen  = CreatePen(PS_SOLID, 1, stripeCol);
        HPEN oPillPen = (HPEN)SelectObject(mdc, pillPen);
        RoundRect(mdc, pillX, pillY, pillX + pillW, pillY + DPS(16), DPS(8), DPS(8));
        SelectObject(mdc, oPill);
        SelectObject(mdc, oPillPen);
        DeleteObject(pillBr);
        DeleteObject(pillPen);
        SetTextColor(mdc, stripeCol);
        RECT sr = {pillX, pillY, pillX + pillW, pillY + DPS(16)};
        DrawTextW(mdc, stStr, -1, &sr, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

        /* ── row 2: Wi-Fi SSID + signal, or type + description ───────── */
        WCHAR row2[220];
        if (a->type == IF_TYPE_IEEE80211 && a->state == STATE_UP && a->ssid[0]) {
            const WCHAR *band = L"";
            if      (a->channel >= 1   && a->channel <= 14)  band = L"2.4 GHz";
            else if (a->channel >= 32  && a->channel <= 177) band = L"5 GHz";
            else if (a->channel >= 1   && a->channel <= 233 && a->channel > 177) band = L"6 GHz";
            if (band[0])
                swprintf(row2, 220, L"Wi-Fi  ·  %s  ·  %s  ·  %d%%",
                         a->ssid, band, a->signal);
            else
                swprintf(row2, 220, L"Wi-Fi  ·  %s  ·  %d%%",
                         a->ssid, a->signal);
            SetTextColor(mdc, dimmed ? colSub : C_HDR);
        } else {
            swprintf(row2, 220, L"%s  ·  %s", TypeStr(a->type), a->desc);
            SetTextColor(mdc, colSub);
        }
        RECT r2 = {cr.left + DPS(14), cr.top + DPS(31), cr.right - DPS(8), cr.top + DPS(45)};
        DrawTextW(mdc, row2, -1, &r2, DT_SINGLELINE | DT_END_ELLIPSIS);

        /* ── row 3: IPv4  |  DHCP / Static ──────────────────────────── */
        WCHAR ipStr[64];
        if (a->ipv4[0] && a->prefix > 0)
            swprintf(ipStr, 64, L"IPv4:  %s /%d", a->ipv4, a->prefix);
        else
            swprintf(ipStr, 64, L"IPv4:  %s", a->ipv4[0] ? a->ipv4 : L"\u2014");
        SetTextColor(mdc, colSub);
        RECT r3l = {cr.left + DPS(14), cr.top + DPS(49), cr.left + DPS(250), cr.top + DPS(63)};
        DrawTextW(mdc, ipStr, -1, &r3l, DT_SINGLELINE | DT_END_ELLIPSIS);

        const WCHAR *ipMode = a->dhcp ? L"DHCP" : L"Static";
        SetTextColor(mdc, dimmed ? colSub : (a->dhcp ? C_DHCP : C_STAT));
        RECT r3r = {cr.left + DPS(248), cr.top + DPS(49), cr.right - DPS(8), cr.top + DPS(63)};
        DrawTextW(mdc, ipMode, -1, &r3r, DT_SINGLELINE | DT_RIGHT);

        /* ── row 4: Subnet mask  |  Speed ────────────────────────────── */
        SetTextColor(mdc, colSub);
        WCHAR maskStr[48];
        swprintf(maskStr, 48, L"Mask:  %s", a->mask[0] ? a->mask : L"\u2014");
        RECT r4l = {cr.left + DPS(14), cr.top + DPS(66), cr.left + DPS(220), cr.top + DPS(80)};
        DrawTextW(mdc, maskStr, -1, &r4l, DT_SINGLELINE | DT_END_ELLIPSIS);

        WCHAR spStr[48];
        swprintf(spStr, 48, L"%s", a->speed);
        RECT r4r = {cr.left + DPS(218), cr.top + DPS(66), cr.right - DPS(8), cr.top + DPS(80)};
        DrawTextW(mdc, spStr, -1, &r4r, DT_SINGLELINE | DT_RIGHT);

        /* ── row 5: Gateway  |  live throughput ────────────────────── */
        SetTextColor(mdc, colSub);
        WCHAR gwStr[64];
        swprintf(gwStr, 64, L"GW:    %s", a->gw[0] ? a->gw : L"—");
        RECT r5l = {cr.left + DPS(14), cr.top + DPS(83), cr.left + DPS(200), cr.top + DPS(97)};
        DrawTextW(mdc, gwStr, -1, &r5l, DT_SINGLELINE | DT_END_ELLIPSIS);

        if (a->state == STATE_UP && (a->rxBps || a->txBps)) {
            WCHAR tputStr[64];
            FmtThroughput(a->rxBps, a->txBps, tputStr, 64);
            SetTextColor(mdc, dimmed ? colSub : C_DHCP);
            RECT r5r = {cr.left + DPS(198), cr.top + DPS(83), cr.right - DPS(8), cr.top + DPS(97)};
            DrawTextW(mdc, tputStr, -1, &r5r, DT_SINGLELINE | DT_RIGHT);
        }

        /* ── row 6: DNS ─────────────────────────────────────────────── */
        SetTextColor(mdc, colSub);
        WCHAR dnsStr[120];
        if (a->dns1[0] && a->dns2[0])
            swprintf(dnsStr, 120, L"DNS:   %s,  %s", a->dns1, a->dns2);
        else if (a->dns1[0])
            swprintf(dnsStr, 120, L"DNS:   %s", a->dns1);
        else
            wcscpy(dnsStr, L"DNS:   —");
        RECT r6 = {cr.left + DPS(14), cr.top + DPS(100), cr.right - DPS(8), cr.top + DPS(114)};
        DrawTextW(mdc, dnsStr, -1, &r6, DT_SINGLELINE | DT_END_ELLIPSIS);

        /* ── row 7: MAC (left)  |  IPv6 (right, truncated) ─────────── */
        WCHAR macStr[36];
        swprintf(macStr, 36, L"MAC:   %s", a->mac);
        RECT r7l = {cr.left + DPS(14), cr.top + DPS(117), cr.left + DPS(190), cr.top + DPS(131)};
        DrawTextW(mdc, macStr, -1, &r7l, DT_SINGLELINE | DT_END_ELLIPSIS);

        WCHAR ip6Str[90];
        swprintf(ip6Str, 90, L"IPv6:  %s", a->ipv6[0] ? a->ipv6 : L"—");
        RECT r7r = {cr.left + DPS(185), cr.top + DPS(117), cr.right - DPS(8), cr.top + DPS(131)};
        DrawTextW(mdc, ip6Str, -1, &r7r, DT_SINGLELINE | DT_RIGHT | DT_END_ELLIPSIS);

        /* ── Wi-Fi scanner: toggle bar + collapsible results dropdown ──── */
        if (IsScanCard(a)) {
            int barY = cr.top + CARD_H;

            /* separator above the bar */
            HPEN sep  = CreatePen(PS_SOLID, 1, edgeCol);
            HPEN oSep = (HPEN)SelectObject(mdc, sep);
            MoveToEx(mdc, cr.left + DPS(10), barY, NULL);
            LineTo  (mdc, cr.right - DPS(10), barY);
            SelectObject(mdc, oSep);
            DeleteObject(sep);

            /* expand/collapse chevron */
            COLORREF barCol = g_connState == 2 ? C_DOWN : (g_scanHover ? C_TXT : C_HDR);
            HPEN chv  = CreatePen(PS_SOLID, DPS(2) < 1 ? 1 : DPS(2), barCol);
            HPEN oChv = (HPEN)SelectObject(mdc, chv);
            int cxx = cr.left + DPS(18), cyy = barY + SCAN_BAR_H / 2;
            if (g_scanOpen) {                       /* ▾ */
                MoveToEx(mdc, cxx - DPS(4), cyy - DPS(2), NULL);
                LineTo  (mdc, cxx,          cyy + DPS(3));
                LineTo  (mdc, cxx + DPS(4), cyy - DPS(2));
            } else {                                /* ▸ */
                MoveToEx(mdc, cxx - DPS(2), cyy - DPS(4), NULL);
                LineTo  (mdc, cxx + DPS(3), cyy);
                LineTo  (mdc, cxx - DPS(2), cyy + DPS(4));
            }
            SelectObject(mdc, oChv);
            DeleteObject(chv);

            /* bar label — connect status takes priority over scan state */
            WCHAR barTxt[80];
            if      (g_connState == 1) swprintf(barTxt, 80, L"Connecting to %s…", g_connSsid);
            else if (g_connState == 2) swprintf(barTxt, 80, L"Couldn't connect to %s", g_connSsid);
            else if (g_scanning)       wcscpy(barTxt, L"Scanning…");
            else if (g_scanOpen)       swprintf(barTxt, 80, L"Nearby networks  (%d)", g_scanCount);
            else                       wcscpy(barTxt, L"Scan nearby networks");
            SelectObject(mdc, fSub);
            SetTextColor(mdc, barCol);
            RECT btr = {cr.left + DPS(30), barY, cr.right - DPS(10), barY + SCAN_BAR_H};
            DrawTextW(mdc, barTxt, -1, &btr, DT_SINGLELINE | DT_VCENTER);

            /* results dropdown */
            if (g_scanOpen && !(g_scanning && g_scanCount == 0)) {
                int ly = barY + SCAN_BAR_H + SCAN_PAD;
                if (g_scanCount == 0) {
                    SelectObject(mdc, fSub);
                    SetTextColor(mdc, colSub);
                    RECT er = {cr.left + DPS(16), ly, cr.right - DPS(10), ly + SCAN_ROW_H};
                    DrawTextW(mdc, L"No networks found.", -1, &er, DT_SINGLELINE | DT_VCENTER);
                } else {
                    for (int s = 0; s < g_scanCount; s++) {
                        ScanNet *n = &g_scan[s];
                        int ry = ly + s * SCAN_ROW_H;

                        /* hover / pressed highlight behind the row */
                        if (s == g_scanRowPress || s == g_scanRowHover) {
                            COLORREF hl = (s == g_scanRowPress) ? RGB(52, 56, 82) : RGB(36, 38, 55);
                            HBRUSH hb  = CreateSolidBrush(hl);
                            HBRUSH ohb = (HBRUSH)SelectObject(mdc, hb);
                            HPEN   ohp = (HPEN)SelectObject(mdc, GetStockObject(NULL_PEN));
                            RoundRect(mdc, cr.left + DPS(6), ry - DPS(2),
                                      cr.right - DPS(6), ry + SCAN_ROW_H - DPS(4), DPS(6), DPS(6));
                            SelectObject(mdc, ohp);
                            SelectObject(mdc, ohb);
                            DeleteObject(hb);
                        }

                        /* line 1: [lock] SSID [✓ if connected]  |  band · dBm */
                        int tx = cr.left + DPS(16);
                        if (n->sec != SEC_OPEN) {
                            SelectObject(mdc, fIcon);
                            SetTextColor(mdc, colSub);
                            RECT lk = {tx, ry, tx + DPS(16), ry + DPS(15)};
                            DrawTextW(mdc, L"", -1, &lk, DT_SINGLELINE | DT_VCENTER);  /* MDL2 lock */
                            tx += DPS(17);
                        }
                        BOOL isConn = (a->ssid[0] && wcscmp(n->ssid, a->ssid) == 0);
                        SelectObject(mdc, fSub);
                        SetTextColor(mdc, isConn ? C_UP : C_TXT);
                        WCHAR nm[44];
                        if      (isConn)     swprintf(nm, 44, L"%s  ✓", n->ssid);   /* ✓ connected */
                        else if (n->ssid[0]) { wcsncpy(nm, n->ssid, 43); nm[43] = 0; }
                        else                 wcscpy(nm, L"(hidden network)");
                        RECT l1 = {tx, ry, cr.left + DPS(232), ry + DPS(15)};
                        DrawTextW(mdc, nm, -1, &l1, DT_SINGLELINE | DT_END_ELLIPSIS);

                        COLORREF sigCol = n->rssi >= -60 ? C_UP : n->rssi >= -75 ? C_STAT : C_DOWN;
                        WCHAR sig[40];
                        swprintf(sig, 40, L"%s · %d dBm", BandStr(n->band), n->rssi);
                        SetTextColor(mdc, sigCol);
                        RECT s1 = {cr.left + DPS(230), ry, cr.right - DPS(12), ry + DPS(15)};
                        DrawTextW(mdc, sig, -1, &s1, DT_SINGLELINE | DT_RIGHT);

                        /* line 2: BSSID  |  ch · quality */
                        SelectObject(mdc, fScan);
                        SetTextColor(mdc, colSub);
                        WCHAR bss[20]; FmtBssid(n->bssid, bss, 20);
                        RECT l2 = {cr.left + DPS(16), ry + DPS(15), cr.left + DPS(232), ry + DPS(29)};
                        DrawTextW(mdc, bss, -1, &l2, DT_SINGLELINE);

                        WCHAR meta[48];
                        const WCHAR *secL = (n->sec == SEC_SAE) ? L" · WPA3" : L"";
                        if (n->channel > 0) swprintf(meta, 48, L"ch %d · %d%%%s", n->channel, n->quality, secL);
                        else                swprintf(meta, 48, L"%d%%%s", n->quality, secL);
                        RECT s2 = {cr.left + DPS(230), ry + DPS(15), cr.right - DPS(12), ry + DPS(29)};
                        DrawTextW(mdc, meta, -1, &s2, DT_SINGLELINE | DT_RIGHT);
                    }
                }
            }
        }

        y += CardTotalH(a) + CARD_GAP;
    }

    /* Remove clip region before drawing the fixed toggle button */
    SelectClipRgn(mdc, NULL);
    DeleteObject(clipRgn);

    /* ── show/hide disabled toggle button — fixed at bottom of widget ── */
    if (g_nDisabled > 0) {
        RECT togRc = {HPAD, H - BPAD - TOGGLE_H, W - HPAD, H - BPAD};

        /* background fill behind toggle to cover any scrolled cards */
        HBRUSH bgFill = CreateSolidBrush(C_BG);
        RECT bgRc = {0, H - ToggleBarH(), W, H};
        FillRect(mdc, &bgRc, bgFill);
        DeleteObject(bgFill);

        COLORREF togBg = g_togHover ? RGB(38, 42, 62) : RGB(22, 24, 36);
        COLORREF togBd = g_togHover ? RGB(80, 86, 120) : RGB(40, 42, 60);

        HBRUSH togBr = CreateSolidBrush(togBg);
        HPEN   togPn = CreatePen(PS_SOLID, 1, togBd);
        HBRUSH oTBr  = (HBRUSH)SelectObject(mdc, togBr);
        HPEN   oTPn  = (HPEN)SelectObject(mdc, togPn);
        RoundRect(mdc, togRc.left, togRc.top, togRc.right, togRc.bottom, DPS(6), DPS(6));
        SelectObject(mdc, oTBr);
        SelectObject(mdc, oTPn);
        DeleteObject(togBr);
        DeleteObject(togPn);

        /* chevron arrow */
        COLORREF arrowCol = g_togHover ? C_TXT : C_SUB;
        HPEN arrowPen = CreatePen(PS_SOLID, DPS(2) < 1 ? 1 : DPS(2), arrowCol);
        HPEN oAP = (HPEN)SelectObject(mdc, arrowPen);
        int ax = togRc.left + DPS(18);
        int ay = togRc.top  + (TOGGLE_H / 2);
        if (g_showDisabled) {
            MoveToEx(mdc, ax - DPS(5), ay + DPS(3), NULL); LineTo(mdc, ax,          ay - DPS(3));
            LineTo  (mdc, ax + DPS(5), ay + DPS(3));
        } else {
            MoveToEx(mdc, ax - DPS(5), ay - DPS(3), NULL); LineTo(mdc, ax,          ay + DPS(3));
            LineTo  (mdc, ax + DPS(5), ay - DPS(3));
        }
        SelectObject(mdc, oAP);
        DeleteObject(arrowPen);

        /* label text */
        SelectObject(mdc, fSub);
        SetTextColor(mdc, g_togHover ? C_TXT : C_SUB);
        WCHAR togLabel[48];
        if (g_showDisabled)
            wcscpy(togLabel, L"Hide disabled");
        else
            swprintf(togLabel, 48, L"Show disabled  (%d)", g_nDisabled);
        RECT togTxt = {togRc.left + DPS(32), togRc.top, togRc.right - DPS(8), togRc.bottom};
        DrawTextW(mdc, togLabel, -1, &togTxt, DT_VCENTER | DT_SINGLELINE);
    }

    /* Deselect our fonts (restore a stock font) so DeleteObject can actually
       free them — DeleteObject fails on an object still selected into a DC. */
    SelectObject(mdc, GetStockObject(SYSTEM_FONT));
    DeleteObject(fHdr);
    DeleteObject(fName);
    DeleteObject(fSub);
    DeleteObject(fScan);
    DeleteObject(fIcon);

    BitBlt(hdc, 0, 0, W, H, mdc, 0, 0, SRCCOPY);
    SelectObject(mdc, obmp);
    DeleteObject(mbmp);
    DeleteDC(mdc);

    EndPaint(hwnd, &ps);
}

/* ══════════════════════════════════════════════════════════════════════════
   NETWORK CHANGE CALLBACK  (OS thread-pool — must not block)
   ══════════════════════════════════════════════════════════════════════════ */
static VOID WINAPI OnNetChange(PVOID ctx, PMIB_IPINTERFACE_ROW row,
                               MIB_NOTIFICATION_TYPE type)
{
    (void)ctx; (void)row; (void)type;
    PostMessageW(g_hMain, WM_NETCHANGED, 0, 0);
}

/* ══════════════════════════════════════════════════════════════════════════
   WINDOW PROCEDURES
   ══════════════════════════════════════════════════════════════════════════ */
/* Returns the minimize-button rect in client coordinates */
static RECT GetBtnRect(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    int bx = rc.right - DPS(8) - BTN_SZ;
    int by = (DPS(30) - BTN_SZ) / 2 + DPS(1);
    RECT btn = {bx, by, bx + BTN_SZ, by + BTN_SZ};
    return btn;
}

/* Returns the toggle button rect — fixed at the bottom of the widget */
static RECT GetToggleRect(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    RECT btn = {HPAD, rc.bottom - BPAD - TOGGLE_H,
                rc.right - HPAD, rc.bottom - BPAD};
    return btn;
}

/* Rect of the Wi-Fi "scan" bar in client coords, if that card is laid out.
   Walks the cards (variable height) the same way the painter does. */
static BOOL GetScanBarRect(HWND hwnd, RECT *out)
{
    if (!g_vis) return FALSE;
    RECT rc; GetClientRect(hwnd, &rc);
    int y = HDR_H + CARD_GAP - g_scrollY;
    for (int i = 0; i < g_nAd; i++) {
        if (g_ad[i].state == STATE_DISABLED && !g_showDisabled) continue;
        const Adapter *a = &g_ad[i];
        if (IsScanCard(a)) {
            int barTop = y + CARD_H;
            out->left   = HPAD;
            out->top    = barTop;
            out->right  = rc.right - HPAD;
            out->bottom = barTop + SCAN_BAR_H;
            return TRUE;
        }
        y += CardTotalH(a) + CARD_GAP;
    }
    return FALSE;
}

/* Index of the scan-result row under client point pt, or -1. Walks cards the
   same way the painter does so it stays in sync with variable heights. */
static int ScanRowAt(HWND hwnd, POINT pt)
{
    if (!g_scanOpen || g_scanCount <= 0) return -1;   /* mouse msgs only arrive when visible */
    RECT rc; GetClientRect(hwnd, &rc);
    if (pt.x < HPAD || pt.x > rc.right - HPAD) return -1;
    int y = HDR_H + CARD_GAP - g_scrollY;
    for (int i = 0; i < g_nAd; i++) {
        if (g_ad[i].state == STATE_DISABLED && !g_showDisabled) continue;
        const Adapter *a = &g_ad[i];
        if (IsScanCard(a)) {
            int ly = y + CARD_H + SCAN_BAR_H + SCAN_PAD;
            if (pt.y < ly) return -1;
            int row = (pt.y - ly) / SCAN_ROW_H;
            return (row >= 0 && row < g_scanCount && pt.y < ly + g_scanCount * SCAN_ROW_H) ? row : -1;
        }
        y += CardTotalH(a) + CARD_GAP;
    }
    return -1;
}

static LRESULT CALLBACK WidgetProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_PAINT:
            PaintWidget(hwnd);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        /* Monitor / scale changed → rescale all geometry + fonts, reposition */
        case WM_DPICHANGED:
            g_dpi = HIWORD(wp);
            if (g_vis) { PositionWidget(); InvalidateRect(hwnd, NULL, TRUE); }
            return 0;

        /* 1 Hz throughput sampler (runs only while widget is visible) */
        case WM_TIMER:
            if (wp == TIMER_TPUT) {
                SampleThroughput();
                InvalidateRect(hwnd, NULL, FALSE);
            } else if (wp == TIMER_SCAN) {       /* scan-complete notif was missed */
                FetchScanResults();
                PositionWidget();
                InvalidateRect(hwnd, NULL, TRUE);
            }
            return 0;

        /* Public IP fetch completed → repaint header */
        case WM_APP + 1:
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;

        /* Wi-Fi scan completed → pull results, resize the card, repaint */
        case WM_SCANDONE:
            FetchScanResults();
            PositionWidget();
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;

        /* Wi-Fi connect attempt finished (wParam = WLAN reason code, 0 = success) */
        case WM_CONNDONE:
            if (g_connState != 1) return 0;          /* ignore events we didn't initiate */
            if (wp == 0 /* WLAN_REASON_CODE_SUCCESS */) {
                g_connState = 0;                     /* the adapter card + ✓ now reflect it */
                RefreshAdapters();
                UpdateTip();
            } else {
                g_connState = 2;                     /* show "couldn't connect" briefly */
                SetTimer(hwnd, TIMER_CONNCLR, 4000, NULL);
            }
            PositionWidget();
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;

        /* Mouse wheel scrolling */
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            g_scrollY -= delta / 2;
            ClampScroll();
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }

        /* Press a scan row → show the pressed highlight */
        case WM_LBUTTONDOWN: {
            POINT pt  = {(short)LOWORD(lp), (short)HIWORD(lp)};
            int   row = ScanRowAt(hwnd, pt);
            if (row >= 0) { g_scanRowPress = row; InvalidateRect(hwnd, NULL, FALSE); }
            return 0;
        }

        /* Click handling: minimize, toggle, connect to a scanned network, or field-copy */
        case WM_LBUTTONUP: {
            POINT pt   = {(short)LOWORD(lp), (short)HIWORD(lp)};
            if (g_scanRowPress >= 0) { g_scanRowPress = -1; InvalidateRect(hwnd, NULL, FALSE); }
            RECT  rBtn = GetBtnRect(hwnd);
            RECT  rTog = GetToggleRect(hwnd);
            if (PtInRect(&rBtn, pt)) {
                HideWidget();
                return 0;
            }
            if (g_nDisabled > 0 && PtInRect(&rTog, pt)) {
                g_showDisabled = !g_showDisabled;
                g_scrollY = 0;
                PositionWidget();
                InvalidateRect(hwnd, NULL, TRUE);
                return 0;
            }
            /* Card area: scan-bar toggle, scan-result row (copy BSSID), or field-copy.
               Cards are variable height, so walk them like the painter does. */
            if (pt.y >= HDR_H) {
                int yy = HDR_H + CARD_GAP - g_scrollY;
                for (int i = 0; i < g_nAd; i++) {
                    if (g_ad[i].state == STATE_DISABLED && !g_showDisabled) continue;
                    const Adapter *a = &g_ad[i];
                    int ch = CardTotalH(a);
                    if (pt.y >= yy && pt.y < yy + ch) {
                        int off = pt.y - yy;            /* offset within this card */

                        if (IsScanCard(a)) {
                            int barTop = CARD_H;
                            if (off >= barTop && off < barTop + SCAN_BAR_H) {
                                /* toggle the dropdown; opening triggers a fresh scan */
                                if (g_scanOpen) g_scanOpen = FALSE;
                                else { g_scanOpen = TRUE; StartWifiScan(&a->guid); }
                                PositionWidget();
                                InvalidateRect(hwnd, NULL, TRUE);
                                return 0;
                            }
                            if (g_scanOpen && g_scanCount > 0) {
                                int ly = barTop + SCAN_BAR_H + SCAN_PAD;
                                if (off >= ly) {
                                    int row = (off - ly) / SCAN_ROW_H;
                                    g_scanRowPress = -1;
                                    InvalidateRect(hwnd, NULL, FALSE);   /* release the pressed look */
                                    if (row >= 0 && row < g_scanCount)
                                        ConnectToScan(row);     /* click a network → connect to it */
                                    return 0;
                                }
                            }
                        }

                        if (off <= CARD_H) {
                            const WCHAR *copy = NULL;
                            if      (off >= DPS(31)  && off <= DPS(45))  copy = a->ssid[0] ? a->ssid : NULL;
                            else if (off >= DPS(49)  && off <= DPS(63))  copy = a->ipv4[0] ? a->ipv4 : NULL;
                            else if (off >= DPS(66)  && off <= DPS(80))  copy = a->mask[0] ? a->mask : NULL;
                            else if (off >= DPS(83)  && off <= DPS(97))  copy = a->gw[0]   ? a->gw   : NULL;
                            else if (off >= DPS(100) && off <= DPS(114)) copy = a->dns1[0] ? a->dns1 : NULL;
                            else if (off >= DPS(117) && off <= DPS(131)) {
                                RECT rc; GetClientRect(hwnd, &rc);
                                copy = (pt.x < rc.right / 2) ? a->mac : (a->ipv6[0] ? a->ipv6 : NULL);
                            }
                            if (copy) CopyToClipboard(copy);
                        }
                        return 0;
                    }
                    yy += ch + CARD_GAP;
                }
            }
            return 0;
        }

        /* Hover highlights for both buttons */
        case WM_MOUSEMOVE: {
            POINT pt     = {(short)LOWORD(lp), (short)HIWORD(lp)};
            RECT  rBtn   = GetBtnRect(hwnd);
            RECT  rTog   = GetToggleRect(hwnd);
            RECT  rScan;
            BOOL  hasScan = GetScanBarRect(hwnd, &rScan);
            BOOL  hBtn   = PtInRect(&rBtn, pt);
            BOOL  hTog   = g_nDisabled > 0 && PtInRect(&rTog, pt);
            BOOL  hScan  = hasScan && PtInRect(&rScan, pt);
            int   hRow   = ScanRowAt(hwnd, pt);
            if (hBtn != g_btnHover || hTog != g_togHover || hScan != g_scanHover || hRow != g_scanRowHover) {
                g_btnHover     = hBtn;
                g_togHover     = hTog;
                g_scanHover    = hScan;
                g_scanRowHover = hRow;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            return 0;
        }

        case WM_MOUSELEAVE:
            if (g_btnHover || g_togHover || g_scanHover || g_scanRowHover >= 0 || g_scanRowPress >= 0) {
                g_btnHover = g_togHover = g_scanHover = FALSE;
                g_scanRowHover = g_scanRowPress = -1;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;

        /* Header is draggable; the two buttons and card area are not */
        case WM_NCHITTEST: {
            LRESULT hit  = DefWindowProcW(hwnd, msg, wp, lp);
            if (hit == HTCLIENT) {
                POINT pt   = {(short)LOWORD(lp), (short)HIWORD(lp)};
                RECT  rBtn = GetBtnRect(hwnd);
                RECT  rTog = GetToggleRect(hwnd);
                ScreenToClient(hwnd, &pt);
                if (PtInRect(&rBtn, pt))                    return HTCLIENT;
                if (g_nDisabled > 0 && PtInRect(&rTog, pt)) return HTCLIENT;
                if (pt.y < HDR_H)                           return HTCAPTION;
            }
            return hit;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_wmTaskbar) { AddTrayIcon(); return 0; }

    switch (msg) {

        case WM_TRAYICON:
            switch ((UINT)lp) {
                case WM_LBUTTONUP:   ToggleWidget(); break;
                case WM_RBUTTONUP:   ShowMenu(hwnd); break;
            }
            return 0;

        case WM_NETCHANGED:
            RefreshAdapters();
            UpdateTip();
            {
                BOOL anyUp = FALSE;
                for (int i = 0; i < g_nAd; i++) if (g_ad[i].up) { anyUp = TRUE; break; }
                if (g_nid.hIcon) DestroyIcon(g_nid.hIcon);
                g_nid.hIcon = MakeIcon(anyUp);
                Shell_NotifyIconW(NIM_MODIFY, &g_nid);
            }
            /* Public IP may have changed too — refetch in the background */
            g_publicIp[0] = 0;
            if (g_vis) FetchPublicIpAsync();
            if (g_vis) {
                PositionWidget();
                InvalidateRect(g_hWid, NULL, TRUE);
            }
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDM_STARTUP:
                    SetStartup(!GetStartup());
                    break;
                case IDM_EXIT:
                    Shell_NotifyIconW(NIM_DELETE, &g_nid);
                    if (g_hNotify) CancelMibChangeNotify2(g_hNotify);
                    if (g_wlan) { WlanCloseHandle(g_wlan, NULL); g_wlan = NULL; }
                    free(g_scan);
                    free(g_ad);
                    PostQuitMessage(0);
                    break;
            }
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ══════════════════════════════════════════════════════════════════════════
   ENTRY POINT
   ══════════════════════════════════════════════════════════════════════════ */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show)
{
    (void)hPrev; (void)cmd; (void)show;
    g_hInst = hInst;

    /* single-instance guard */
    HANDLE mutex = CreateMutexW(NULL, TRUE, L"NetMon_SingleInstance_v1");
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(mutex); return 0; }

    SetProcessDPIAware();

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    g_wmTaskbar = RegisterWindowMessageW(L"TaskbarCreated");

    /* ── main (hidden message-only) window ─────────────────────────────── */
    WNDCLASSEXW wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = MainProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"NetMon_Main";
    RegisterClassExW(&wc);

    g_hMain = CreateWindowExW(0, L"NetMon_Main", L"NetMon",
                              0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInst, NULL);

    /* ── widget window ─────────────────────────────────────────────────── */
    WNDCLASSEXW ww = {0};
    ww.cbSize        = sizeof(ww);
    ww.lpfnWndProc   = WidgetProc;
    ww.hInstance     = hInst;
    ww.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    ww.hbrBackground = NULL;
    ww.lpszClassName = L"NetMon_Widget";
    RegisterClassExW(&ww);

    /* WS_EX_TOOLWINDOW  = no taskbar button, no Alt+Tab entry
       WS_EX_TOPMOST     = always above other windows
       WS_EX_NOACTIVATE  = never steals focus — user keeps working uninterrupted */
    g_hWid = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        L"NetMon_Widget", L"NetMon",
        WS_POPUP,
        0, 0, WW, 100,
        NULL, NULL, hInst, NULL);

    /* Windows 11: rounded corners */
    DWORD corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(g_hWid, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

    /* Windows 11: custom border colour */
    COLORREF borderCol = C_EDGE;
    DwmSetWindowAttribute(g_hWid, DWMWA_BORDER_COLOR, &borderCol, sizeof(borderCol));

    /* Capture the widget monitor's DPI so all geometry + fonts scale to it.
       GetDpiForWindow is Win10 1607+, so load it dynamically (no hard dep). */
    {
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        typedef UINT (WINAPI *PFNGDFW)(HWND);
        PFNGDFW pGetDpi = u32 ? (PFNGDFW)GetProcAddress(u32, "GetDpiForWindow") : NULL;
        if (pGetDpi) { UINT d = pGetDpi(g_hWid); if (d) g_dpi = d; }
    }

    /* ── initial data + tray ───────────────────────────────────────────── */
    RefreshAdapters();
    AddTrayIcon();

    /* ── subscribe to interface-change events (no polling!) ────────────── */
    NotifyIpInterfaceChange(AF_UNSPEC, OnNetChange, NULL, FALSE, &g_hNotify);

    /* ── message loop ──────────────────────────────────────────────────── */
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    WSACleanup();
    CloseHandle(mutex);
    return (int)m.wParam;
}
