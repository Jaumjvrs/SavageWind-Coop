// (coop-upnp, 0.2.0) Router port for a co-op host: while this game hosts, it asks the home router over UPnP
// to forward the co-op UDP port to this PC, reads the router's internet address and says
// whether friends can reach it. Replaces the old PowerShell helper window.
//
// coop_net_start (host, not bound to 127.0.0.1) calls coop_upnp_start; coop_net_stop and the process exit call
// coop_upnp_stop / coop_upnp_shutdown. The router is only asked from a background thread (network calls can take seconds).
// A mapping is removed at the end only when this game made it (or found its own, "Savage Wind Co-op" to this PC, left
// by a game that did not close normally); a mapping to another PC is never touched.
//
// Off: Options > Co-op "Open router port automatically" (settings coop_upnp) or SAVAGEWIND_COOP_UPNP=0.
// SAVAGEWIND_COOP_UPNP_DRYRUN=1: everything except adding / removing the mapping (logs what it would do). Test
// instances (SAVAGEWIND_CONTROL set: the developer control socket) default to a dry run; SAVAGEWIND_COOP_UPNP_DRYRUN=0
// lets them change the router.
// Windows only: elsewhere coop_upnp_start reports COOP_UPNP_UNSUPPORTED.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COOP_UPNP_DESCRIPTION "Savage Wind Co-op"

enum {
    COOP_UPNP_IDLE,         // not hosting (or hosting only on this PC: split screen, 127.0.0.1)
    COOP_UPNP_DISABLED,     // the setting (or SAVAGEWIND_COOP_UPNP=0) is off: the player forwards the port
    COOP_UPNP_UNSUPPORTED,  // not on Windows
    COOP_UPNP_WORKING,      // asking the router
    COOP_UPNP_OPEN,         // the router forwards the port to this PC (made now, or found)
    COOP_UPNP_NO_ROUTER,    // the router did not answer (UPnP off or not supported)
    COOP_UPNP_NO_LAN,       // this PC's network address was not found
    COOP_UPNP_FOREIGN,      // the port is already forwarded to another PC (or another port of this one): left alone
    COOP_UPNP_REFUSED,      // the router refused to add the mapping
    COOP_UPNP_CGNAT,        // the port is open, but the router's internet address is a shared / private one
};

typedef struct {
    int state;          // COOP_UPNP_*
    int port;           // the UDP port
    bool dry_run;       // nothing was / will be changed on the router
    bool owned;         // the mapping is ours: removed when hosting stops
    char lan[48];       // this PC's address on the home network ("": unknown)
    char external[48];  // the router's internet address ("": unknown)
    char other[48];     // COOP_UPNP_FOREIGN: where the port goes now
} CoopUpnpStatus;

void coop_upnp_start(int port);              // hosting started (game thread; returns at once)
void coop_upnp_stop(void);                   // hosting stopped: our mapping is removed in the background
void coop_upnp_shutdown(void);               // the process ends: like stop, waits a few seconds at most
void coop_upnp_status(CoopUpnpStatus *out);  // the current session's status (IDLE when none)
// (0.1.1) This PC's address on the home network (192.168.x.x, 10.x.x.x, ...), for players on the same Wi-Fi: the
// router's internet address often does not work from inside the same network. The UPnP answer's when there was one,
// else the address Windows would use to reach the internet (no packet is sent). false: none found.
bool coop_lan_address(char *out, int cap);

// ---- plain decisions (runtime/coop_upnp_logic.c; tests/test_coop_net.c "upnp")
enum { COOP_ADDR_INVALID = -1, COOP_ADDR_PUBLIC, COOP_ADDR_PRIVATE, COOP_ADDR_SHARED };
// An IPv4 address: PUBLIC; PRIVATE (10/8, 172.16/12, 192.168/16, 169.254/16, 127/8, 0/8); SHARED (100.64/10, CGNAT)
int coop_upnp_addr_kind(const char *ipv4);
// What to do about the router's mapping of our port. found: the router has one; client / internal_port / desc: its
// target and description; mine: client is one of this PC's addresses.
enum { COOP_UPNP_ADD, COOP_UPNP_ADOPT, COOP_UPNP_REUSE, COOP_UPNP_LEAVE };
int coop_upnp_decide(bool found, bool mine, int internal_port, const char *desc, int port);
// A mapping description made by this game (starts with COOP_UPNP_DESCRIPTION: some routers add junk after it)
bool coop_upnp_is_ours(const char *desc);
// The state once the mapping is in place (or not): CGNAT when the internet address is not a public one
int coop_upnp_final_state(int state, const char *external);
// One line for the player; tr: the menu's translation (NULL: English). Returns out.
const char *coop_upnp_status_text(const CoopUpnpStatus *s, const char *(*tr)(const char *), char *out, int cap);
// Short value for the menu row ("Open", "Not open", ...), English (the caller translates)
const char *coop_upnp_status_short(const CoopUpnpStatus *s);

#ifdef __cplusplus
}
#endif
