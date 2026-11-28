// Actual receiver exercised over loopback UDP, without game assets or guest memory.
#include "coop_net.c"
#include <assert.h>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>
typedef int SOCKET;
typedef uint32_t DWORD;
typedef int WSADATA;
#define FIONBIO 1
#define MAKEWORD(a, b) 0
#define closesocket close
static int WSAStartup(int v, WSADATA *w) { (void)v; (void)w; return 0; }
static void Sleep(unsigned ms) {
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}
static uint64_t GetTickCount64(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}
static DWORD GetTickCount(void) { return (DWORD)GetTickCount64(); }
static int ioctlsocket(int s, long cmd, unsigned long *arg) {
    (void)cmd;
    int fl = fcntl(s, F_GETFL, 0);
    return fl < 0 ? -1 : fcntl(s, F_SETFL, *arg ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}
static int test_setsockopt(int s, int level, int optname, const void *optval, socklen_t optlen) {
    if (level == SOL_SOCKET && optname == SO_RCVTIMEO && optlen == sizeof(DWORD)) {
        DWORD ms = *(const DWORD *)optval;
        struct timeval tv = {(time_t)(ms / 1000), (suseconds_t)((ms % 1000) * 1000)};
        return setsockopt(s, level, optname, &tv, sizeof tv);
    }
    return setsockopt(s, level, optname, optval, optlen);
}
#define setsockopt test_setsockopt
static int test_getsockname(int s, struct sockaddr *addr, int *len) {
    socklen_t sl = (socklen_t)*len;
    int r = getsockname(s, addr, &sl);
    *len = (int)sl;
    return r;
}
#define getsockname test_getsockname
static int test_putenv(const char *s) {
    const char *eq = strchr(s, '=');
    if (!eq) return -1;
    char key[128];
    size_t n = (size_t)(eq - s);
    if (!n || n >= sizeof key) return -1;
    memcpy(key, s, n);
    key[n] = 0;
    return eq[1] ? setenv(key, eq + 1, 1) : unsetenv(key);
}
#define _putenv test_putenv
#endif

// (protocol 53) every datagram the tests send carries the join-code tag, like the game's (coop_sendto); a call written
// (sendto)(...) sends a raw datagram without it
static int tagged_sendto(SOCKET s, const char *b, int l, int f, const struct sockaddr *a, int al) {
    (void)f; (void)al; return coop_sendto(s, b, l, (const struct sockaddr_in *)a);
}
#define sendto(...) tagged_sendto(__VA_ARGS__)

uint64_t plat_now_ns(void) { return (uint64_t)GetTickCount64() * 1000000ull; }

// Count calls to the router thread. The "upnp" tests use the real parsers,
// mapping decisions and status text without sending anything to a router.
#include "coop_upnp_logic.c"
static int upnp_starts, upnp_stops;
void coop_upnp_start(int port) { (void)port; upnp_starts++; }
void coop_upnp_stop(void) { upnp_stops++; }
void coop_upnp_shutdown(void) {}
void coop_upnp_status(CoopUpnpStatus *out) { memset(out, 0, sizeof *out); }

static const char *upnp_tr_test(const char *en) { return !strcmp(en, "In use") ? "Em uso" : en; }

static void upnp_parse_test(void) {
    const char *sender = "192.168.4.1";
    CoopUpnpUrl loc, url;
    assert(coop_upnp_ssdp("HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\n"
                          "lOcAtIoN:\t http://192.168.4.1:46171/igd/rootDesc.xml \r\n\r\n", sender, &loc));
    assert(loc.port == 46171 && !strcmp(loc.target, "/igd/rootDesc.xml"));
    assert(!coop_upnp_ssdp("HTTP/1.1 404 Not Found\r\nLOCATION: http://192.168.4.1/\r\n\r\n", sender, &url));
    assert(!coop_upnp_ssdp("HTTP/1.1 200 OK\r\nLOCATION: http://192.168.4.1/\r\n"
                           "LOCATION: http://192.168.4.2/\r\n\r\n", sender, &url));
    assert(!coop_upnp_url("https://192.168.4.1/", sender, &loc, &url));
    assert(!coop_upnp_url("http://192.168.4.1@192.168.4.2/", sender, &loc, &url));
    assert(!coop_upnp_url("http://192.168.4.1:0/", sender, &loc, &url));
    assert(!coop_upnp_url("http://192.168.4.1:65536/", sender, &loc, &url));
    assert(!coop_upnp_url("http://203.0.113.1/", "203.0.113.1", NULL, &url));
    assert(!coop_upnp_url("http://127.0.0.1/", "127.0.0.1", NULL, &url));
    assert(!coop_upnp_url("//192.168.4.2/soap", sender, &loc, &url));
    assert(!coop_upnp_url("/ctl\r\nHost: example.com", sender, &loc, &url));
    assert(coop_upnp_url("http://192.168.4.1?x=1", sender, &loc, &url));
    assert(url.port == 80 && !strcmp(url.target, "/?x=1"));
    assert(coop_upnp_url("?x=1", sender, &loc, &url));
    assert(url.port == 46171 && !strcmp(url.target, "/igd/rootDesc.xml?x=1"));
    assert(coop_upnp_url("../a/./b/..?x=/../", sender, &loc, &url));
    assert(!strcmp(url.target, "/a/?x=/../"));

    const char *root =
        "<?xml version=\"1.0\"?>"
        "<root xmlns=\"urn:schemas-upnp-org:device-1-0\"><specVersion><major>1</major></specVersion>"
        "<device><deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:2</deviceType>"
        "<deviceList><device><deviceType>urn:schemas-upnp-org:device:WANDevice:2</deviceType>"
        "<deviceList><device><deviceType>urn:schemas-upnp-org:device:WANConnectionDevice:2</deviceType><serviceList>"
        "<!-- <service><controlURL>unexpected</controlURL></service> -->"
        "<service><serviceType>urn:schemas-upnp-org:service:WANPPPConnection:1</serviceType>"
        "<controlURL>/ctl/PPPConn</controlURL></service>"
        "<service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:2</serviceType>"
        "<controlURL>../ctl/IPConn2</controlURL></service>"
        "<service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>"
        "<controlURL>http://192.168.4.2/wrong-router</controlURL></service>"
        "<service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>"
        "<controlURL>ctl/IPConn?x=1&amp;y=2</controlURL></service>"
        "</serviceList></device></deviceList></device></deviceList></device></root>";
    const char *cursor = NULL;
    assert(coop_upnp_control(root, &cursor, COOP_UPNP_IP1, sender, &loc, &url));
    assert(url.port == loc.port && !strcmp(url.target, "/igd/ctl/IPConn?x=1&y=2"));
    assert(!coop_upnp_control(root, &cursor, COOP_UPNP_IP1, sender, &loc, &url));
    cursor = NULL;
    assert(coop_upnp_control(root, &cursor, COOP_UPNP_IP2, sender, &loc, &url));
    assert(!strcmp(url.target, "/ctl/IPConn2"));
    cursor = NULL;
    assert(coop_upnp_control(root, &cursor, COOP_UPNP_PPP1, sender, &loc, &url));
    assert(!strcmp(url.target, "/ctl/PPPConn"));
    const char *with_base =
        "<root><URLBase>http://192.168.4.1:8080/upnp/</URLBase><service>"
        "<serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>"
        "<controlURL>IPConn</controlURL></service></root>";
    cursor = NULL;
    assert(coop_upnp_control(with_base, &cursor, COOP_UPNP_IP1, sender, &loc, &url));
    assert(url.port == 8080 && !strcmp(url.target, "/upnp/IPConn"));
    cursor = NULL;
    assert(!coop_upnp_control("<root><URLBase>http://192.168.4.2/</URLBase><service>"
                              "<serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>"
                              "<controlURL>IPConn</controlURL></service></root>",
                              &cursor, COOP_UPNP_IP1, sender, &loc, &url));

    const char *ext = "<s:Envelope xmlns:s='soap'><s:Body><u:GetExternalIPAddressResponse xmlns:u='test'>"
                      "<NewExternalIPAddress>\r\n203.0.113.5 </NewExternalIPAddress>"
                      "</u:GetExternalIPAddressResponse></s:Body></s:Envelope>";
    char value[128];
    assert(coop_upnp_soap_response(ext, "GetExternalIPAddress"));
    assert(!coop_upnp_soap_response(ext, "AddPortMapping"));
    assert(coop_upnp_external(ext, value, sizeof value));
    assert(!strcmp(value, "203.0.113.5"));
    assert(!coop_upnp_xml_text("<!DOCTYPE x><x>abc</x>", "x", value, sizeof value));
    assert(!coop_upnp_xml_text("<x>abc</y>", "x", value, sizeof value));
    assert(coop_upnp_xml_text("<x><![CDATA[a&b]]>&#xE9;<!-- ignored --></x>", "x", value, sizeof value));
    assert(!strcmp(value, "a&b\xc3\xa9"));
    assert(coop_upnp_xml_escape("<&>\"'", value, sizeof value));
    assert(!strcmp(value, "&lt;&amp;&gt;&quot;&apos;"));
    assert(!coop_upnp_xml_escape("&", value, 5));
    const char *fault = "<s:Envelope><s:Body><s:Fault><detail><UPnPError>"
                        "<errorCode>714</errorCode></UPnPError></detail></s:Fault></s:Body></s:Envelope>";
    assert(coop_upnp_fault(fault) == 714);
    assert(!coop_upnp_fault("<errorCode>714</errorCode>"));
    const char *mapping = "<s:Envelope><s:Body><m:GetSpecificPortMappingEntryResponse>"
                          "<NewInternalPort>57001</NewInternalPort><NewInternalClient>192.168.4.3</NewInternalClient>"
                          "<NewEnabled>yes</NewEnabled><NewPortMappingDescription> Test &amp; Co-op </NewPortMappingDescription>"
                          "<NewLeaseDuration>3600</NewLeaseDuration>"
                          "</m:GetSpecificPortMappingEntryResponse></s:Body></s:Envelope>";
    CoopUpnpMapping entry;
    assert(coop_upnp_mapping(mapping, &entry));
    assert(entry.enabled && entry.port == 57001 && entry.lease == 3600);
    assert(!strcmp(entry.client, "192.168.4.3") && !strcmp(entry.description, " Test & Co-op "));
    assert(!coop_upnp_mapping(fault, &entry));
    // (a real router, miniupnpd 2.1: an older description's tail after ours, control bytes, lease 86400)
    const char *garbled = "<s:Envelope><s:Body><u:GetSpecificPortMappingEntryResponse>"
                          "<NewInternalPort>47000</NewInternalPort><NewInternalClient>192.168.1.20</NewInternalClient>"
                          "<NewEnabled>1</NewEnabled><NewPortMappingDescription>Savage Wind Co-opOtherGame(UDP)\x08\x01"
                          "</NewPortMappingDescription><NewLeaseDuration>86400</NewLeaseDuration>"
                          "</u:GetSpecificPortMappingEntryResponse></s:Body></s:Envelope>";
    assert(coop_upnp_mapping(garbled, &entry) && coop_upnp_is_ours(entry.description));
    assert(coop_upnp_decide(true, true, 47000, entry.description, 47000) == COOP_UPNP_ADOPT);
    // (a description broken past reading: the entry still counts, as not ours)
    const char *broken = "<s:Envelope><s:Body><u:GetSpecificPortMappingEntryResponse>"
                         "<NewInternalPort>47000</NewInternalPort><NewInternalClient>192.168.1.20</NewInternalClient>"
                         "<NewEnabled>1</NewEnabled><NewPortMappingDescription>Savage &bogus; Co-op"
                         "</NewPortMappingDescription></u:GetSpecificPortMappingEntryResponse></s:Body></s:Envelope>";
    assert(coop_upnp_mapping(broken, &entry) && !entry.description[0] && !coop_upnp_is_ours(entry.description));
    assert(!coop_upnp_is_ours("Savage Wind co-op 0123") && !coop_upnp_is_ours("Savage Wind Co") && !coop_upnp_is_ours(NULL));

    CoopUpnpHttp head;
    size_t size;
    assert(coop_upnp_http_headers("HTTP/1.0 200 OK\r\nContent-Length: 65536", &head));
    assert(head.status == 200 && head.length == 65536 && !head.chunked);
    assert(coop_upnp_http_headers("HTTP/1.1 500 Error", &head));
    assert(head.status == 500 && head.length == -1 && !head.chunked);
    assert(coop_upnp_http_headers("HTTP/1.1 200 OK\r\ntransfer-encoding: Chunked", &head) && head.chunked);
    assert(!coop_upnp_http_headers("HTTP/1.1 200 OK\r\nContent-Length: 65537", &head));
    assert(!coop_upnp_http_headers("HTTP/1.1 200 OK\r\nContent-Length: 4\r\nTransfer-Encoding: chunked", &head));
    assert(coop_upnp_chunk_size("1fa;some=extension", &size) && size == 506);
    assert(coop_upnp_chunk_size("0", &size) && size == 0);
    assert(!coop_upnp_chunk_size("ffffffffffffffff", &size));
}

static int upnp_main(void) {
    upnp_parse_test();
    // addresses
    assert(coop_upnp_addr_kind("203.0.113.5") == COOP_ADDR_PUBLIC);
    assert(coop_upnp_addr_kind("8.8.8.8") == COOP_ADDR_PUBLIC);
    assert(coop_upnp_addr_kind("10.1.2.3") == COOP_ADDR_PRIVATE);
    assert(coop_upnp_addr_kind("172.16.0.1") == COOP_ADDR_PRIVATE && coop_upnp_addr_kind("172.31.255.255") == COOP_ADDR_PRIVATE);
    assert(coop_upnp_addr_kind("172.15.0.1") == COOP_ADDR_PUBLIC && coop_upnp_addr_kind("172.32.0.1") == COOP_ADDR_PUBLIC);
    assert(coop_upnp_addr_kind("192.168.0.10") == COOP_ADDR_PRIVATE && coop_upnp_addr_kind("169.254.1.1") == COOP_ADDR_PRIVATE);
    assert(coop_upnp_addr_kind("100.64.0.1") == COOP_ADDR_SHARED && coop_upnp_addr_kind("100.127.255.1") == COOP_ADDR_SHARED);
    assert(coop_upnp_addr_kind("100.63.0.1") == COOP_ADDR_PUBLIC && coop_upnp_addr_kind("100.128.0.1") == COOP_ADDR_PUBLIC);
    assert(coop_upnp_addr_kind("") == COOP_ADDR_INVALID && coop_upnp_addr_kind(NULL) == COOP_ADDR_INVALID);
    assert(coop_upnp_addr_kind("1.2.3") == COOP_ADDR_INVALID && coop_upnp_addr_kind("1.2.3.256") == COOP_ADDR_INVALID);
    assert(coop_upnp_addr_kind("1.2.3.4x") == COOP_ADDR_INVALID && coop_upnp_addr_kind("::1") == COOP_ADDR_INVALID);
    // what to do with the router's mapping of the port
    assert(coop_upnp_decide(false, false, 0, "", 47000) == COOP_UPNP_ADD);
    assert(coop_upnp_decide(true, true, 47000, COOP_UPNP_DESCRIPTION, 47000) == COOP_UPNP_ADOPT);
    assert(coop_upnp_decide(true, true, 47000, "Savage Wind co-op 0123", 47000) == COOP_UPNP_REUSE);  // (old helper)
    assert(coop_upnp_decide(true, true, 47000, "", 47000) == COOP_UPNP_REUSE);  // (by hand)
    assert(coop_upnp_decide(true, false, 47000, COOP_UPNP_DESCRIPTION, 47000) == COOP_UPNP_LEAVE);  // (another PC)
    assert(coop_upnp_decide(true, true, 47001, COOP_UPNP_DESCRIPTION, 47000) == COOP_UPNP_LEAVE);  // (another port)
    // CGNAT
    assert(coop_upnp_final_state(COOP_UPNP_OPEN, "203.0.113.5") == COOP_UPNP_OPEN);
    assert(coop_upnp_final_state(COOP_UPNP_OPEN, "100.70.1.2") == COOP_UPNP_CGNAT);
    assert(coop_upnp_final_state(COOP_UPNP_OPEN, "192.168.1.1") == COOP_UPNP_CGNAT);  // (a router behind a router)
    assert(coop_upnp_final_state(COOP_UPNP_OPEN, "") == COOP_UPNP_OPEN);
    assert(coop_upnp_final_state(COOP_UPNP_FOREIGN, "100.70.1.2") == COOP_UPNP_FOREIGN);
    // texts
    CoopUpnpStatus st = {.state = COOP_UPNP_OPEN, .port = 47000};
    char t[320];
    snprintf(st.external, sizeof st.external, "203.0.113.5");
    assert(!strcmp(coop_upnp_status_text(&st, NULL, t, sizeof t), "Port 47000 open. Share this address: 203.0.113.5 (A: copy)"));
    assert(!strcmp(coop_upnp_status_short(&st), "Port open"));
    st.dry_run = true;
    assert(!strcmp(coop_upnp_status_short(&st), "Open (test)"));
    st.dry_run = false, st.state = COOP_UPNP_NO_ROUTER;
    assert(!strcmp(coop_upnp_status_text(&st, NULL, t, sizeof t),
                   "Router did not answer (UPnP off): forward UDP 47000 to this PC by hand."));
    st.state = COOP_UPNP_CGNAT;
    assert(strstr(coop_upnp_status_text(&st, NULL, t, sizeof t), "(CGNAT)"));
    st.state = COOP_UPNP_FOREIGN;
    snprintf(st.other, sizeof st.other, "192.168.0.7");
    assert(!strcmp(coop_upnp_status_text(&st, NULL, t, sizeof t), "Port 47000 is already forwarded to another PC (192.168.0.7)."));
    assert(!strcmp(upnp_tr_test(coop_upnp_status_short(&st)), "Em uso"));
    // (the game's description pane is one line: every text fits in 80 characters with the longest port and addresses)
    CoopUpnpStatus wide = {.port = 65535};
    snprintf(wide.external, sizeof wide.external, "203.255.255.255");
    snprintf(wide.other, sizeof wide.other, "192.168.255.255");
    snprintf(wide.lan, sizeof wide.lan, "192.168.255.255");
    for (wide.state = COOP_UPNP_IDLE; wide.state <= COOP_UPNP_CGNAT; wide.state++) {
        coop_upnp_status_text(&wide, NULL, t, sizeof t);
        if (strlen(t) > 80) { printf("FAIL upnp: too long for one line (%zu): %s\n", strlen(t), t); return 1; }
    }
    st.state = COOP_UPNP_IDLE;
    assert(!*coop_upnp_status_text(&st, NULL, t, sizeof t));
    assert(coop_upnp_status_text(&st, NULL, t, 0) == t);
    st.state = COOP_UPNP_OPEN;
    char small[12];
    coop_upnp_status_text(&st, NULL, small, sizeof small);
    assert(strlen(small) == sizeof small - 1);
    // a host bound to 127.0.0.1 (split screen, tests) never asks the router; stop always tells it
    CoopNetConfig c = {.mode = COOP_MODE_HOST, .port = 0};
    assert(coop_net_start(&c));
    assert(upnp_starts == 0);
    coop_net_stop();
    assert(upnp_stops == 1);
    printf("PASS upnp: SSDP, URLs, XML, SOAP, HTTP framing, mapping decisions, status texts and loopback\n");
    return 0;
}
static uint32_t count(void) { uint32_t s,r; bool k; coop_net_stats(&s,&r,&k); return r; }
static uint64_t request_challenge(SOCKET sender, struct sockaddr_in *target, const CoopPacket *p) {
    // On a restart the test endpoint can still have a full queue for the old session.
    unsigned char wire[MAX_DATAGRAM + COOP_TAG + 1];
    unsigned long nonblocking = 1;
    assert(!ioctlsocket(sender, FIONBIO, &nonblocking));
    while (recv(sender, (char *)wire, sizeof wire, 0) > 0) {}
    nonblocking = 0;
    assert(!ioctlsocket(sender, FIONBIO, &nonblocking));
    assert(coop_sendto(sender, p, sizeof *p, target) == sizeof *p);
    DWORD timeout = 1000;
    assert(!setsockopt(sender, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof timeout));
    for (int i = 0; i < 100; i++) {
        int n = recv(sender, (char *)wire, sizeof wire, 0);
        assert(n > 0);
        if (tag_check(wire, n) != sizeof(CoopChallenge)) continue;
        CoopChallenge c;
        memcpy(&c, wire, sizeof c);
        if (c.magic == COOP_CHALLENGE_MAGIC && c.version == COOP_VERSION && c.session == p->session) {
            assert(n < sizeof *p + COOP_TAG && c.cookie);
            return c.cookie;
        }
    }
    assert(!"no return-address challenge");
    return 0;
}

static void deliver(SOCKET sender, struct sockaddr_in *target, CoopPacket *p, int delta) {
    unsigned char wire[sizeof *p + 1] = {0};
    CoopPacket state = *p;
    if (!delta && host_mode && packet_valid(p)) {
        struct sockaddr_in addr;
        int len = sizeof addr;
        bool bound = !getsockname(sender, (struct sockaddr *)&addr, &len);
        bool known = false;
        pthread_mutex_lock(&lock);
        bool retired = session_retired(p->session);
        for (int g = 1; g < COOP_MAX_PLAYERS; g++)
            if (bound && conn_recent(g, plat_now_ns()) && conn[g].session == p->session &&
                addr.sin_port == conn[g].addr.sin_port) known = true;
        pthread_mutex_unlock(&lock);
        if (!retired && !known) state.challenge = request_challenge(sender, target, p);
    }
    memcpy(wire,&state,sizeof state);
    assert(sendto(sender,(char *)wire,sizeof *p + delta,0,(struct sockaddr *)target,sizeof *target) > 0);
    Sleep(100);
}
// `test_coop_net sim`: the simulated bad network (SAVAGEWIND_COOP_SIM) on a host: held, lost and reordered packets.
static int sim_main(void) {
    WSADATA w; assert(!WSAStartup(MAKEWORD(2,2),&w));
    SOCKET sender = socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    struct sockaddr_in from = {0}, target;
    from.sin_family=AF_INET; from.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(!bind(sender,(struct sockaddr *)&from,sizeof from));
    _putenv("SAVAGEWIND_COOP_PEER="); _putenv("SAVAGEWIND_COOP_PORT=0"); _putenv("SAVAGEWIND_COOP_SIM=400,0,0");
    coop_net_init(); assert(coop_net_on()); assert(sim.on && sim.delay_ms==400 && sim.loss_pct==0);
    int len=sizeof target; getsockname(sock,(struct sockaddr *)&target,&len);
    target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    CoopPacket p={0}; p.magic=COOP_MAGIC; p.version=COOP_VERSION; p.flags=1; p.session=201; p.echo=local_session;
    p.seq=10;
    p.challenge = request_challenge(sender, &target, &p);
    assert(coop_net_partner_id() < 0 && count() == 0);
    assert(coop_sendto(sender, &p, sizeof p, &target) == sizeof p);
    Sleep(100); assert(count()==0);                              // held for 400 ms
    Sleep(450); assert(count()==1);                              // then delivered
    sim.loss_pct=100; p.seq=11; deliver(sender,&target,&p,0); Sleep(500); assert(count()==1 && sim_lost==1); // lost
    sim.loss_pct=0;
    p.seq=31; deliver(sender,&target,&p,0);                      // older one, held 400 ms
    sim.delay_ms=0; p.seq=32; deliver(sender,&target,&p,0); assert(count()==2); // newer one overtakes it
    Sleep(450); assert(count()==2);                              // the late older one is ignored
    // Shared enemies: totals from the current session are kept (the largest), others ignored.
    CoopEnemyPacket ep={0}; ep.magic=COOP_ENEMY_MAGIC; ep.version=COOP_VERSION; ep.session=201; ep.echo=local_session;
    ep.count=2; ep.e[0]=(CoopEnemyDamage){1133377874u,5}; ep.e[1]=(CoopEnemyDamage){42,1};
    sim.delay_ms=0;
    sendto(sender,(char *)&ep,sizeof ep,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
    assert(coop_net_remote_damage(1133377874u)==5 && coop_net_remote_damage(42)==1 && coop_net_remote_damage(7)==0);
    ep.e[0].damage=3; sendto(sender,(char *)&ep,sizeof ep,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
    assert(coop_net_remote_damage(1133377874u)==5);             // an older (smaller) total is ignored
    ep.e[0].damage=9; ep.session=999; sendto(sender,(char *)&ep,sizeof ep,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
    assert(coop_net_remote_damage(1133377874u)==5);             // another session's totals are ignored
    // Enemy positions: only flagged entries, the newest packet seq wins, a reordered older one does not move it.
    float pos[3], yaw[4]; double age;
    assert(!coop_net_remote_enemy_pos(1133377874u,pos,yaw,&age,NULL,0)); // (none sent yet: no flag above)
    ep.session=201; ep.seq=50; ep.e[0].flags=COOP_ENEMY_POS; ep.e[0].pos[0]=-880; ep.e[0].pos[1]=210; ep.e[0].pos[2]=1805;
    ep.e[0].rot[1]=0.7f; ep.e[0].rot[3]=0.7f; sendto(sender,(char *)&ep,sizeof ep,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
    assert(coop_net_remote_enemy_pos(1133377874u,pos,yaw,&age,NULL,0) && pos[0]==-880 && pos[2]==1805 && yaw[1]==0.7f && yaw[3]==0.7f && age<1);
    assert(!coop_net_remote_enemy_pos(42,pos,yaw,&age,NULL,0));      // (its entry had no position)
    ep.seq=49; ep.e[0].pos[0]=-1; sendto(sender,(char *)&ep,sizeof ep,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
    assert(coop_net_remote_enemy_pos(1133377874u,pos,yaw,&age,NULL,0) && pos[0]==-880); // older seq: ignored
    ep.seq=51; ep.e[0].pos[0]=-870; sendto(sender,(char *)&ep,sizeof ep,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
    assert(coop_net_remote_enemy_pos(1133377874u,pos,yaw,&age,NULL,0) && pos[0]==-870); // newer: taken
    ep.seq=52; ep.e[0].pos[0]=0.0f/0.0f; sendto(sender,(char *)&ep,sizeof ep,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
    assert(coop_net_remote_enemy_pos(1133377874u,pos,yaw,&age,NULL,0) && pos[0]==-870); // not a number: ignored
    char as[32];  // its AS name: taken with the position, always terminated
    ep.seq=53; ep.e[0].pos[0]=-860; snprintf(ep.e[0].as,sizeof ep.e[0].as,"Run_Battle");
    sendto(sender,(char *)&ep,sizeof ep,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
    assert(coop_net_remote_enemy_pos(1133377874u,pos,yaw,&age,as,sizeof as) && !strcmp(as,"Run_Battle"));
    ep.seq=54; memset(ep.e[0].as,'A',sizeof ep.e[0].as);
    sendto(sender,(char *)&ep,sizeof ep,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
    assert(coop_net_remote_enemy_pos(1133377874u,pos,yaw,&age,as,sizeof as) && strlen(as)==31);
    // A full table (REMOTE_ENEMIES) forgets the enemy not heard of for the longest; it used to drop new ones forever.
    Sleep(20);
    ep.e[1].flags=0; ep.e[0].flags=0;
    for (uint32_t h=1000; h<1000+REMOTE_ENEMIES; h+=16) {
        ep.seq++; ep.count=16;
        for (int i=0;i<16;i++) ep.e[i]=(CoopEnemyDamage){h+i,1};
        sendto(sender,(char *)&ep,sizeof ep,0,(struct sockaddr *)&target,sizeof target); Sleep(30);
    }
    assert(coop_net_remote_damage(1000+REMOTE_ENEMIES-1)==1);           // the newest are in
    assert(coop_net_remote_damage(1133377874u)==0 && coop_net_remote_damage(42)==0); // the oldest two were replaced
    assert(coop_net_remote_damage(1000)==1);                            // (the rest stays)
    // A packet of another protocol version is ignored (and logged: "runs co-op protocol N, this build has M").
    ep.version=COOP_VERSION-1; ep.seq++; ep.count=1; ep.e[0]=(CoopEnemyDamage){77,9};
    sendto(sender,(char *)&ep,sizeof ep,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
    assert(coop_net_remote_damage(77)==0);
    // Blood moons can revive the same HashId. A delayed packet from before revival must not kill it again.
    assert(coop_net_set_enemy_generation(1));
    assert(coop_net_remote_damage(1000+REMOTE_ENEMIES-1)==0);
    ep.version=COOP_VERSION; ep.seq++; ep.e[0]=(CoopEnemyDamage){77,9};
    sendto(sender,(char *)&ep,sizeof ep,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
    assert(coop_net_remote_damage(77)==0);
    ep.generation=1; ep.seq++; ep.e[0].damage=2;
    sendto(sender,(char *)&ep,sizeof ep,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
    assert(coop_net_remote_damage(77)==2 && !coop_net_set_enemy_generation(1));
    printf("PASS sim: delay, loss, reordering, enemy damage totals and positions, full table\n");
    closesocket(sender);
    return 0;
}
// `test_coop_net events`: reliable events (in order, once, resent until acknowledged) and run-time enemy names.
static bool recv_events(SOCKET s, CoopEventPacket *out, int min_count) {
    for (int tries = 0; tries < 40; tries++) {
        unsigned char wire[MAX_DATAGRAM + COOP_TAG + 1];
        DWORD tmo = 200; setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,(char *)&tmo,sizeof tmo);
        int n = recv(s,(char *)wire,sizeof wire,0);
        if (n > 0) n = tag_check(wire,n);  // (protocol 53: the sender's join-code tag)
        if (n == (int)sizeof *out) { memcpy(out,wire,sizeof *out); if (out->magic==COOP_EVENT_MAGIC && out->count>=min_count) return true; }
    }
    return false;
}
static int events_main(void) {
    WSADATA w; assert(!WSAStartup(MAKEWORD(2,2),&w));
    SOCKET sender = socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    struct sockaddr_in from = {0}, target;
    from.sin_family=AF_INET; from.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(!bind(sender,(struct sockaddr *)&from,sizeof from));
    _putenv("SAVAGEWIND_COOP_PEER="); _putenv("SAVAGEWIND_COOP_PORT=0");
    coop_net_init(); assert(coop_net_on());
    int len=sizeof target; getsockname(sock,(struct sockaddr *)&target,&len);
    target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(!coop_net_send_event(1,"x",1));  // no peer yet
    CoopPacket p={0}; p.magic=COOP_MAGIC; p.version=COOP_VERSION; p.flags=1; p.session=301; p.seq=1;
    deliver(sender,&target,&p,0); p.echo=local_session; p.seq=2; deliver(sender,&target,&p,0); assert(count()==1);
    assert(coop_net_send_event(7,"abc",4) && coop_net_send_event(8,"def",4));
    CoopEventPacket vp;
    assert(recv_events(sender,&vp,2) && vp.e[0].id==1 && vp.e[0].type==7 && !strcmp((char *)vp.e[0].data,"abc") && vp.e[1].id==2);
    // The peer acknowledges the first: only the second is resent.
    CoopEventPacket in={0}; in.magic=COOP_EVENT_MAGIC; in.version=COOP_VERSION; in.session=301; in.echo=local_session;
    in.ack=1; sendto(sender,(char *)&in,sizeof in,0,(struct sockaddr *)&target,sizeof target); Sleep(150);
    { bool seen=false; for (int i=0;i<40 && !seen;i++) seen=recv_events(sender,&vp,1) && vp.count==1 && vp.e[0].id==2;
      assert(seen); }
    // Its events: a gap waits; then in order, once.
    in.ack=2; in.count=1; in.e[0]=(CoopEvent){.id=2,.type=21,.len=1};
    sendto(sender,(char *)&in,sizeof in,0,(struct sockaddr *)&target,sizeof target); Sleep(150);
    uint16_t type; int n; unsigned char data[COOP_EVENT_DATA];
    assert(!coop_net_next_event(&type,data,&n));  // (id 1 missing)
    in.count=2; in.e[0]=(CoopEvent){.id=1,.type=20,.len=2}; in.e[1]=(CoopEvent){.id=2,.type=21,.len=1};
    sendto(sender,(char *)&in,sizeof in,0,(struct sockaddr *)&target,sizeof target); Sleep(150);
    sendto(sender,(char *)&in,sizeof in,0,(struct sockaddr *)&target,sizeof target); Sleep(150);  // (a resend)
    assert(coop_net_next_event(&type,data,&n) && type==20 && n==2);
    assert(coop_net_next_event(&type,data,&n) && type==21 && n==1);
    assert(!coop_net_next_event(&type,data,&n));
    { bool seen=false; for (int i=0;i<40 && !seen;i++) seen=recv_events(sender,&vp,0) && vp.ack==2 && vp.count==0;
      assert(seen); }  // (our ack; ours all delivered. Older packets may still be queued in the socket first.)
    in.e[0].len=COOP_EVENT_DATA+1; in.e[0].id=3; in.count=1;  // too long: dropped
    sendto(sender,(char *)&in,sizeof in,0,(struct sockaddr *)&target,sizeof target); Sleep(150);
    assert(!coop_net_next_event(&type,data,&n));
    { CoopTreeHit e={0x1234u,0x99u,30,{-1145,1954}}, got;
      assert(COOP_VERSION>=95 && sizeof e==20 && sizeof e<=COOP_EVENT_DATA && coop_tree_hit_ok(&e));
      in.count=1;
      in.e[0]=(CoopEvent){.id=3,.type=COOP_EV_TREE_HIT,.len=sizeof e};
      memcpy(in.e[0].data,&e,sizeof e);
      sendto(sender,(char *)&in,sizeof in,0,(struct sockaddr *)&target,sizeof target); Sleep(150);
      assert(coop_net_next_event(&type,data,&n) && type==COOP_EV_TREE_HIT && n==sizeof e);
      memcpy(&got,data,sizeof got);
      assert(!memcmp(&e,&got,sizeof e) && coop_tree_hit_ok(&got));
      e.hash=0; assert(!coop_tree_hit_ok(&e)); e.hash=5;
      e.area=0; assert(!coop_tree_hit_ok(&e)); e.area=1;
      e.life=0; assert(!coop_tree_hit_ok(&e)); e.life=-1; assert(!coop_tree_hit_ok(&e));
      e.life=COOP_TREE_LIFE_MAX+1; assert(!coop_tree_hit_ok(&e)); e.life=1;
      e.from[0]=NAN; assert(!coop_tree_hit_ok(&e)); e.from[0]=0;
      e.from[1]=INFINITY; assert(!coop_tree_hit_ok(&e));
      e.from[1]=-100001; assert(!coop_tree_hit_ok(&e)); e.from[1]=0;
      assert(coop_tree_hit_ok(&e)); }
    // (protocol 74) partner's arrows visible in flight: round-trip and validation
    { CoopArrowFly f={0x42u,0x99u,COOP_ARROW_FIRE,COOP_ARROW_FLY_SHOT,{0,0},{-833.0f,200.5f,1815.0f},{120.0f,5.0f,-40.0f},-9.8f}, got;
      assert(sizeof f==40 && sizeof f<=COOP_EVENT_DATA && coop_arrow_fly_ok(&f));
      in.count=1;
      in.e[0]=(CoopEvent){.id=4,.type=COOP_EV_ARROW_FLY,.len=sizeof f};
      memcpy(in.e[0].data,&f,sizeof f);
      sendto(sender,(char *)&in,sizeof in,0,(struct sockaddr *)&target,sizeof target); Sleep(150);
      assert(coop_net_next_event(&type,data,&n) && type==COOP_EV_ARROW_FLY && n==sizeof f);
      memcpy(&got,data,sizeof got);
      assert(!memcmp(&f,&got,sizeof f) && coop_arrow_fly_ok(&got));
      CoopArrowFly b=f; b.id=0; assert(!coop_arrow_fly_ok(&b));
      b=f; b.area=0; assert(!coop_arrow_fly_ok(&b));
      b=f; b.kind=COOP_ARROW_TYPES; assert(!coop_arrow_fly_ok(&b));
      b=f; b.flags=0; assert(!coop_arrow_fly_ok(&b));
      b=f; b.flags=3; assert(!coop_arrow_fly_ok(&b));
      b=f; b.pad[1]=1; assert(!coop_arrow_fly_ok(&b));
      b=f; b.pos[0]=NAN; assert(!coop_arrow_fly_ok(&b));
      b=f; b.pos[2]=100001.0f; assert(!coop_arrow_fly_ok(&b));
      b=f; b.vel[1]=INFINITY; assert(!coop_arrow_fly_ok(&b));
      b=f; b.vel[0]=301.0f; assert(!coop_arrow_fly_ok(&b));
      b=f; b.vel[0]=b.vel[1]=b.vel[2]=0; assert(!coop_arrow_fly_ok(&b));
      b=f; b.gravity=1.0f; assert(!coop_arrow_fly_ok(&b));
      b=f; b.gravity=-51.0f; assert(!coop_arrow_fly_ok(&b));
      b=f; b.gravity=NAN; assert(!coop_arrow_fly_ok(&b));
      CoopArrowFly s={0x42u,0x99u,COOP_ARROW_FIRE,COOP_ARROW_FLY_STOP,{0,0},{-800.0f,200.0f,1800.0f},{0,0,0},0.0f};
      assert(coop_arrow_fly_ok(&s));
      b=s; b.vel[0]=1.0f; assert(!coop_arrow_fly_ok(&b));
      b=s; b.gravity=-9.8f; assert(!coop_arrow_fly_ok(&b));
      b=s; b.pos[1]=NAN; assert(!coop_arrow_fly_ok(&b)); }
    { CoopWildPoll p={.area=7,.token=8,.previous=6,.caught={11}};
      assert(coop_wild_poll_ok(&p));
      p.previous=0; assert(!coop_wild_poll_ok(&p)); p.previous=6;
      p.caught[1]=11; assert(!coop_wild_poll_ok(&p));
      CoopWildRoster r={.area=7,.token=8,.animals={{11,1,{1,0,0},0},{12,2,{2,0,0},1}}}, got;
      assert(coop_wild_roster_ok(&r));
      in.e[0]=(CoopEvent){.id=5,.type=COOP_EV_WILD_ROSTER,.len=sizeof r};
      memcpy(in.e[0].data,&r,sizeof r);
      sendto(sender,(char *)&in,sizeof in,0,(struct sockaddr *)&target,sizeof target); Sleep(150);
      assert(coop_net_next_event(&type,data,&n) && type==COOP_EV_WILD_ROSTER && n==sizeof r);
      memcpy(&got,data,sizeof got); assert(!memcmp(&r,&got,sizeof r));
      r.animals[1].id=11; assert(!coop_wild_roster_ok(&r)); r.animals[1].id=12;
      r.animals[0].species=999; assert(!coop_wild_roster_ok(&r)); r.animals[0].species=1;
      r.animals[1].yaw=NAN; assert(!coop_wild_roster_ok(&r)); r.animals[1].yaw=0;
      r.animals[1].pos[2]=25; assert(!coop_wild_roster_ok(&r)); r.animals[1].pos[2]=0;
      r.animals[0]=(CoopWildAnimal){0}; assert(!coop_wild_roster_ok(&r));
      CoopWildReady ready={7,8};
      in.e[0]=(CoopEvent){.id=6,.type=COOP_EV_WILD_ROSTER,.len=sizeof ready};
      memcpy(in.e[0].data,&ready,sizeof ready);
      sendto(sender,(char *)&in,sizeof in,0,(struct sockaddr *)&target,sizeof target); Sleep(150);
      assert(coop_net_next_event(&type,data,&n) && type==COOP_EV_WILD_ROSTER && n==sizeof ready);
      got=(CoopWildRoster){0}; memcpy(&got,data,n);
      assert(got.area==7 && got.token==8 && coop_wild_roster_ok(&got)); }
    // (protocol 80) one shared small animal caught / killed: round trip and validation
    { CoopFaunaGone g={7,0x1234u,3,COOP_FAUNA_PLACED,COOP_FAUNA_CAUGHT,0,{-895,190,1760}}, got;
      assert(sizeof g==24 && sizeof g<=COOP_EVENT_DATA && coop_fauna_gone_ok(&g));
      in.e[0]=(CoopEvent){.id=7,.type=COOP_EV_FAUNA_GONE,.len=sizeof g};
      memcpy(in.e[0].data,&g,sizeof g);
      sendto(sender,(char *)&in,sizeof in,0,(struct sockaddr *)&target,sizeof target); Sleep(150);
      assert(coop_net_next_event(&type,data,&n) && type==COOP_EV_FAUNA_GONE && n==sizeof g);
      memcpy(&got,data,sizeof got); assert(!memcmp(&g,&got,sizeof g));
      CoopFaunaGone b=g; b.area=0; assert(!coop_fauna_gone_ok(&b));
      b=g; b.id=0; assert(!coop_fauna_gone_ok(&b));
      b=g; b.species=0; assert(!coop_fauna_gone_ok(&b));
      b=g; b.flags=4; assert(!coop_fauna_gone_ok(&b));
      b=g; b.why=0; assert(!coop_fauna_gone_ok(&b));
      b=g; b.why=3; assert(!coop_fauna_gone_ok(&b));
      b=g; b.pad=1; assert(!coop_fauna_gone_ok(&b));
      b=g; b.pos[1]=NAN; assert(!coop_fauna_gone_ok(&b));
      b=g; b.pos[0]=200000; assert(!coop_fauna_gone_ok(&b)); }
    // (protocol 82) a fire lit / put out: round trip and validation
    { CoopFire f={1345630049u,0x1234u,{-1461.3f,252.7f,1773.1f},1,0,{0,0}}, got;
      assert(sizeof f==24 && sizeof f<=COOP_EVENT_DATA && coop_fire_ok(&f));
      in.e[0]=(CoopEvent){.id=8,.type=COOP_EV_FIRE,.len=sizeof f};
      memcpy(in.e[0].data,&f,sizeof f);
      sendto(sender,(char *)&in,sizeof in,0,(struct sockaddr *)&target,sizeof target); Sleep(150);
      assert(coop_net_next_event(&type,data,&n) && type==COOP_EV_FIRE && n==sizeof f);
      memcpy(&got,data,sizeof got); assert(!memcmp(&f,&got,sizeof f));
      CoopFire b=f; b.hash=0; assert(coop_fire_ok(&b));
      b=f; b.area=0; assert(!coop_fire_ok(&b));
      b=f; b.on=2; assert(!coop_fire_ok(&b));
      b=f; b.flags=1; assert(!coop_fire_ok(&b));
      b=f; b.pad[1]=1; assert(!coop_fire_ok(&b));
      b=f; b.pos[2]=NAN; assert(!coop_fire_ok(&b));
      b=f; b.pos[0]=-200000; assert(!coop_fire_ok(&b)); }
    { CoopGrassFire g={0x1234u,{-1012.3f,240.5f,1810.8f}}, got;  // (protocol 83: grass set on fire)
      assert(sizeof g==16 && sizeof g<=COOP_EVENT_DATA && coop_grassfire_ok(&g));
      CoopGrassFire b=g; b.area=0; assert(!coop_grassfire_ok(&b));
      b=g; b.pos[1]=INFINITY; assert(!coop_grassfire_ok(&b));
      b=g; b.pos[2]=150000; assert(!coop_grassfire_ok(&b));
      memcpy(&got,&g,sizeof g); assert(got.area==g.area && got.pos[0]==g.pos[0]); }
    { CoopBeam g={0x55aa1234u,0x1234u,COOP_BEAM_SHOT,0,1,0,{54.5f,135.8f,-578.9f},{0.1f,-0.2f,0.3f},200.0f}, got;
      // (protocol 90: Guardian beam shot / targeting laser)
      assert(sizeof g==40 && sizeof g<=COOP_EVENT_DATA && coop_beam_ok(&g));
      CoopBeam b=g; b.area=0; assert(!coop_beam_ok(&b));
      b=g; b.what=0; assert(!coop_beam_ok(&b));
      b=g; b.what=4; assert(!coop_beam_ok(&b));
      b=g; b.kind=COOP_BEAM_KINDS; assert(!coop_beam_ok(&b));
      b=g; b.pad=1; assert(!coop_beam_ok(&b));
      b=g; b.target=-2; assert(!coop_beam_ok(&b));
      b=g; b.target=COOP_MAX_PLAYERS; assert(!coop_beam_ok(&b));
      b=g; b.pos[1]=NAN; assert(!coop_beam_ok(&b));
      b=g; b.aim[0]=25.0f; assert(!coop_beam_ok(&b));
      b=g; b.target=-1; b.aim[0]=25.0f; b.aim[1]=140.0f; assert(coop_beam_ok(&b));
      b=g; b.speed=0.5f; assert(!coop_beam_ok(&b));
      b=g; b.what=COOP_BEAM_AIM; assert(!coop_beam_ok(&b));  // aim carries no speed
      b.speed=0; assert(coop_beam_ok(&b));
      b.kind=1; assert(coop_beam_ok(&b));  // (AIM: kind 1 = the laser comes from the body itself)
      b=g; b.what=COOP_BEAM_AIM_END; b.speed=0; assert(coop_beam_ok(&b));
      memcpy(&got,&g,sizeof g); assert(got.hash==g.hash && got.aim[2]==g.aim[2]); }
    { CoopMapMark g={COOP_MAPMARK_ADD,31,{0,0},{-1005.5f,250.25f,1810.5f}}, got;  // (protocol 95: shared map markers)
      assert(sizeof g==16 && sizeof g<=COOP_EVENT_DATA && coop_mapmark_ok(&g));
      CoopMapMark b=g; b.what=0; assert(!coop_mapmark_ok(&b));
      b=g; b.what=3; assert(!coop_mapmark_ok(&b));
      b=g; b.what=COOP_MAPMARK_DEL; assert(coop_mapmark_ok(&b));
      b=g; b.no=26; assert(!coop_mapmark_ok(&b));
      b=g; b.no=36; assert(!coop_mapmark_ok(&b));
      b=g; b.no=42; assert(!coop_mapmark_ok(&b));
      b=g; b.no=27; assert(coop_mapmark_ok(&b));
      b=g; b.no=41; assert(coop_mapmark_ok(&b));
      b=g; b.pad[1]=1; assert(!coop_mapmark_ok(&b));
      b=g; b.pos[0]=NAN; assert(!coop_mapmark_ok(&b));
      b=g; b.pos[2]=-20000.0f; assert(!coop_mapmark_ok(&b));
      b=g; b.pos[1]=INFINITY; assert(!coop_mapmark_ok(&b));
      memcpy(&got,&g,sizeof g); assert(got.no==31 && got.pos[2]==g.pos[2]); }
    { CoopShrineRide e={0xf7d0622fu,{-673.2f,173.1f,1513.0f}}, got;
      assert(COOP_VERSION==101 && sizeof e==16 && coop_shrine_ride_ok(&e));
      assert(!coop_net_send_event(COOP_EV_SHRINE_RIDE,&e,sizeof e));
      p.seq++; deliver(sender,&target,&p,0);
      assert(coop_net_send_event_to(partner_pid,COOP_EV_SHRINE_RIDE,&e,sizeof e));
      bool seen=false;
      for (int i=0;i<40 && !seen;i++) seen=recv_events(sender,&vp,1) && vp.e[0].type==COOP_EV_SHRINE_RIDE;
      assert(seen && vp.e[0].len==sizeof e);
      memcpy(&got,vp.e[0].data,sizeof got); assert(!memcmp(&got,&e,sizeof e));
      assert(!shrine_payload_ok(COOP_EV_SHRINE_RIDE,&e,sizeof e - 1));
      got=e; got.hash=0; assert(!coop_shrine_ride_ok(&got));
      got=e; got.pos[0]=NAN; assert(!coop_shrine_ride_ok(&got));
      got=e; got.pos[1]=INFINITY; assert(!coop_shrine_ride_ok(&got));
      got=e; got.pos[2]=-10001; assert(!coop_shrine_ride_ok(&got));
      CoopShrineDoor door={38,{-667.3f,173.1f,1515.2f}};
      assert(coop_shrine_door_ok(&door));
      assert(!coop_net_send_event(COOP_EV_SHRINE_DOOR,&door,sizeof door));
      assert(!shrine_payload_ok(COOP_EV_SHRINE_DOOR,&door,sizeof door + 1));
      CoopShrineDoor bad=door; bad.dungeon=COOP_SHRINE_MAX+1;
      assert(!coop_shrine_door_ok(&bad));
      in.count=2;
      in.e[0]=(CoopEvent){.id=9,.type=COOP_EV_SHRINE_DOOR,.len=sizeof bad};
      in.e[1]=(CoopEvent){.id=10,.type=COOP_EV_SHRINE_DOOR,.len=sizeof door};
      memcpy(in.e[0].data,&bad,sizeof bad);
      memcpy(in.e[1].data,&door,sizeof door);
      sendto(sender,(char *)&in,sizeof in,0,(struct sockaddr *)&target,sizeof target); Sleep(150);
      assert(coop_net_next_event(&type,data,&n) && type==COOP_EV_SHRINE_DOOR && n==sizeof door);
      assert(!memcmp(data,&door,sizeof door) && !coop_net_next_event(&type,data,&n));
      door.dungeon=0; assert(coop_shrine_door_ok(&door));
      door.dungeon=COOP_SHRINE_DOOR_SCENE; assert(coop_shrine_door_ok(&door));  // (0.1.1: "my door scene started")
      door.dungeon=COOP_SHRINE_DOOR_SCENE-1; assert(!coop_shrine_door_ok(&door)); door.dungeon=0;
      door.pos[2]=NAN; assert(!coop_shrine_door_ok(&door)); }
    // Run-time enemies: SPAWNED entries listed, names validated.
    CoopEnemyPacket ep={0}; ep.magic=COOP_ENEMY_MAGIC; ep.version=COOP_VERSION; ep.session=301; ep.echo=local_session;
    ep.seq=5; ep.count=2;
    ep.e[0]=(CoopEnemyDamage){0x80000005u,3,COOP_ENEMY_SPAWNED|COOP_ENEMY_LOADED};
    ep.e[1]=(CoopEnemyDamage){12345,0,COOP_ENEMY_LOADED};
    ep.names[0]=(CoopSpawnName){0x80000005u,"Enemy_Keese_Fire"}; ep.names[1]=(CoopSpawnName){0x80000006u,"../Enemy_x"};
    sendto(sender,(char *)&ep,sizeof ep,0,(struct sockaddr *)&target,sizeof target); Sleep(150);
    CoopEnemyDamage rem[8]; double heard[8]; char name[64];
    assert(coop_net_remote_spawned(rem,heard,8)==1 && rem[0].hash==0x80000005u && rem[0].damage==3 && heard[0]<1);
    assert(coop_net_remote_spawn_name(0x80000005u,name,sizeof name) && !strcmp(name,"Enemy_Keese_Fire"));
    assert(!coop_net_remote_spawn_name(0x80000006u,name,sizeof name));
    // (84) Only a loaded snapshot can publish an element. A reordered packet cannot restore a cleared effect.
    ep.count=1; ep.seq=6;
    ep.e[0]=(CoopEnemyDamage){12345,0,COOP_ENEMY_BURNING};
    sendto(sender,(char *)&ep,sizeof ep,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
    assert(!coop_net_remote_enemy_elem(12345));
    ep.seq=7; ep.e[0].flags |= COOP_ENEMY_LOADED | COOP_ENEMY_FROZEN | COOP_ENEMY_SHOCKED | (1u << 31);
    sendto(sender,(char *)&ep,sizeof ep,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
    assert(coop_net_remote_enemy_elem(12345)==COOP_ENEMY_ELEM_MASK);
    uint32_t elem_flags; double elem_age;
    assert(coop_net_remote_enemy_flags(12345,&elem_flags,&elem_age) &&
           elem_flags==(COOP_ENEMY_LOADED|COOP_ENEMY_ELEM_MASK));
    CoopEnemyDamage local_enemy={.hash=12345};  // remembered on the host, currently loaded on the guest only
    coop_net_set_enemies(&local_enemy,1);
    pthread_mutex_lock(&lock);
    CoopEnemyPacket relayed={0};
    uint64_t elem_now=plat_now_ns();
    build_enemies_for(2,&relayed,elem_now);
    assert(relayed.count>=1 && relayed.e[0].hash==12345 &&
           (relayed.e[0].flags & COOP_ENEMY_ELEM_MASK)==COOP_ENEMY_ELEM_MASK);
    build_enemies_for(1,&relayed,elem_now);
    assert(relayed.count>=1 && relayed.e[0].hash==12345 && !(relayed.e[0].flags & COOP_ENEMY_ELEM_MASK));
    build_enemies_for(2,&relayed,elem_now+COOP_ENEMY_ELEM_FRESH_NS);
    assert(relayed.count>=1 && relayed.e[0].hash==12345 && !(relayed.e[0].flags & COOP_ENEMY_ELEM_MASK));
    pthread_mutex_unlock(&lock);
    coop_net_set_enemies(NULL,0);
    ep.seq=8; ep.e[0].flags=COOP_ENEMY_LOADED;
    sendto(sender,(char *)&ep,sizeof ep,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
    ep.seq=7; ep.e[0].flags |= COOP_ENEMY_BURNING;
    sendto(sender,(char *)&ep,sizeof ep,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
    assert(!coop_net_remote_enemy_elem(12345));
    // (protocol 55) enemy look-at flags kept, unknown bits dropped
    { ep.seq=6; ep.count=1;
      ep.e[0]=(CoopEnemyDamage){12346,0,COOP_ENEMY_LOADED|COOP_ENEMY_POS|COOP_ENEMY_LOOK|COOP_ENEMY_LOOK_PARTNER|COOP_ENEMY_MTXPOS|256u};
      ep.names[0]=(CoopSpawnName){0x80000007u,"Animal_Boar_A"}; ep.names[1]=(CoopSpawnName){0x80000008u,"Bird_x"};
      sendto(sender,(char *)&ep,sizeof ep,0,(struct sockaddr *)&target,sizeof target); Sleep(150);
      uint32_t fl=0; double fage=99;
      assert(coop_net_remote_enemy_flags(12346,&fl,&fage) &&
             fl==(COOP_ENEMY_LOADED|COOP_ENEMY_POS|COOP_ENEMY_LOOK|COOP_ENEMY_LOOK_PARTNER|COOP_ENEMY_MTXPOS));  // (56: MTXPOS kept)
      // (protocol 55) run-time animal names pass (SPAWNANIMALS), other kinds do not
      assert(coop_net_remote_spawn_name(0x80000007u,name,sizeof name) && !strcmp(name,"Animal_Boar_A"));
      assert(!coop_net_remote_spawn_name(0x80000008u,name,sizeof name)); }
    // A restarted peer: its event ids start at 1 again.
    p.session=302; p.seq=1; deliver(sender,&target,&p,0);
    in.session=302; in.count=1; in.ack=0; in.e[0]=(CoopEvent){.id=1,.type=240,.len=0};
    sendto(sender,(char *)&in,sizeof in,0,(struct sockaddr *)&target,sizeof target); Sleep(150);
    assert(coop_net_next_event(&type,data,&n) && type==240);
    assert(!coop_net_remote_spawn_name(0x80000005u,name,sizeof name));  // (names of the old session dropped)
    // (protocol 31) area effects (bomb blast): the receiver's validation
    { CoopBeastRot r={2030243123u,0x1234u,-0.349f}; assert(sizeof r==12 && coop_beast_rot_ok(&r));
      r.angle=NAN; assert(!coop_beast_rot_ok(&r)); r.angle=7; assert(!coop_beast_rot_ok(&r));
      r.angle=0; r.hash=0; assert(!coop_beast_rot_ok(&r)); }
    // (protocol 41) motion-control platforms: the receiver's validation
    { CoopGyro g={3281110075u,0x1234u,7,COOP_GYRO_DRIVING,0,{1,0,0, 0,0.8f,-0.6f, 0,0.6f,0.8f}};
      assert(sizeof g==48 && coop_gyro_ok(&g));
      g.flags=0; assert(coop_gyro_ok(&g)); g.flags=2; assert(!coop_gyro_ok(&g)); g.flags=0;
      g.pad=1; assert(!coop_gyro_ok(&g)); g.pad=0;
      g.rot[4]=NAN; assert(!coop_gyro_ok(&g)); g.rot[4]=2; assert(!coop_gyro_ok(&g)); g.rot[4]=0.1f; assert(!coop_gyro_ok(&g));
      g.rot[4]=0.8f; g.area=0; assert(!coop_gyro_ok(&g)); g.area=1; g.hash=0; assert(!coop_gyro_ok(&g)); }
    // (protocol 54) trees cut down: the receiver's validation
    { CoopTree e={0x1234u,0x99u,{0.6f,0,-0.8f},1,{0,0,0}};
      assert(sizeof e==24 && sizeof e<=COOP_EVENT_DATA && coop_tree_ok(&e));
      e.hash=0; assert(!coop_tree_ok(&e)); e.hash=5; e.area=0; assert(!coop_tree_ok(&e)); e.area=1;
      e.dir[0]=NAN; assert(!coop_tree_ok(&e)); e.dir[0]=2; assert(!coop_tree_ok(&e)); e.dir[0]=0;
      e.instant=2; assert(!coop_tree_ok(&e)); e.instant=0; e.pad[2]=1; assert(!coop_tree_ok(&e)); e.pad[2]=0;
      assert(coop_tree_ok(&e)); }
    // (protocol 67) felled trunks: the receiver's validation
    { CoopTrunk e={0x1234u,0x99u,1,COOP_TRUNK_REST,0,{1,2,3},{0,0,0,1},{0,-1,0}};
      assert(sizeof e==52 && sizeof e<=COOP_EVENT_DATA && coop_trunk_ok(&e));
      e.hash=0; assert(!coop_trunk_ok(&e)); e.hash=5; e.area=0; assert(!coop_trunk_ok(&e)); e.area=1;
      e.gen=0; assert(!coop_trunk_ok(&e)); e.gen=2; e.flags=4; assert(!coop_trunk_ok(&e)); e.flags=0;
      e.pad=1; assert(!coop_trunk_ok(&e)); e.pad=0; e.pos[0]=NAN; assert(!coop_trunk_ok(&e)); e.pos[0]=1;
      e.rot[3]=0.5f; assert(!coop_trunk_ok(&e)); e.rot[3]=1; e.vel[2]=99; assert(!coop_trunk_ok(&e)); e.vel[2]=0;
      assert(coop_trunk_ok(&e));
      CoopTrunk g={7,9,3,COOP_TRUNK_GONE,0,{NAN},{0},{0}}; assert(coop_trunk_ok(&g)); }
    { CoopCritter e={0x1234u,0x99u,{1.0f,2.0f,-3.0f}};  // (protocol 56)
      assert(sizeof e==20 && sizeof e<=COOP_EVENT_DATA && coop_critter_ok(&e));
      e.hash=0; assert(!coop_critter_ok(&e)); e.hash=5; e.area=0; assert(!coop_critter_ok(&e)); e.area=1;
      e.pos[1]=NAN; assert(!coop_critter_ok(&e)); e.pos[1]=200000.0f; assert(!coop_critter_ok(&e)); e.pos[1]=0;
      assert(coop_critter_ok(&e)); }
    { CoopChestShut e={0x1234u,0x99u,{1.0f,2.0f,-3.0f},0};  // (protocol 75)
      assert(sizeof e==24 && sizeof e<=COOP_EVENT_DATA && coop_chest_shut_ok(&e));
      e.hash=0; assert(!coop_chest_shut_ok(&e)); e.hash=5; e.area=0; assert(!coop_chest_shut_ok(&e)); e.area=1;
      e.pos[2]=INFINITY; assert(!coop_chest_shut_ok(&e)); e.pos[2]=-200000.0f; assert(!coop_chest_shut_ok(&e));
      e.pos[2]=0; e.flags=1; assert(!coop_chest_shut_ok(&e)); e.flags=0; assert(coop_chest_shut_ok(&e)); }
    // (protocol 43) Cryonis ice blocks: the receiver's validation
    { CoopIce e={COOP_ICE_ID|0x40000001u,0x1234u,COOP_ICE_CREATE,{0,0,0},{0.8f,0,-0.6f,12.5f, 0,1,0,-3, 0.6f,0,0.8f,1500}};
      assert(sizeof e==60 && sizeof e<=COOP_EVENT_DATA && coop_ice_ok(&e));
      e.m[3]=NAN; assert(!coop_ice_ok(&e)); e.m[3]=200000; assert(!coop_ice_ok(&e)); e.m[3]=12.5f;
      e.m[5]=0.2f; assert(!coop_ice_ok(&e)); e.m[5]=NAN; assert(!coop_ice_ok(&e)); e.m[5]=1;
      e.pad[1]=1; assert(!coop_ice_ok(&e)); e.pad[1]=0;
      e.kind=3; assert(!coop_ice_ok(&e)); e.kind=0; assert(!coop_ice_ok(&e)); e.kind=COOP_ICE_CREATE;
      e.id=5; assert(!coop_ice_ok(&e)); e.id=COOP_ICE_ID|1; e.area=0; assert(!coop_ice_ok(&e)); e.area=1;
      CoopIce g={COOP_ICE_ID|7,9,COOP_ICE_GONE,{0,0,0},{0}}; assert(coop_ice_ok(&g));  // (gone: no matrix needed)
      g.id=7; assert(!coop_ice_ok(&g)); }
    { CoopAreaFx a={COOP_AREAFX_BOMB,0,8,{1,2,3},{0,0,0},4.5f}; assert(sizeof a==32 && coop_areafx_ok(&a));
      a.dir[0]=1; assert(coop_areafx_ok(&a));
      CoopAreaFx b=a; b.kind=6; assert(!coop_areafx_ok(&b)); b.kind=0; assert(!coop_areafx_ok(&b));
      // (protocol 35: kind 1 = an arrow hit, flags = the arrow type 0..5; other kinds keep flags 0)
      b=a; b.kind=COOP_AREAFX_ARROW; b.radius=0; assert(coop_areafx_ok(&b));
      b.flags=COOP_ARROW_SHOCK; assert(coop_areafx_ok(&b)); b.flags=COOP_ARROW_TYPES; assert(!coop_areafx_ok(&b));
      b.flags=255; assert(!coop_areafx_ok(&b)); b=a; b.flags=COOP_ARROW_FIRE; assert(!coop_areafx_ok(&b));
      b=a; b.kind=COOP_AREAFX_THROW; b.radius=0; assert(coop_areafx_ok(&b));  // (protocol 32: a thrown weapon's hit)
      b=a; b.kind=COOP_AREAFX_FOE_ARROW; b.radius=0; b.flags=COOP_ARROW_FIRE; assert(coop_areafx_ok(&b));  // (protocol 50: an enemy's arrow)
      b.flags=COOP_ARROW_TYPES; assert(!coop_areafx_ok(&b));
      b=a; b.kind=COOP_AREAFX_FOE_SHOT; b.radius=0; b.flags=COOP_SHOT_FIRE; assert(coop_areafx_ok(&b));  // (protocol 51: an enemy's shot)
      b.flags=COOP_SHOT_BOMB|COOP_SHOT_KNOCK; assert(coop_areafx_ok(&b)); b.flags=COOP_SHOT_TYPES; assert(!coop_areafx_ok(&b));
      b.flags=0x20; assert(!coop_areafx_ok(&b)); b.kind=6; b.flags=0; assert(!coop_areafx_ok(&b));
      b=a; b.flags=1; assert(!coop_areafx_ok(&b));
      b=a; b.damage=-1; assert(!coop_areafx_ok(&b)); b.damage=401; assert(!coop_areafx_ok(&b));
      b=a; b.pos[1]=NAN; assert(!coop_areafx_ok(&b)); b=a; b.pos[2]=1e6f; assert(!coop_areafx_ok(&b));
      b=a; b.dir[2]=INFINITY; assert(!coop_areafx_ok(&b)); b=a; b.dir[0]=2; assert(!coop_areafx_ok(&b));
      b=a; b.radius=21; assert(!coop_areafx_ok(&b)); b.radius=-1; assert(!coop_areafx_ok(&b)); b.radius=NAN; assert(!coop_areafx_ok(&b)); }
    // (protocol 32) dropped items: the receiver's validation
    { CoopDrop d={0}; d.id=COOP_DROP_ID|7; d.area=99; d.pos[0]=3320.5f; d.pos[1]=255.8f; d.pos[2]=-497.8f; d.yaw=10; d.life=1500;
      strcpy(d.name,"Weapon_Sword_002"); assert(sizeof d==80 && sizeof d<=COOP_EVENT_DATA && coop_drop_ok(&d));
      { CoopDrop f=d; f.flags=COOP_DROP_F_MOVING; assert(coop_drop_ok(&f)); f.flags=COOP_DROP_F_UPDATE; assert(coop_drop_ok(&f));  // (34)
        f.flags=COOP_DROP_F_MOVING|COOP_DROP_F_UPDATE; assert(coop_drop_ok(&f)); f.flags=4; assert(!coop_drop_ok(&f));
        f.flags=0; f.pad[1]=1; assert(!coop_drop_ok(&f)); }
      CoopDrop e=d; strcpy(e.name,"Item_Fruit_A"); e.life=-1; assert(coop_drop_ok(&e));
      e=d; strcpy(e.name,"Obj_FireWoodBundle"); assert(coop_drop_ok(&e));
      e=d; e.id=7; assert(!coop_drop_ok(&e));
      e=d; strcpy(e.name,"Enemy_Lynel_Gold"); assert(!coop_drop_ok(&e));
      e=d; strcpy(e.name,"Animal_Insect_A"); assert(coop_drop_ok(&e)); strcpy(e.name,"BeeHome"); assert(coop_drop_ok(&e));  // (33)
      e=d; strcpy(e.name,"Animal_Boar_A"); assert(!coop_drop_ok(&e));
      e=d; strcpy(e.name,"Weapon_../x"); assert(!coop_drop_ok(&e));
      e=d; e.name[0]=0; assert(!coop_drop_ok(&e));
      e=d; memset(e.name,'A',sizeof e.name); memcpy(e.name,"Item_",5); assert(!coop_drop_ok(&e));  // (not terminated)
      e=d; e.life=-2; assert(!coop_drop_ok(&e)); e.life=10000001; assert(!coop_drop_ok(&e));
      e=d; e.pos[1]=NAN; assert(!coop_drop_ok(&e)); e=d; e.pos[0]=-1e6f; assert(!coop_drop_ok(&e));
      e=d; e.yaw=INFINITY; assert(!coop_drop_ok(&e)); e.yaw=1000; assert(!coop_drop_ok(&e));
      CoopDropGone g={COOP_DROP_ID|7,COOP_DROP_PICKED,{0,0,0}}; assert(sizeof g==8 && coop_drop_gone_ok(&g));
      g.reason=COOP_DROP_VANISHED; assert(coop_drop_gone_ok(&g));
      CoopDropGone h=g; h.reason=0; assert(!coop_drop_gone_ok(&h)); h.reason=3; assert(!coop_drop_gone_ok(&h));
      h=g; h.id=5; assert(!coop_drop_gone_ok(&h)); h=g; h.pad[1]=1; assert(!coop_drop_gone_ok(&h)); }
    // (protocol 33) remote bombs in the state packet: the receiver's validation
    { CoopBomb b={COOP_BOMB_ROUND,{0,0,0},{-845.5f,200.2f,1803.1f},{1,0,0,0,1,0,0,0,1},{3.5f,4.0f,-2.0f}}; assert(sizeof b==64 && coop_bomb_ok(&b));
      { CoopBomb v=b; v.vel[1]=NAN; assert(!coop_bomb_ok(&v)); v=b; v.vel[0]=101; assert(!coop_bomb_ok(&v)); }  // (34)
      CoopBomb z={0}; assert(coop_bomb_ok(&z));  // (an empty slot)
      CoopBomb e=b; e.kind=COOP_BOMB_CUBE; assert(coop_bomb_ok(&e)); e.kind=3; assert(!coop_bomb_ok(&e));
      e=b; e.pad[2]=1; assert(!coop_bomb_ok(&e));
      e=b; e.pos[0]=NAN; assert(!coop_bomb_ok(&e)); e=b; e.pos[2]=2e5f; assert(!coop_bomb_ok(&e));
      e=b; e.rot[4]=INFINITY; assert(!coop_bomb_ok(&e)); e=b; e.rot[8]=-2; assert(!coop_bomb_ok(&e)); }
    // (39, coop-polish) the rider's horse: the receiver's validation
    { CoopHorse h={0}; assert(sizeof h==296 && coop_horse_ok(&h));  // (no horse)
      CoopHorse e=h; e.mtx[0]=1; assert(!coop_horse_ok(&e));       // (no horse but data)
      h.flags=COOP_HORSE_SHOWN; snprintf(h.body,sizeof h.body,"GameRomHorseBone_AllDay"); h.mtx[0]=h.mtx[5]=h.mtx[10]=1;
      h.rel[0]=h.rel[5]=h.rel[10]=1; assert(coop_horse_ok(&h));
      e=h; e.body[0]=0; assert(!coop_horse_ok(&e));
      e=h; memset(e.body,'A',sizeof e.body); assert(!coop_horse_ok(&e));  // unterminated
      e=h; e.flags|=32; assert(!coop_horse_ok(&e));
      e=h; e.flags|=COOP_HORSE_HAND_L|COOP_HORSE_HAND_R; assert(coop_horse_ok(&e));  // (44)
      e=h; e.coat=200; e.socks=(uint8_t)(4<<5|31); assert(coop_horse_ok(&e));  // (45) coat / socks
      e=h; e.socks=(uint8_t)(5<<5); assert(!coop_horse_ok(&e));      // (45) colour kind out of range
      e=h; e.mtx[11]=NAN; assert(!coop_horse_ok(&e)); e=h; e.mtx[3]=2e5f; assert(!coop_horse_ok(&e));
      e=h; e.mtx[1]=2; assert(!coop_horse_ok(&e)); e=h; e.vel[2]=21; assert(!coop_horse_ok(&e));
      e=h; e.n_in=COOP_HORSE_INPUTS+1; assert(!coop_horse_ok(&e));
      e=h; e.in_kind[0]=3; assert(!coop_horse_ok(&e));                 // (a value past n_in)
      e=h; e.n_in=1; e.in_kind[0]=21; assert(!coop_horse_ok(&e));      // (vec3 kind)
      e=h; e.n_in=1; e.in_kind[0]=66; assert(!coop_horse_ok(&e));      // (bools kind: own field)
      e=h; e.n_in=1; e.in_kind[0]=2; e.in_val[0]=0x7fc00000u; assert(!coop_horse_ok(&e));  // (float NaN)
      e=h; e.n_in=1; e.in_kind[0]=43; e.in_val[0]=0x7fc00000u; assert(coop_horse_ok(&e));  // (int kind: any value)
      e=h; e.seq[5]=255; e.as[5]=COOP_HORSE_AS_COUNT-1; assert(coop_horse_ok(&e));
      e=h; e.as[0]=COOP_HORSE_AS_COUNT; assert(!coop_horse_ok(&e));
      // (92, coop-horse2) other mounts in the same struct: a sand seal (not seated), Sidon / the Master Cycle (seated)
      e=h; memset(e.body,0,sizeof e.body); snprintf(e.body,sizeof e.body,"Animal_SunazarashiSP_A"); e.as[0]=47; assert(coop_horse_ok(&e));
      e=h; memset(e.body,0,sizeof e.body); snprintf(e.body,sizeof e.body,"GameROMMotorcycle"); e.flags|=COOP_HORSE_RIDDEN; assert(coop_horse_ok(&e)); }
    // (protocol 86, coop-water) shrine water wheels, dynamic water levels, timed switches
    { CoopWaterPuz w={0}; w.hash=3848708082u; w.area=0x12345678u; w.gen=1; w.kind=COOP_WP_WHEEL;
      w.flags=COOP_WP_F_ON|COOP_WP_F_ACTIVE; w.pos[0]=-8.75f; w.pos[1]=6.5f; w.pos[2]=21.0f;
      w.rot[3]=1.0f; w.val[0]=1.2f;
      assert(sizeof w==52 && sizeof w<=COOP_EVENT_DATA && coop_waterpuz_ok(&w));
      { CoopWaterPuz got;
        in.count=1; in.e[0]=(CoopEvent){.id=2,.type=COOP_EV_WATERPUZ,.len=sizeof w};
        memcpy(in.e[0].data,&w,sizeof w);
        sendto(sender,(char *)&in,sizeof in,0,(struct sockaddr *)&target,sizeof target); Sleep(150);
        assert(coop_net_next_event(&type,data,&n) && type==COOP_EV_WATERPUZ && n==sizeof w);
        memcpy(&got,data,sizeof got);
        assert(!memcmp(&w,&got,sizeof w) && coop_waterpuz_ok(&got)); }
      CoopWaterPuz b=w; b.hash=0; assert(!coop_waterpuz_ok(&b));
      b=w; b.area=0; assert(!coop_waterpuz_ok(&b));
      b=w; b.kind=0; assert(!coop_waterpuz_ok(&b));
      b=w; b.kind=COOP_WP_KIND_MAX+1; assert(!coop_waterpuz_ok(&b));
      b=w; b.flags=4; assert(!coop_waterpuz_ok(&b));
      b=w; b.pos[0]=NAN; assert(!coop_waterpuz_ok(&b));
      b=w; b.pos[1]=200000.0f; assert(!coop_waterpuz_ok(&b));
      b=w; b.rot[3]=0.0f; assert(!coop_waterpuz_ok(&b));           // non-unit quaternion
      b=w; b.rot[0]=INFINITY; assert(!coop_waterpuz_ok(&b));
      b=w; b.val[0]=51.0f; assert(!coop_waterpuz_ok(&b));          // angvel out of range
      b=w; b.val[2]=NAN; assert(!coop_waterpuz_ok(&b));
      CoopWaterPuz s={0}; s.hash=4014884288u; s.area=0x12345678u; s.kind=COOP_WP_WATER;
      s.flags=COOP_WP_F_ON|COOP_WP_F_ACTIVE; s.pos[1]=4.2f; s.rot[3]=1.0f; s.val[0]=7.75f; s.val[1]=0.8f;
      assert(coop_waterpuz_ok(&s));
      b=s; b.pos[0]=1.0f; assert(!coop_waterpuz_ok(&b));            // water uses pos[1] only
      b=s; b.rot[0]=1.0f; b.rot[3]=0.0f; assert(!coop_waterpuz_ok(&b));
      b=s; b.val[1]=-1.0f; assert(!coop_waterpuz_ok(&b));          // negative speed
      b=s; b.val[1]=101.0f; assert(!coop_waterpuz_ok(&b));         // speed out of range
      b=s; b.val[2]=1.0f; assert(!coop_waterpuz_ok(&b));           // unused val[2] must be 0
      CoopWaterPuz t={0}; t.hash=4127325551u; t.area=0x12345678u; t.kind=COOP_WP_TIMELAG;
      t.flags=COOP_WP_F_ON; t.rot[3]=1.0f; t.val[0]=10.0f; t.val[1]=15.0f;
      assert(coop_waterpuz_ok(&t));
      b=t; b.kind=COOP_WP_TIMER; assert(coop_waterpuz_ok(&b));
      b=t; b.kind=COOP_WP_LIMITED; assert(coop_waterpuz_ok(&b));
      b=t; b.pos[1]=1.0f; assert(!coop_waterpuz_ok(&b));            // timer pos must be 0
      b=t; b.val[0]=-0.1f; assert(!coop_waterpuz_ok(&b));           // negative elapsed
      b=t; b.val[1]=0.0f; assert(!coop_waterpuz_ok(&b));            // wait_time must be > 0
      b=t; b.val[1]=10001.0f; assert(!coop_waterpuz_ok(&b));
      b=t; b.val[2]=1.0f; assert(!coop_waterpuz_ok(&b));
      assert(!ev_pairwise(COOP_EV_WATERPUZ, (const uint8_t *)&w, (int)sizeof w)); }
    { CoopLightning strike={plat_now_ns(),0x1234u,{-834,199,1812}}, bad=strike;
      assert(coop_lightning_ok(&strike) && sizeof strike==24);
      bad.at_ns=0; assert(!coop_lightning_ok(&bad));
      bad=strike; bad.area=0; assert(!coop_lightning_ok(&bad));
      bad=strike; bad.pos[1]=NAN; assert(!coop_lightning_ok(&bad));
      bad=strike; bad.pos[2]=100001; assert(!coop_lightning_ok(&bad));
      assert(!coop_net_send_event_to(1,COOP_EV_LIGHTNING,&bad,sizeof bad));
      assert(coop_net_send_event_to(1,COOP_EV_LIGHTNING,&strike,sizeof strike));
      assert(recv_events(sender,&vp,1) && vp.e[0].type==COOP_EV_LIGHTNING &&
             vp.e[0].len==sizeof strike && !memcmp(vp.e[0].data,&strike,sizeof strike));
      in.e[0]=(CoopEvent){.id=3,.type=COOP_EV_LIGHTNING,.len=sizeof strike};
      memcpy(in.e[0].data,&strike,sizeof strike);
      sendto(sender,(char *)&in,sizeof in,0,(struct sockaddr *)&target,sizeof target); Sleep(150);
      assert(!coop_net_next_event(&type,data,&n)); }  // host discards guest-originated strikes
    { CoopDishOffer offer = {.from=in.session,.to=local_session,.id=1,.area=99,.from_pid=(uint8_t)partner_pid,.to_pid=0,
                            .dish={"Item_Cook_A_01",16,150,30,6,2.0f,{0x12ab,0,0,0,0}}};
      assert(coop_dish_offer_ok(&offer) && sizeof offer==96 && ev_pairwise(COOP_EV_DISH,NULL,0));
      CoopDishOffer bad=offer; memset(bad.dish.name,'x',sizeof bad.dish.name); assert(!coop_dish_offer_ok(&bad));
      bad=offer; bad.dish.name[11]='/'; assert(!coop_dish_offer_ok(&bad));
      bad=offer; bad.dish.hearts=-1; assert(!coop_dish_offer_ok(&bad));
      bad=offer; bad.dish.seconds=1801; assert(!coop_dish_offer_ok(&bad));
      bad=offer; bad.dish.effect=100; assert(!coop_dish_offer_ok(&bad));
      bad=offer; bad.dish.level=INFINITY; assert(!coop_dish_offer_ok(&bad));
      bad=offer; bad.dish.price=-1; assert(!coop_dish_offer_ok(&bad));
      bad=offer; bad.area=0; assert(!coop_dish_offer_ok(&bad));
      bad=offer; bad.pad[1]=1; assert(!coop_dish_offer_ok(&bad));                         // (DISHINV) ids
      bad=offer; bad.to_pid=bad.from_pid; assert(!coop_dish_offer_ok(&bad));
      bad=offer; bad.to_pid=COOP_MAX_PLAYERS; assert(!coop_dish_offer_ok(&bad));
      assert(!coop_net_send_event(COOP_EV_DISH,&offer,sizeof offer));  // never broadcast
      assert(!coop_net_send_event_to(partner_pid,COOP_EV_DISH,&offer,sizeof offer));  // reversed session identities
      bad=offer; bad.to_pid=3;  // (for another player: not ours)
      in.e[0]=(CoopEvent){.id=wl[partner_pid].in_last+1,.type=COOP_EV_DISH,.len=sizeof bad};
      memcpy(in.e[0].data,&bad,sizeof bad);
      sendto(sender,(char *)&in,sizeof in,0,(struct sockaddr *)&target,sizeof target); Sleep(150);
      assert(!coop_net_next_event(&type,data,&n));
      in.e[0]=(CoopEvent){.id=wl[partner_pid].in_last+1,.type=COOP_EV_DISH,.len=sizeof offer};
      memcpy(in.e[0].data,&offer,sizeof offer);
      sendto(sender,(char *)&in,sizeof in,0,(struct sockaddr *)&target,sizeof target); Sleep(150);
      sendto(sender,(char *)&in,sizeof in,0,(struct sockaddr *)&target,sizeof target); Sleep(150);
      assert(coop_net_next_event(&type,data,&n) && type==COOP_EV_DISH && n==sizeof offer && !memcmp(data,&offer,n));
      assert(!coop_net_next_event(&type,data,&n));
      in.e[0].id++; in.e[0].type=(uint16_t)(COOP_EV_DISH|(2<<8));
      sendto(sender,(char *)&in,sizeof in,0,(struct sockaddr *)&target,sizeof target); Sleep(150);
      assert(!coop_net_next_event(&type,data,&n));
      CoopDishResult result={.from=local_session,.to=in.session,.id=offer.id,.accepted=1,.from_pid=0,.to_pid=(uint8_t)partner_pid};
      assert(coop_dish_result_ok(&result) && sizeof result==32 && ev_pairwise(COOP_EV_DISH_RESULT,NULL,0));
      result.accepted=2; assert(!coop_dish_result_ok(&result));
      result.accepted=1; assert(!coop_net_send_event(COOP_EV_DISH_RESULT,&result,sizeof result));
      assert(coop_net_send_event_to(partner_pid,COOP_EV_DISH_RESULT,&result,sizeof result));
      bool seen=false;
      for (int i=0;i<40 && !seen;i++) {
          if (!recv_events(sender,&vp,1)) continue;
          for (int j=0;j<vp.count;j++)
              if (vp.e[j].type==COOP_EV_DISH_RESULT && vp.e[j].len==sizeof result && !memcmp(vp.e[j].data,&result,sizeof result))
                  seen=true;
      }
      assert(seen); }
    printf("PASS events: order, gaps, duplicates, acks, length check, restart; run-time enemy names; area effects; dropped items; bombs; waterpuz; lightning; cooked dishes\n");
    closesocket(sender);
    return 0;
}
// `test_coop_net multi` (protocol 56): a host with two guests. Player ids, the partner role, relay of a guest's state /
// tree to the other guest with its id, world data only from the partner, per-player views, the partner hand-over.
// `test_coop_net multi-guest`: a guest told it is an extra player: no partner, views by origin, no events.
static int recv_kind(SOCKET s, void *out, int size, uint32_t magic, int (*want)(const void *)) {
    for (int tries = 0; tries < 60; tries++) {
        unsigned char wire[MAX_DATAGRAM + COOP_TAG + 1];
        DWORD tmo = 200; setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,(char *)&tmo,sizeof tmo);
        int n = recv(s,(char *)wire,sizeof wire,0);
        if (n > 0) n = tag_check(wire,n);
        uint32_t m; if (n >= 4) memcpy(&m,wire,4);
        if (n == size && m == magic) { memcpy(out,wire,(size_t)size); if (!want || want(out)) return 1; }
    }
    return 0;
}
static int from_guest2(const void *v) { return ((const CoopPacket *)v)->origin == 2; }
static int from_host(const void *v) { return ((const CoopPacket *)v)->origin == 0; }
static int tree_from_guest2(const void *v) { return ((const CoopAsTreePacket *)v)->origin == 2; }
static SOCKET loop_socket(void) {
    SOCKET s = socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    struct sockaddr_in a = {0}; a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(!bind(s,(struct sockaddr *)&a,sizeof a));
    return s;
}
static int multi_main(void) {
    WSADATA w; assert(!WSAStartup(MAKEWORD(2,2),&w));
    SOCKET ga = loop_socket(), gb = loop_socket(), gc = loop_socket();
    struct sockaddr_in target;
    _putenv("SAVAGEWIND_COOP_PEER="); _putenv("SAVAGEWIND_COOP_PORT=0"); _putenv("SAVAGEWIND_COOP_CODE=");
    _putenv("SAVAGEWIND_COOP_MULTIWORLD=0");  // (58: this test is the phase-1 behaviour, the off switch)
    coop_net_init(); assert(coop_net_on());
    int len=sizeof target; getsockname(sock,(struct sockaddr *)&target,&len);
    target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(coop_net_my_id()==0 && coop_net_partner_id()==-1);
    CoopState me={0}; me.pos[0]=1; me.flags=COOP_FLAG_IN_WORLD; coop_net_set_local(&me);
    // guest A first: player 1, the world partner
    CoopPacket a={0}; a.magic=COOP_MAGIC; a.version=COOP_VERSION; a.flags=1; a.session=201; a.seq=1; a.pos[0]=111;
    deliver(ga,&target,&a,0);
    assert(coop_net_partner_id()==1 && count()==0);  // (admitted; no echo of ours yet)
    a.echo=local_session; a.seq=2; deliver(ga,&target,&a,0); assert(count()==1);
    // guest B: player 2, an extra guest
    CoopPacket b=a; b.session=202; b.echo=local_session; b.seq=1; b.pos[0]=222;
    deliver(gb,&target,&b,0); assert(count()==2 && coop_net_partner_id()==1);
    // the host's own state to each: its id, the role (partner only), who is connected
    CoopPacket r;
    assert(recv_kind(ga,&r,sizeof r,COOP_MAGIC,from_host));  // (an older one may still say only player 1)
    assert(r.your_id==1 && r.role==COOP_ROLE_PARTNER && r.echo==201 &&
           r.pos[0]==1 && (r.players&3)==3);
    assert(recv_kind(gb,&r,sizeof r,COOP_MAGIC,from_host) && r.your_id==2 && r.role==0 && r.echo==202 && r.pos[0]==1);
    // B's state is relayed to A with B's id and seq, as from the host (session) to A (echo); A's to B likewise
    b.seq=2; b.pos[0]=223; b.area=7; b.flags=COOP_FLAG_IN_WORLD | COOP_FLAG_MAGBEAM;
    b.magbeam_tip[0]=229.5f;
    b.stasisfx=(CoopStasisFx){.hash=830217036, .serial=7, .foe_ticks=45, .direction={0,0,-127}, .charge=25,
                             .flags=COOP_STASISFX_ARROW|COOP_STASISFX_VISIBLE};
    deliver(gb,&target,&b,0);
    { int seen=0; for (int i=0;i<10 && !seen;i++) seen=recv_kind(ga,&r,sizeof r,COOP_MAGIC,from_guest2) && r.seq==2;
      assert(seen && r.pos[0]==223 && r.session==local_session && r.echo==201 && r.your_id==1 && r.role==0);
      assert((r.flags & COOP_FLAG_MAGBEAM) && r.magbeam_tip[0]==229.5f);
      assert(!memcmp(&r.stasisfx,&b.stasisfx,sizeof r.stasisfx)); }
    a.seq=3; a.pos[0]=112; deliver(ga,&target,&a,0);
    { int seen=0; for (int i=0;i<60 && !seen;i++) { CoopPacket q; if (!recv_kind(gb,&q,sizeof q,COOP_MAGIC,NULL)) break;
        seen = q.origin==1 && q.pos[0]==112 && q.echo==202; } assert(seen); }
    // B's AS tree is relayed to A with B's id
    { CoopAsTreePacket ap={0}; ap.magic=COOP_ASTREE_MAGIC; ap.version=COOP_VERSION; ap.session=202; ap.echo=local_session;
      ap.seq=5; ap.t.flags=COOP_ASTREE_VALID; ap.t.normal[1]=1; ap.t.f[0]=0.7f; ap.t.as[0]=12; ap.t.str[0]=3;
      int ok_tree = coop_astree_ok(&ap.t); assert(ok_tree);
      sendto(gb,(char *)&ap,sizeof ap,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
      if (ok_tree) {
          CoopAsTreePacket rq;
          assert(recv_kind(ga,&rq,sizeof rq,COOP_ASTREE_MAGIC,tree_from_guest2) && rq.echo==201 && rq.seq==5);
          CoopAsTree rt; double age;
          assert(!coop_net_remote_astree(&rt,&age));  // (the partner sent none)
          coop_net_view(2); assert(coop_net_remote_astree(&rt,&age)); coop_net_view(-1);
      } else printf("(tree relay part skipped: no valid sample tree)\n"); }
    // views: the partner by default, another player with coop_net_view
    CoopState rs; double age;
    assert(coop_net_players(2.0)==((1u<<1)|(1u<<2)));
    assert(coop_net_get_remote(&rs,&age) && rs.pos[0]==112 && !rs.stasisfx.hash);
    coop_net_view(2); assert(coop_net_get_remote(&rs,&age) && rs.pos[0]==223 &&
                            !memcmp(&rs.stasisfx,&b.stasisfx,sizeof rs.stasisfx));
    coop_net_view(5); assert(!coop_net_get_remote(&rs,&age));
    coop_net_view(-1); assert(coop_net_get_remote(&rs,&age) && rs.pos[0]==112);
    // world data (objects) only from the partner
    { CoopObjPacket op={0}; CoopObject ro;
      op.magic=COOP_OBJ_MAGIC; op.version=COOP_VERSION; op.session=202; op.echo=local_session; op.seq=7; op.count=1;
      op.o[0].hash=77; op.o[0].flags=COOP_OBJ_OWNED; op.o[0].rot[3]=1;
      sendto(gb,(char *)&op,sizeof op,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
      assert(!coop_net_remote_object(77,&ro,&age));
      op.session=201; sendto(ga,(char *)&op,sizeof op,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
      assert(coop_net_remote_object(77,&ro,&age)); }
    // a bad id field rejects the packet
    b.seq=3; b.origin=COOP_MAX_PLAYERS; deliver(gb,&target,&b,0); assert(count()==4);
    b.origin=0; b.role=4; deliver(gb,&target,&b,0); assert(count()==4);
    b.role=0; b.weather=COOP_WEATHER_TYPES+1; deliver(gb,&target,&b,0); assert(count()==4);
    b.weather=1; deliver(gb,&target,&b,0); assert(count()==4);   // guests cannot send host weather even with origin 0
    b.weather=0; b.moon_day=COOP_MOON_DAY_ON; deliver(gb,&target,&b,0); assert(count()==4);
    b.moon_day=COOP_MOON_DAY_UNKNOWN;
    // a third guest: player 3
    CoopPacket c=b; c.session=203; c.seq=1; c.pos[0]=333;
    c.flags=COOP_FLAG_IN_WORLD; memset(c.magbeam_tip,0,sizeof c.magbeam_tip);
    c.stasisfx=(CoopStasisFx){0};
    deliver(gc,&target,&c,0); assert(count()==5);
    assert(coop_net_players(2.0)==((1u<<1)|(1u<<2)|(1u<<3)));
    // A silent for 5 s: the next guest heard is the partner; A comes back as an extra guest, keeping its id
    for (int i=0;i<11;i++) { b.seq++; deliver(gb,&target,&b,0); c.seq++; deliver(gc,&target,&c,0); Sleep(300); }
    assert(coop_net_partner_id()==2);
    assert(coop_net_get_remote(&rs,&age) && rs.pos[0]==223);
    a.seq++; deliver(ga,&target,&a,0); assert(coop_net_partner_id()==2);
    { int seen=0; for (int i=0;i<200 && !seen;i++) { if (!recv_kind(gb,&r,sizeof r,COOP_MAGIC,from_host)) break;
        seen = r.your_id==2 && r.role==COOP_ROLE_PARTNER; } assert(seen); }
    { int seen=0; for (int i=0;i<200 && !seen;i++) { if (!recv_kind(ga,&r,sizeof r,COOP_MAGIC,from_host)) break;
        seen = r.your_id==1 && r.role==0; } assert(seen); }
    printf("PASS multi: ids, partner role, relay of state and tree with the sender's id, views, world data from the "
           "partner only, id validation, third guest, partner hand-over\n");
    closesocket(ga); closesocket(gb); closesocket(gc);
    return 0;
}
static int multi_guest_main(void) {
    WSADATA w; assert(!WSAStartup(MAKEWORD(2,2),&w));
    SOCKET host = loop_socket();
    struct sockaddr_in hostaddr, target; int len=sizeof hostaddr; getsockname(host,(struct sockaddr *)&hostaddr,&len);
    char setting[100]; snprintf(setting,sizeof setting,"SAVAGEWIND_COOP_PEER=127.0.0.1:%d",ntohs(hostaddr.sin_port));
    _putenv(setting); _putenv("SAVAGEWIND_COOP_PORT=0"); _putenv("SAVAGEWIND_COOP_CODE=");
    coop_net_init(); assert(coop_net_on());
    len=sizeof target; getsockname(sock,(struct sockaddr *)&target,&len);
    target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(coop_net_partner_id()==0 && coop_net_my_id()==-1);
    CoopPacket h={0}; h.magic=COOP_MAGIC; h.version=COOP_VERSION; h.flags=1; h.session=401; h.echo=local_session;
    h.seq=1; h.pos[0]=10; h.your_id=2; h.role=0; h.players=7;
    h.weather=3; h.moon_day=COOP_MOON_DAY_ON; h.bloodmoon=11;
    deliver(host,&target,&h,0); assert(count()==1);
    assert(coop_net_my_id()==2 && coop_net_partner_id()==-1);
    CoopState rs; double age; uint64_t session;
    assert(!coop_net_get_remote(&rs,&age));            // no world partner: the world code plays alone
    assert(coop_net_get_host(&rs,&age,&session) && session==401 && age<1.0 &&
           rs.weather==3 && rs.moon_day==COOP_MOON_DAY_ON && rs.bloodmoon==11);
    assert(!coop_net_send_event(7,"abc",4));           // no events with the host
    coop_net_view(0); assert(coop_net_get_remote(&rs,&age) && rs.pos[0]==10);
    // another guest's state, relayed (origin 1, its own seq)
    CoopPacket o=h; o.origin=1; o.seq=500; o.pos[0]=20; o.your_id=2; o.players=0;
    deliver(host,&target,&o,0); assert(count()==1);    // a relayed guest cannot set host weather
    o.weather=o.bloodmoon=0;
    deliver(host,&target,&o,0); assert(count()==1);    // or a bloody day
    o.moon_day=COOP_MOON_DAY_UNKNOWN; o.bloodmoon=11;
    deliver(host,&target,&o,0); assert(count()==1);    // or a blood moon counter
    o.bloodmoon=0;
    deliver(host,&target,&o,0); assert(count()==2);
    coop_net_view(1); assert(coop_net_get_remote(&rs,&age) && rs.pos[0]==20);
    assert(coop_net_get_host(&rs,&age,&session) && rs.pos[0]==10 && rs.bloodmoon==11);
    o.seq=499; o.pos[0]=21; deliver(host,&target,&o,0); assert(count()==2);   // older seq of player 1
    o.seq=2; o.origin=2; o.pos[0]=30; deliver(host,&target,&o,0); assert(count()==2);  // our own id: ignored
    coop_net_view(-1);
    assert(coop_net_players(2.0)==((1u<<0)|(1u<<1)));
    // our own state carries our id
    CoopPacket r; CoopState me={0}; me.pos[0]=5; me.flags=COOP_FLAG_IN_WORLD; coop_net_set_local(&me);
    { int seen=0; for (int i=0;i<20 && !seen;i++) { if (!recv_kind(host,&r,sizeof r,COOP_MAGIC,NULL)) break;
        seen = r.origin==2 && r.pos[0]==5; } assert(seen); }
    // promoted to partner: the partner calls see the host again
    h.seq=2; h.role=COOP_ROLE_PARTNER; deliver(host,&target,&h,0);
    assert(coop_net_partner_id()==0 && coop_net_get_remote(&rs,&age) && rs.pos[0]==10 && coop_net_send_event(7,"abc",4));
    printf("PASS multi-guest: id from the host, extra guest (no partner, no events), views by origin, own id ignored, "
           "own id sent, promotion\n");
    closesocket(host);
    return 0;
}
#include "test_coop_multiworld.inc"  // (58) `multi-world`, `multi-world-guest`
#include "test_coop_multitarget.inc"  // (60) `multi-target`, `multi-target-guest`
#include "test_coop_multihandover.inc"  // (n6) `multi-handover`
#include "test_coop_dishmulti.inc"  // (DISHINV) `multi-dish`, `multi-dish-guest`, `utf8`
// `test_coop_net names` (protocol 62): player names and ping, both as the host and (after coop_net_stop, in the same
// process) as a guest started from a config (the co-op menu's way), plus the field checks.
static int names_has(const void *v) { const CoopNamePacket *p = v; return p->count >= 2 && !strcmp(p->e[1].name,"Pal"); }
static int names_main(void) {
    WSADATA w; assert(!WSAStartup(MAKEWORD(2,2),&w));
    CoopNetConfig room = {.mode = COOP_MODE_HOST, .code = " \t\r\n"};
    _putenv("SAVAGEWIND_COOP_BIND=");
    _putenv("SAVAGEWIND_COOP_OPEN=");
    assert(coop_net_start(&room));
    char generated[9];
    snprintf(generated, sizeof generated, "%s", coop_net_room_code());
    assert(strlen(coop_net_room_code()) == 8 && strspn(generated, "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567") == 8);
    coop_net_stop();
    assert(coop_net_start(&room) && strcmp(generated, coop_net_room_code()));
    coop_net_stop();
    _putenv("SAVAGEWIND_COOP_OPEN=1");
    assert(coop_net_start(&room) && !*coop_net_room_code());
    coop_net_stop();
    _putenv("SAVAGEWIND_COOP_OPEN=");
    _putenv("SAVAGEWIND_COOP_BIND=127.0.0.1");
    SOCKET ga = loop_socket();
    struct sockaddr_in target;
    _putenv("SAVAGEWIND_COOP_PEER="); _putenv("SAVAGEWIND_COOP_PORT=0"); _putenv("SAVAGEWIND_COOP_CODE=");
    _putenv("SAVAGEWIND_COOP_NAME=Hôst\tName");  // (cleaned: UTF-8 kept, tab -> '?')
    coop_net_init(); assert(coop_net_on() && coop_net_env_driven());
    int len=sizeof target; getsockname(sock,(struct sockaddr *)&target,&len);
    target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(!strcmp(local_name, "H\xc3\xb4st?Name"));
    CoopPacket a={0}; a.magic=COOP_MAGIC; a.version=COOP_VERSION; a.flags=1; a.session=301; a.seq=1;
    deliver(ga,&target,&a,0); a.echo=local_session; a.seq=2; deliver(ga,&target,&a,0); assert(count()==1);
    CoopNamePacket np={0}; np.magic=COOP_NAME_MAGIC; np.version=COOP_VERSION; np.session=301; np.echo=local_session;
    np.count=1; np.e[0].pid=0xff; np.e[0].ping_ms=0xffff; strcpy(np.e[0].name,"Pal"); np.e[0].tint=COOP_TINT_BLUE;
    coop_net_set_tint(COOP_TINT_RED);
    #define SENDN(p) do { sendto(ga,(char *)&(p),sizeof (p),0,(struct sockaddr *)&target,sizeof target); Sleep(100); } while (0)
    CoopNamePacket bad=np; bad.e[0].name[1]=7; SENDN(bad); assert(names[1][0]==0);             // control character
    bad=np; memset(bad.e[0].name,'x',COOP_NAME_LEN); SENDN(bad); assert(names[1][0]==0);        // not terminated
    bad=np; bad.count=COOP_MAX_PLAYERS+1; SENDN(bad); assert(names[1][0]==0);                     // count
    bad=np; bad.e[0].pid=9; SENDN(bad); assert(names[1][0]==0);                                   // id
    bad=np; bad.version=61; SENDN(bad); assert(names[1][0]==0);                                   // protocol
    bad=np; bad.echo=12345; SENDN(bad); assert(names[1][0]==0);                                   // not our session
    bad=np; bad.session=999; SENDN(bad); assert(names[1][0]==0);                                  // not that guest's
    bad=np; bad.e[0].tint=COOP_TINT_COUNT; SENDN(bad); assert(names[1][0]==0 && tints[1]==0);    // (65) colour range
    bad=np; bad.enemy_hp=COOP_ENEMY_HP_MAX+2; SENDN(bad); assert(names[1][0]==0);                // (101) enemy life range
    bad=np; strcpy(bad.e[0].name,"Jo\xc3o"); SENDN(bad); assert(names[1][0]==0);                // (DISHINV) broken UTF-8
    bad=np; strcpy(bad.e[0].name,"Jo\xc0\xa3o"); SENDN(bad); assert(names[1][0]==0);            // overlong
    bad=np; strcpy(bad.e[0].name,"Jo\xc2\x85o"); SENDN(bad); assert(names[1][0]==0);            // C1 control
    SENDN(np); assert(!strcmp(names[1],"Pal") && coop_net_player_tint(1)==COOP_TINT_BLUE && coop_net_player_tint(0)==COOP_TINT_RED);  // (the id is the one the host gave, never what the guest says)
    bad=np; strcpy(bad.e[0].name,"Rapid rename"); SENDN(bad); assert(!strcmp(names[1],"Pal"));
    // the host's list to the guest: both names; echoing its clock gives the host a ping
    CoopNamePacket r;
    assert(recv_kind(ga,&r,sizeof r,COOP_NAME_MAGIC,names_has) && r.echo==301);
    assert(r.e[0].pid==0 && !strcmp(r.e[0].name,"H\xc3\xb4st?Name") && r.e[1].pid==1 && !strcmp(r.e[1].name,"Pal"));
    assert(r.e[0].tint==COOP_TINT_RED && r.e[1].tint==COOP_TINT_BLUE);  // (65) colours relayed
    assert(r.enemy_hp==0+1);  // (101) the host's enemy life (default 0: off) to its guests
    Sleep(20);  // GetTickCount64 can return the same tick for the echoed clock and the next receive.
    np.echo_t_ns=r.t_ns; np.echo_hold_us=1000; SENDN(np);
    assert(pings[1] >= 0 && pings[1] < 2000);
    CoopLobbyEntry le[8]; int n=coop_net_lobby(le,8);
    assert(n==2 && le[0].me && le[0].host && !strcmp(le[0].name,"Hôst?Name") && le[1].pid==1 && le[1].partner &&
           !strcmp(le[1].name,"Pal") && le[1].ping_ms==pings[1] && le[1].age_s >= 0);
    // stop: everything forgotten, a new session from a config (the menu: join that "host" = the ga socket)
    uint64_t old=local_session;
    coop_net_stop(); assert(!coop_net_on() && names[1][0]==0 && coop_net_lobby(le,8)==1);
    CoopNetConfig c={0}; c.mode=COOP_MODE_SOLO; assert(!coop_net_start(&c) && *coop_net_error());
    c.mode=COOP_MODE_JOIN; assert(!coop_net_start(&c));  // (no address)
    struct sockaddr_in gaddr; len=sizeof gaddr; getsockname(ga,(struct sockaddr *)&gaddr,&len);
    snprintf(c.peer,sizeof c.peer,"localhost:%d",ntohs(gaddr.sin_port));  // (a host name resolves too)
    snprintf(c.name,sizeof c.name,"Joiner");
    assert(coop_net_start(&c) && coop_net_on() && !coop_net_env_driven() && !coop_net_is_host() && local_session!=old);
    getsockname(sock,(struct sockaddr *)&target,&len); target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    // the "host" (ga) answers the guest's state; then its name list arrives and the guest's name goes out
    { int seen=0; for (int i=0;i<20 && !seen;i++) { CoopPacket g; if (!recv_kind(ga,&g,sizeof g,COOP_MAGIC,NULL)) break;
        seen = g.session==local_session; } assert(seen); }  // (older ones from the stopped host may still be queued)
    CoopPacket h={0}; h.magic=COOP_MAGIC; h.version=COOP_VERSION; h.session=401; h.echo=local_session; h.seq=1;
    h.your_id=1; h.role=COOP_ROLE_PARTNER; h.players=7; deliver(ga,&target,&h,0); assert(coop_net_my_id()==1);
    CoopNamePacket hn={0}; hn.magic=COOP_NAME_MAGIC; hn.version=COOP_VERSION; hn.session=401; hn.echo=local_session;
    hn.t_ns=777; hn.count=3; hn.e[0].pid=0; strcpy(hn.e[0].name,"Host"); hn.e[1].pid=1; strcpy(hn.e[1].name,"Fake me");
    hn.e[2].pid=2; strcpy(hn.e[2].name,"Third"); hn.e[2].ping_ms=42; hn.e[2].tint=COOP_TINT_PURPLE; hn.e[0].tint=COOP_TINT_GREEN;
    coop_net_set_tint(COOP_TINT_YELLOW);
    coop_net_set_enemy_hp(200); assert(coop_net_enemy_hp()==-1);  // (101) a guest: the host's, none heard yet
    hn.enemy_hp=COOP_ENEMY_HP_MAX+2; SENDN(hn); assert(coop_net_enemy_hp()==-1);  // out of range: dropped
    hn.enemy_hp=76;
    SENDN(hn); assert(coop_net_enemy_hp()==75);
    assert(!strcmp(names[0],"Host") && !strcmp(names[2],"Third") && names[1][0]==0 && pings[2]==42);
    assert(coop_net_player_tint(2)==COOP_TINT_PURPLE && coop_net_player_tint(0)==COOP_TINT_GREEN && coop_net_player_tint(1)==COOP_TINT_YELLOW);
    { int seen=0; for (int i=0;i<20 && !seen;i++) { CoopNamePacket q; if (!recv_kind(ga,&q,sizeof q,COOP_NAME_MAGIC,NULL)) break;
        seen = q.count==1 && q.e[0].pid==1 && !strcmp(q.e[0].name,"Joiner") && q.e[0].tint==COOP_TINT_YELLOW && q.echo==401 && q.echo_t_ns==777; } assert(seen); }
    n=coop_net_lobby(le,8);
    assert(n==3 && le[0].me && le[0].pid==1 && le[1].pid==0 && le[1].host && !strcmp(le[1].name,"Host") &&
           le[2].pid==2 && le[2].ping_ms==42);
    coop_net_stop();
    printf("PASS names: names + tunic colours validated, host list + ping, stop and a config start as a guest\n");
    return 0;
}

int main(int argc, char **argv) {
    _putenv("SAVAGEWIND_COOP_BIND=127.0.0.1");
    if (argc > 1 && !strcmp(argv[1], "upnp")) return upnp_main();
    if (argc > 1 && !strcmp(argv[1], "names")) return names_main();
    if (argc > 1 && !strcmp(argv[1], "multi-dish")) return multidish_main();
    if (argc > 1 && !strcmp(argv[1], "multi-dish-guest")) return multidish_guest_main();
    if (argc > 1 && !strcmp(argv[1], "utf8")) return utf8_main();
    if (argc > 1 && !strcmp(argv[1], "multi-world")) return multiworld_main();
    if (argc > 1 && !strcmp(argv[1], "multi-target")) return multitarget_main();
    if (argc > 1 && !strcmp(argv[1], "multi-handover")) return multihandover_main();
    if (argc > 1 && !strcmp(argv[1], "multi-target-guest")) return multitarget_guest_main();
    if (argc > 1 && !strcmp(argv[1], "multi-objects")) return multiobj_main();
    if (argc > 1 && !strcmp(argv[1], "multi-objects-guest")) return multiobj_guest_main();
    if (argc > 1 && !strcmp(argv[1], "multi-world-guest")) return multiworld_guest_main();
    if (argc > 1 && !strcmp(argv[1], "multi")) return multi_main();
    if (argc > 1 && !strcmp(argv[1], "multi-guest")) return multi_guest_main();
    if (argc > 1 && !strcmp(argv[1], "sim")) return sim_main();
    if (argc > 1 && !strcmp(argv[1], "events")) return events_main();
    bool guest = argc > 1;
    WSADATA w; assert(!WSAStartup(MAKEWORD(2,2),&w));
    SOCKET sender = socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    struct sockaddr_in from = {0}, target;
    from.sin_family=AF_INET; from.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(!bind(sender,(struct sockaddr *)&from,sizeof from));
    int len=sizeof from; getsockname(sender,(struct sockaddr *)&from,&len);
    char setting[100];
    snprintf(setting,sizeof setting,"SAVAGEWIND_COOP_PEER=%s", "");
    if(guest) snprintf(setting,sizeof setting,"SAVAGEWIND_COOP_PEER=127.0.0.1:%d",ntohs(from.sin_port));
    _putenv(setting); _putenv("SAVAGEWIND_COOP_PORT=0"); _putenv("SAVAGEWIND_COOP_CODE= Tri Force ");
    coop_net_init(); assert(coop_net_on());
    { uint64_t k0, k1;  // (case, spaces; derived aside: the running network thread keeps the live key)
      code_key_derive("tri force", &k0, &k1); assert(k0==code_k0 && k1==code_k1);
      code_key_derive("triforce", &k0, &k1); assert(k0!=code_k0); }
    len=sizeof target; getsockname(sock,(struct sockaddr *)&target,&len);
    target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    CoopPacket p={0}; p.magic=COOP_MAGIC; p.version=COOP_VERSION; p.flags=1;
    p.seq=10000; p.pos[0]=100; p.session=101;
    if(guest) p.your_id=1, p.role=COOP_ROLE_PARTNER;  // (56) the host tells its world partner its id and role
    if (!guest) {
        uint64_t cookie = request_challenge(sender, &target, &p);
        assert(cookie && coop_net_partner_id() < 0 && count() == 0);
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sender, &fds);
        struct timeval wait = {0, 150000};
        assert(select((int)sender + 1, &fds, NULL, NULL, &wait) == 0); // no gameplay or lobby data before proof
    }
    deliver(sender,&target,&p,0); assert(count()==0); // no reciprocal echo yet
    p.echo=local_session; deliver(sender,&target,&p,0); assert(count()==1);
    deliver(sender,&target,&p,0); assert(count()==1); // duplicate
    p.seq--; deliver(sender,&target,&p,0); assert(count()==1);
    p.seq=10001; p.pos[0]=INFINITY; deliver(sender,&target,&p,0); assert(count()==1);
    assert(COOP_VERSION>=73);
    p.pos[0]=100; p.moon_day=COOP_MOON_DAY_ON+1; deliver(sender,&target,&p,0); assert(count()==1);
    p.moon_day=COOP_MOON_DAY_ON; p.weather=COOP_WEATHER_TYPES+1;
    deliver(sender,&target,&p,0); assert(count()==1);
    p.weather=0;
    if (!guest) { deliver(sender,&target,&p,0); assert(count()==1); }
    p.moon_day=COOP_MOON_DAY_UNKNOWN;
    p.pos[0]=100; p.yaw=NAN; deliver(sender,&target,&p,0); assert(count()==1);
    p.yaw=0; p.anim=COOP_ANIM_COUNT; deliver(sender,&target,&p,0); assert(count()==1);
    p.anim=0; p.anim_upper=COOP_ANIM_UPPER_COUNT; deliver(sender,&target,&p,0); assert(count()==1);
    p.anim_upper=0; p.face=COOP_FACE_COUNT; deliver(sender,&target,&p,0); assert(count()==1);
    p.face=0; p.aim_flags=8; deliver(sender,&target,&p,0); assert(count()==1);
    p.aim_flags=COOP_AIM_ON|COOP_AIM_PITCH; p.aim_pitch=91; deliver(sender,&target,&p,0); assert(count()==1);
    p.aim_pitch=-30; p.aim_yaw=-128; deliver(sender,&target,&p,0); assert(count()==1);
    p.aim_yaw=20;
    p.flags=128; deliver(sender,&target,&p,0); assert(count()==1);
    p.flags=COOP_FLAG_MAGBEAM; deliver(sender,&target,&p,0); assert(count()==1);  // beam outside the world
    p.flags=1; snprintf(p.weapon,sizeof p.weapon,"../x"); deliver(sender,&target,&p,0); assert(count()==1); // path chars
    memset(p.weapon,'A',sizeof p.weapon); deliver(sender,&target,&p,0); assert(count()==1);                  // unterminated
    p.anim_seq=42; p.anim_upper=COOP_ANIM_UPPER_BOW; p.anim_upper_seq=7;
    snprintf(p.weapon,sizeof p.weapon,"Weapon_Sword_044");
    // (64) A held beam must have a finite, nearby tip and a world area.
    p.flags=COOP_FLAG_IN_WORLD | COOP_FLAG_MAGBEAM;
    p.magbeam_tip[0]=105; assert(!packet_valid(&p)); // missing area
    p.area=7; p.magbeam_tip[0]=NAN; deliver(sender,&target,&p,0); assert(count()==1);
    p.magbeam_tip[0]=100001; assert(!packet_valid(&p));
    p.magbeam_tip[0]=100+COOP_MAGBEAM_RANGE+1; assert(!packet_valid(&p));
    p.magbeam_tip[0]=105; p.flags=COOP_FLAG_IN_WORLD; assert(!packet_valid(&p)); // inactive with a tip
    // (66) A lock can be sent before the first strike, and an arrow can be hidden.
    { CoopPacket check=p;
      check.magbeam_tip[0]=0;
      check.stasisfx=(CoopStasisFx){.hash=830217036, .serial=5, .flags=COOP_STASISFX_START};
      assert(packet_valid(&check));
      check.area=0; assert(!packet_valid(&check));
      check.area=7; check.flags=0; assert(!packet_valid(&check));
      check.flags=COOP_FLAG_IN_WORLD; check.stasisfx.serial=0; assert(!packet_valid(&check));
      check.stasisfx=(CoopStasisFx){.foe_ticks=1}; assert(!packet_valid(&check));
      check.stasisfx=(CoopStasisFx){.serial=5,.hash=830217036,.foe_ticks=60}; assert(packet_valid(&check));
      check.stasisfx.flags=COOP_STASISFX_VISIBLE; assert(!packet_valid(&check));
      check.stasisfx.flags=COOP_STASISFX_ARROW; check.stasisfx.charge=50; assert(!packet_valid(&check));
      check.stasisfx.direction[2]=-128; assert(!packet_valid(&check));
      check.stasisfx.direction[2]=-127; assert(packet_valid(&check));
      check.stasisfx.direction[1]=127; assert(!packet_valid(&check));
      check.stasisfx.direction[1]=0; check.stasisfx.charge=101; assert(!packet_valid(&check));
      check.stasisfx.charge=50; check.stasisfx.flags|=0x80; assert(!packet_valid(&check));
      check.stasisfx.flags=COOP_STASISFX_ARROW|COOP_STASISFX_VISIBLE;
      p.stasisfx=check.stasisfx;
      check.stasisfx.hash=0; deliver(sender,&target,&check,0); assert(count()==1); }
    p.flags|=COOP_FLAG_MAGBEAM; deliver(sender,&target,&p,0); assert(count()==2);
    { CoopState rs; double age; assert(coop_net_get_remote(&rs,&age) && !strcmp(rs.weapon,"Weapon_Sword_044") && rs.anim_seq==42 && rs.anim_upper==COOP_ANIM_UPPER_BOW && rs.anim_upper_seq==7 &&
      rs.aim_pitch==-30 && rs.aim_yaw==20 && rs.aim_flags==(COOP_AIM_ON|COOP_AIM_PITCH) &&
      (rs.flags & COOP_FLAG_MAGBEAM) && rs.magbeam_tip[0]==105 &&
      !memcmp(&rs.stasisfx,&p.stasisfx,sizeof rs.stasisfx)); }
    p.flags=COOP_FLAG_IN_WORLD; p.magbeam_tip[0]=0;
    if (guest) p.weather=3, p.moon_day=COOP_MOON_DAY_ON;
    p.stasisfx=(CoopStasisFx){0};
    // (protocol 14) gear and armor: names like the weapon's, bone in range, finite relative matrix
    assert(COOP_BONE_ARM_R==7 && COOP_BONE_COUNT==8);  // (protocol 72) Arm_2_R is a gear bone
    p.seq++; p.gear[1].bone=COOP_BONE_COUNT; deliver(sender,&target,&p,0); assert(count()==2);                 // bad bone
    p.gear[1].bone=COOP_BONE_WEAPON_L; p.gear[1].rel[3]=NAN; deliver(sender,&target,&p,0); assert(count()==2);  // NaN
    p.gear[1].rel[3]=0.1f; snprintf(p.gear[1].name,sizeof p.gear[1].name,"a/b"); deliver(sender,&target,&p,0); assert(count()==2);
    snprintf(p.armor[1],sizeof p.armor[1],"..\\x"); snprintf(p.gear[1].name,sizeof p.gear[1].name,"Weapon_Shield_001");
    deliver(sender,&target,&p,0); assert(count()==2);                                                           // bad armor
    snprintf(p.armor[1],sizeof p.armor[1],"Armor_001_Upper"); deliver(sender,&target,&p,0); assert(count()==3); // accepted
    { CoopState rs; double age; assert(coop_net_get_remote(&rs,&age) && !strcmp(rs.armor[1],"Armor_001_Upper") &&
      !strcmp(rs.gear[1].name,"Weapon_Shield_001") && rs.gear[1].bone==COOP_BONE_WEAPON_L && rs.gear[1].rel[3]==0.1f &&
      !(rs.flags & COOP_FLAG_MAGBEAM) && rs.magbeam_tip[0]==0 && !rs.stasisfx.hash && !rs.stasisfx.flags &&
      rs.weather==p.weather && rs.moon_day==p.moon_day); }
    // (protocol 33) remote bombs in the state packet: a bad one rejects the packet, a good one arrives
    assert(sizeof p==1464 && sizeof p+COOP_TAG==1472);
    p.seq++; p.bombs[1].kind=COOP_BOMB_CUBE; p.bombs[1].rot[0]=NAN; deliver(sender,&target,&p,0); assert(count()==3);
    p.bombs[1].rot[0]=1; p.bombs[1].vel[2]=INFINITY; deliver(sender,&target,&p,0); assert(count()==3);
    if (guest) p.moon_day=COOP_MOON_DAY_OFF;
    p.bombs[1].vel[2]=-6.5f; p.bombs[1].pos[1]=200.5f; deliver(sender,&target,&p,0); assert(count()==4);
    { CoopBomb rb[COOP_BOMBS]; double age; assert(coop_net_remote_bombs(rb,&age) && rb[1].kind==COOP_BOMB_CUBE &&
      rb[1].pos[1]==200.5f && rb[1].vel[2]==-6.5f && rb[0].kind==COOP_BOMB_NONE); }
    if (guest) { CoopState rs; double age; uint64_t session;
        assert(coop_net_get_host(&rs,&age,&session) && rs.weather==3 && rs.moon_day==COOP_MOON_DAY_OFF); }
    if (guest) {
        CoopLightning strike={8000000000ull,0x1234u,{-834,199,1812}};
        CoopNamePacket clock={.magic=COOP_NAME_MAGIC,.version=COOP_VERSION,.session=p.session,.echo=local_session,
                              .t_ns=strike.at_ns};
        assert(!coop_net_lightning_recent(strike.at_ns));
        sendto(sender,(char *)&clock,sizeof clock,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
        assert(coop_net_lightning_recent(strike.at_ns) && !coop_net_lightning_recent(strike.at_ns-2000000000ull));
        assert(!coop_net_send_event(COOP_EV_LIGHTNING,&strike,sizeof strike));
        CoopEventPacket strikes={.magic=COOP_EVENT_MAGIC,.version=COOP_VERSION,.session=p.session,.echo=local_session,.count=3};
        strikes.e[0]=(CoopEvent){.id=1,.type=COOP_EV_LIGHTNING,.len=sizeof strike-1};
        strikes.e[1]=(CoopEvent){.id=2,.type=COOP_EV_LIGHTNING|(2<<8),.len=sizeof strike};
        strikes.e[2]=(CoopEvent){.id=3,.type=COOP_EV_LIGHTNING,.len=sizeof strike};
        for (int i=0;i<3;i++) memcpy(strikes.e[i].data,&strike,sizeof strike);
        sendto(sender,(char *)&strikes,sizeof strikes,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
        uint16_t type; int n, origin; unsigned char data[COOP_EVENT_DATA];
        assert(coop_net_next_event_from(&type,data,&n,&origin) && type==COOP_EV_LIGHTNING && origin==0 &&
               n==sizeof strike && !memcmp(data,&strike,sizeof strike));
        assert(!coop_net_next_event(&type,data,&n));
    }
    // (39, coop-polish) the rider's horse in the state packet: a bad one rejects the packet, a good one arrives
    { CoopHorse h={0}; h.flags=COOP_HORSE_SHOWN|COOP_HORSE_RIDDEN|COOP_HORSE_REL_SADDLE;
      snprintf(h.body,sizeof h.body,"GameRomHorse01"); snprintf(h.mane,sizeof h.mane,"Horse_Link_Mane_00L_Reduction");
      float m[12]={1,0,0,-845.5f, 0,1,0,200.2f, 0,0,1,1803.1f}, r[12]={1,0,0,0, 0,1,0,0.3f, 0,0,1,-0.1f};
      memcpy(h.mtx,m,sizeof m); memcpy(h.rel,r,sizeof r); h.as[0]=12; h.seq[0]=3; h.n_in=2; h.in_kind[0]=0;
      float f=0.75f; memcpy(&h.in_val[0],&f,4); h.in_kind[1]=42; h.in_val[1]=7; h.bools=5;
      p.seq++; p.horse=h; p.horse.as[1]=COOP_HORSE_AS_COUNT; deliver(sender,&target,&p,0); assert(count()==4);
      p.horse=h; snprintf(p.horse.saddle,sizeof p.horse.saddle,"../x"); deliver(sender,&target,&p,0); assert(count()==4);
      p.horse=h; p.horse.rel[3]=11; deliver(sender,&target,&p,0); assert(count()==4);
      p.horse=h; p.horse.flags=COOP_HORSE_RIDDEN; deliver(sender,&target,&p,0); assert(count()==4);
      p.horse=h; p.horse.in_kind[1]=50; deliver(sender,&target,&p,0); assert(count()==4);
      p.horse=h; p.horse=h; deliver(sender,&target,&p,0); assert(count()==5);
      CoopHorse rh; double age; assert(coop_net_remote_horse(&rh,&age) && !strcmp(rh.body,"GameRomHorse01") &&
        rh.mtx[3]==-845.5f && rh.rel[7]==0.3f && rh.in_val[1]==7 && rh.bools==5 && rh.as[0]==12);
      memset(&p.horse,0,sizeof p.horse); }
    // (protocol 49) items held in the hands: a bad one rejects the packet, a good one arrives
    { p.seq++; snprintf(p.held[2].name,sizeof p.held[2].name,"Item_Fruit_A"); p.held[2].pos[1]=6;
      deliver(sender,&target,&p,0); assert(count()==5);
      p.held[2].pos[1]=NAN; deliver(sender,&target,&p,0); assert(count()==5);
      p.held[2].pos[1]=0.25f; snprintf(p.held[0].name,sizeof p.held[0].name,"a b"); deliver(sender,&target,&p,0); assert(count()==5);
      p.held[0].name[0]=0; snprintf(p.gear[COOP_GEAR_SLATE].name,sizeof p.gear[0].name,"Item_Conductor");
      p.gear[COOP_GEAR_SLATE].bone=COOP_BONE_POD_A; deliver(sender,&target,&p,0); assert(count()==6);
      CoopState rs; double age; assert(coop_net_get_remote(&rs,&age) && !strcmp(rs.held[2].name,"Item_Fruit_A") &&
        rs.held[2].pos[1]==0.25f && !rs.held[0].name[0] && !strcmp(rs.gear[COOP_GEAR_SLATE].name,"Item_Conductor"));
      memset(p.held,0,sizeof p.held); memset(&p.gear[COOP_GEAR_SLATE],0,sizeof p.gear[0]); }
    // (protocol 52) enemy shots flying in the sender's game: a bad one rejects the packet, a good one arrives
    p.seq++; p.shots[2].kind=COOP_SHOTV_OCT_ROCK; p.shots[2].vel[0]=NAN; deliver(sender,&target,&p,0); assert(count()==6);
    p.shots[2].vel[0]=201; deliver(sender,&target,&p,0); assert(count()==6);
    p.shots[2].vel[0]=-55.5f; p.shots[2].pos[2]=1450.25f; deliver(sender,&target,&p,0); assert(count()==7);
    { CoopShotView rv[COOP_SHOT_VIEWS]; double age; assert(coop_net_remote_shot_views(rv,&age) &&
      rv[2].kind==COOP_SHOTV_OCT_ROCK && rv[2].vel[0]==-55.5f && rv[2].pos[2]==1450.25f && rv[0].kind==COOP_SHOTV_NONE); }
    p.shots[2]=(CoopShotView){0};
    // (protocol 53) the join code: a datagram without its tag, with another code's tag, or with a changed byte is dropped
    { uint32_t rej=code_rejected; p.seq++;
      assert((sendto)(sender,(char *)&p,sizeof p,0,(struct sockaddr *)&target,sizeof target)>0); Sleep(100);
      assert(count()==7 && code_rejected==rej+1);
      unsigned char w[sizeof p+COOP_TAG]; memcpy(w,&p,sizeof p);
      tag_put(w+sizeof p,siphash24(code_k0^1,code_k1,w,sizeof p));  // (another code's key)
      assert((sendto)(sender,(char *)w,sizeof w,0,(struct sockaddr *)&target,sizeof target)>0); Sleep(100);
      assert(count()==7 && code_rejected==rej+2);
      tag_put(w+sizeof p,siphash24(code_k0,code_k1,w,sizeof p));
      w[100]^=1; assert((sendto)(sender,(char *)w,sizeof w,0,(struct sockaddr *)&target,sizeof target)>0); Sleep(100);
      assert(count()==7 && code_rejected==rej+3);
      w[100]^=1; assert((sendto)(sender,(char *)w,sizeof w,0,(struct sockaddr *)&target,sizeof target)>0); Sleep(100);
      assert(count()==8); }
    // (protocol 29) tree mode packet: validated, newest seq wins, only from the current session
    { CoopAsTreePacket ap={0}; CoopAsTree rt; double age;
      ap.magic=COOP_ASTREE_MAGIC; ap.version=COOP_VERSION; ap.session=101; ap.echo=local_session; ap.seq=5;
      ap.t.flags=COOP_ASTREE_VALID; ap.t.normal[1]=1; ap.t.f[0]=0.7f; ap.t.as[0]=12; ap.t.str[0]=3; ap.t.vel[2]=0.18f;
      assert(sizeof ap==288);
      ap.t.as[3]=COOP_AS_NAME_COUNT; sendto(sender,(char *)&ap,sizeof ap,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
      assert(!coop_net_remote_astree(&rt,&age));                                     // AS index out of range
      ap.t.as[3]=0; ap.t.str[2]=COOP_AS_STRING_COUNT; sendto(sender,(char *)&ap,sizeof ap,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
      assert(!coop_net_remote_astree(&rt,&age));                                     // string index out of range
      ap.t.str[2]=0; ap.t.f[5]=NAN; sendto(sender,(char *)&ap,sizeof ap,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
      assert(!coop_net_remote_astree(&rt,&age));                                     // not a number
      ap.t.f[5]=0; ap.t.vel[0]=50; sendto(sender,(char *)&ap,sizeof ap,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
      assert(!coop_net_remote_astree(&rt,&age));                                     // velocity too large
      ap.t.vel[0]=0; ap.t.flags=0x80; sendto(sender,(char *)&ap,sizeof ap,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
      assert(!coop_net_remote_astree(&rt,&age));                                     // unknown flag
      // (protocol 39) effect-set properties: mask in range, values finite floats or small ints, unmasked ones 0
      { float one5=1.5f; uint32_t bits; memcpy(&bits,&one5,4);
        ap.t.flags=COOP_ASTREE_VALID; ap.t.fx_mask=4; ap.t.fx[2]=bits; sendto(sender,(char *)&ap,sizeof ap,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
        assert(!coop_net_remote_astree(&rt,&age));                                   // fx without the FX flag
        ap.t.flags=COOP_ASTREE_VALID|COOP_ASTREE_FX; ap.t.fx_mask=1u<<COOP_FX_PROPS; sendto(sender,(char *)&ap,sizeof ap,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
        assert(!coop_net_remote_astree(&rt,&age));                                   // mask bit out of range
        ap.t.fx_mask=4; ap.t.fx[5]=7; sendto(sender,(char *)&ap,sizeof ap,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
        assert(!coop_net_remote_astree(&rt,&age));                                   // value outside the mask
        ap.t.fx[5]=0; ap.t.fx[2]=0x7fc00000u; sendto(sender,(char *)&ap,sizeof ap,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
        assert(!coop_net_remote_astree(&rt,&age));                                   // NaN
        ap.t.fx[2]=bits; ap.t.champ=COOP_CHAMP_COUNT; sendto(sender,(char *)&ap,sizeof ap,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
        assert(!coop_net_remote_astree(&rt,&age));                                   // (89) shield count bit without a count
        ap.t.champ=2u<<COOP_CHAMP_COUNT_SHIFT; sendto(sender,(char *)&ap,sizeof ap,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
        assert(!coop_net_remote_astree(&rt,&age));                                   // (89) a count without the bit
        assert(!coop_champ_ok(COOP_CHAMP_SHIELD,0) && coop_champ_ok(0,0) && coop_champ_ok(0xff00u|COOP_CHAMP_READY,COOP_ASTREE_VALID));
        ap.t.champ=0; ap.t.charge_lv=3; sendto(sender,(char *)&ap,sizeof ap,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
        assert(!coop_net_remote_astree(&rt,&age));                                   // (47) charge level above 2
        ap.t.charge_lv=0; ap.t.eat=COOP_EAT_MAX+1; sendto(sender,(char *)&ap,sizeof ap,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
        assert(!coop_net_remote_astree(&rt,&age));                                   // (81) eat case out of range
        ap.t.eat=COOP_EAT_DRINK; ap.t.eat_seq=5;
        ap.t.champ=COOP_CHAMP_SHIELD|COOP_CHAMP_COUNT|(3u<<COOP_CHAMP_COUNT_SHIFT)|(2u<<COOP_CHAMP_BREAK_SHIFT)|(1u<<COOP_CHAMP_USE_SHIFT(COOP_CHAMP_URBOSA)); }
      sendto(sender,(char *)&ap,sizeof ap,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
      assert(coop_net_remote_astree(&rt,&age) && rt.as[0]==12 && rt.f[0]==0.7f && rt.str[0]==3 && rt.vel[2]==0.18f &&
             rt.fx_mask==4 && rt.fx[2]==0x3fc00000u && rt.eat==COOP_EAT_DRINK && rt.eat_seq==5 &&
             rt.champ==(COOP_CHAMP_SHIELD|COOP_CHAMP_COUNT|(3u<<COOP_CHAMP_COUNT_SHIFT)|(2u<<COOP_CHAMP_BREAK_SHIFT)|(1u<<COOP_CHAMP_USE_SHIFT(COOP_CHAMP_URBOSA)))); // accepted
      ap.seq=4; ap.t.as[0]=13; sendto(sender,(char *)&ap,sizeof ap,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
      assert(coop_net_remote_astree(&rt,&age) && rt.as[0]==12);                      // older seq ignored
      ap.seq=6; ap.session=999; sendto(sender,(char *)&ap,sizeof ap,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
      assert(coop_net_remote_astree(&rt,&age) && rt.as[0]==12);                      // other session ignored
      ap.session=101; ap.version=COOP_VERSION-1; sendto(sender,(char *)&ap,sizeof ap,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
      assert(coop_net_remote_astree(&rt,&age) && rt.as[0]==12); }                    // other protocol ignored
    // (protocol 80) fauna snapshot: host -> guest only, validated, newest seq wins, only from the current session
    { CoopFaunaPacket fp={0}; CoopFauna rf; double age; uint32_t fseq;
      fp.magic=COOP_FAUNA_MAGIC; fp.version=COOP_VERSION; fp.session=101; fp.echo=local_session; fp.seq=5;
      fp.f.area=7; fp.f.center[0]=100; fp.f.count=2;
      fp.f.a[0]=(CoopFaunaAnimal){.id=11,.species=1,.yaw=100,.pos={110,0,0}};
      fp.f.a[1]=(CoopFaunaAnimal){.id=0xabcd,.species=40,.flags=COOP_FAUNA_PLACED,.pos={100,5,40}};
      assert(sizeof fp==696 && coop_fauna_ok(&fp.f));
      assert(coop_fauna_species("Animal_Insect_Z")>0 && !strcmp(coop_fauna_name(coop_fauna_species("Animal_Fish_X")),"Animal_Fish_X"));
      assert(!coop_fauna_species("Animal_Fox_A") && !coop_fauna_species("Enemy_Keese") && !coop_fauna_name(999));
#define SENDF() sendto(sender,(char *)&fp,sizeof fp,0,(struct sockaddr *)&target,sizeof target); Sleep(100)
      CoopFauna b=fp.f;
      b.a[0].species=0; assert(!coop_fauna_ok(&b)); b=fp.f;
      b.a[0].species=200; assert(!coop_fauna_ok(&b)); b=fp.f;
      b.a[1].flags=4; assert(!coop_fauna_ok(&b)); b=fp.f;
      b.a[1].flags=COOP_FAUNA_PLACED|COOP_FAUNA_FLED; assert(coop_fauna_ok(&b)); b=fp.f;
      b.a[1].pos[2]=100+COOP_FAUNA_R; assert(!coop_fauna_ok(&b)); b=fp.f;
      b.a[0].pos[1]=NAN; assert(!coop_fauna_ok(&b)); b=fp.f;
      b.a[1].id=0; assert(!coop_fauna_ok(&b)); b=fp.f;
      b.a[1].id=11; b.a[1].flags=0; assert(!coop_fauna_ok(&b)); b=fp.f;
      b.a[1].id=11; assert(coop_fauna_ok(&b)); b=fp.f;  // (a HashId may equal a process id)
      b.count=1; assert(!coop_fauna_ok(&b)); b=fp.f;      // (a listed slot beyond count)
      b.count=COOP_FAUNA_MAX+1; assert(!coop_fauna_ok(&b)); b=fp.f;
      b.area=0; assert(!coop_fauna_ok(&b)); b=fp.f;
      b.pad[1]=1; assert(!coop_fauna_ok(&b)); b=fp.f;
      b.center[2]=INFINITY; assert(!coop_fauna_ok(&b)); b=fp.f;
      fp.f.a[0].species=0; SENDF(); assert(!coop_net_remote_fauna(&rf,&age,&fseq));  // invalid: dropped
      fp.f.a[0].species=1; SENDF();
      if (guest) assert(coop_net_remote_fauna(&rf,&age,&fseq) && fseq==5 && rf.count==2 && rf.a[1].id==0xabcd && age<1);
      else assert(!coop_net_remote_fauna(&rf,&age,&fseq));                         // the host never takes one
      fp.seq=4; fp.f.a[0].id=12; SENDF();
      if (guest) assert(coop_net_remote_fauna(&rf,&age,&fseq) && rf.a[0].id==11);  // older seq ignored
      fp.seq=6; fp.session=999; SENDF();
      if (guest) assert(coop_net_remote_fauna(&rf,&age,&fseq) && rf.a[0].id==11);  // other session ignored
      fp.session=101; fp.version=COOP_VERSION-1; SENDF();
      if (guest) assert(coop_net_remote_fauna(&rf,&age,&fseq) && rf.a[0].id==11);  // other protocol ignored
      fp.version=COOP_VERSION; SENDF();
      if (guest) assert(coop_net_remote_fauna(&rf,&age,&fseq) && rf.a[0].id==12 && fseq==6);
#undef SENDF
      if (!guest) {  // the host sends a set once to its partner
        b=fp.f; coop_net_set_fauna(&b);
        bool seen=false;
        // (time-limited, not 60 reads: the host's state packets queued on a slow CI machine used them all up)
        for (DWORD t0=GetTickCount(); GetTickCount()-t0<6000 && !seen;) {
          fd_set fds; FD_ZERO(&fds); FD_SET(sender,&fds);
          struct timeval wait={0,100000};
          if (select((int)sender + 1,&fds,NULL,NULL,&wait)<=0) continue;
          unsigned char buf[2048]; int got=recv(sender,(char *)buf,sizeof buf,0);
          uint32_t mg; memcpy(&mg,buf,4);
          if (got>=(int)sizeof(CoopFaunaPacket) && mg==COOP_FAUNA_MAGIC) {
            CoopFaunaPacket in; memcpy(&in,buf,sizeof in);
            seen=in.version==COOP_VERSION && in.echo==101 && coop_fauna_ok(&in.f) && in.f.a[0].id==12;
          }
        }
        assert(seen);
      } }
    // (protocol 48) ragdoll pose packet: validated, newest seq wins, only from the current session
    { CoopPosePacket rp={0}; CoopPose ro; double age;
      rp.magic=COOP_POSE_MAGIC; rp.version=COOP_VERSION; rp.session=101; rp.echo=local_session; rp.seq=5;
      rp.pose.flags=COOP_POSE_ACTIVE; rp.pose.count=COOP_POSE_BONES; rp.pose.mask=5; rp.pose.root[1]=0.9f;
      rp.pose.q[0][3]=32767; rp.pose.q[2][0]=23170; rp.pose.q[2][3]=23170;
      assert(sizeof rp==232);
#define SENDP() sendto(sender,(char *)&rp,sizeof rp,0,(struct sockaddr *)&target,sizeof target); Sleep(100)
      rp.pose.flags=2; SENDP(); assert(!coop_net_remote_pose(&ro,&age));            // unknown flag
      rp.pose.flags=COOP_POSE_ACTIVE; rp.pose.count=3; SENDP(); assert(!coop_net_remote_pose(&ro,&age));  // bone count
      rp.pose.count=COOP_POSE_BONES; rp.pose.pad=1; SENDP(); assert(!coop_net_remote_pose(&ro,&age));     // padding
      rp.pose.pad=0; rp.pose.mask=1u<<COOP_POSE_BONES|5; SENDP(); assert(!coop_net_remote_pose(&ro,&age));  // mask range
      rp.pose.mask=5; rp.pose.root[0]=NAN; SENDP(); assert(!coop_net_remote_pose(&ro,&age));              // not a number
      rp.pose.root[0]=25; SENDP(); assert(!coop_net_remote_pose(&ro,&age));                               // root too far
      rp.pose.root[0]=0.1f; rp.pose.q[2][0]=0; SENDP(); assert(!coop_net_remote_pose(&ro,&age));          // not unit length
      rp.pose.q[2][0]=23170; rp.pose.q[4][1]=100; SENDP(); assert(!coop_net_remote_pose(&ro,&age));      // unmasked not 0
      rp.pose.q[4][1]=0; SENDP();
      assert(coop_net_remote_pose(&ro,&age) && ro.mask==5 && ro.root[1]==0.9f && ro.q[2][3]==23170 && ro.flags==COOP_POSE_ACTIVE);  // accepted
      rp.seq=4; rp.pose.flags=0; SENDP(); assert(coop_net_remote_pose(&ro,&age) && ro.flags==COOP_POSE_ACTIVE);  // older seq ignored
      rp.seq=6; rp.session=999; SENDP(); assert(coop_net_remote_pose(&ro,&age) && ro.flags==COOP_POSE_ACTIVE);  // other session
      rp.session=101; rp.version=COOP_VERSION-1; SENDP(); assert(coop_net_remote_pose(&ro,&age) && ro.flags==COOP_POSE_ACTIVE);
      rp.version=COOP_VERSION; SENDP(); assert(coop_net_remote_pose(&ro,&age) && ro.flags==0);            // newer: taken
#undef SENDP
    }
    // (protocol 30) shared objects: OWNED and CARRIED flags kept, other bits dropped, bad numbers rejected
    { CoopObjPacket op={0}; CoopObject ro; double age;
      op.magic=COOP_OBJ_MAGIC; op.version=COOP_VERSION; op.session=101; op.echo=local_session; op.seq=7; op.count=2;
      op.o[0].hash=1887609879u; op.o[0].gen=3; op.o[0].flags=COOP_OBJ_OWNED|COOP_OBJ_CARRIED|0x40; op.o[0].rot[3]=1;
      op.o[0].pos[1]=-26; op.o[0].vel[2]=9; op.o[0].rot[3]=0.9f;
      op.o[1].hash=42; op.o[1].flags=COOP_OBJ_OWNED; op.o[1].rot[3]=1; op.o[1].pos[0]=NAN;
      sendto(sender,(char *)&op,sizeof op,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
      assert(coop_net_remote_object(1887609879u,&ro,&age) && ro.gen==3 && ro.flags==(COOP_OBJ_OWNED|COOP_OBJ_CARRIED) &&
             ro.pos[1]==-26 && ro.vel[2]==9 && fabsf(ro.rot[3]-1)<0.0001f);
      assert(!coop_net_remote_object(42,&ro,&age));                                  // not a number: dropped
      op.seq=8; op.count=1; op.o[0].flags=COOP_OBJ_OWNED;
      sendto(sender,(char *)&op,sizeof op,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
      assert(coop_net_remote_object(1887609879u,&ro,&age) && ro.flags==COOP_OBJ_OWNED);  // let go: CARRIED cleared
      // (protocol 43) STASIS kept, other unknown bits still dropped
      op.seq=9; op.o[0].flags=COOP_OBJ_OWNED|COOP_OBJ_STASIS|0x80;
      sendto(sender,(char *)&op,sizeof op,0,(struct sockaddr *)&target,sizeof target); Sleep(100);
      assert(coop_net_remote_object(1887609879u,&ro,&age) && ro.flags==(COOP_OBJ_OWNED|COOP_OBJ_STASIS)); }
    p.seq++;
    p.flags=COOP_FLAG_IN_WORLD|COOP_FLAG_HURT; deliver(sender,&target,&p,1); assert(count()==8);  // (protocol 50 HURT)
    { CoopPacket elem=p;
      elem.flags=COOP_FLAG_IN_WORLD|COOP_FLAG_LINK_ELEM; assert(packet_valid(&elem));
      elem.flags=COOP_FLAG_LINK_FIRE; assert(!packet_valid(&elem));  // no effect during a load
      elem.flags=COOP_FLAG_IN_WORLD|COOP_FLAG_BLUE_FIRE; assert(!packet_valid(&elem));
      elem.flags=COOP_FLAG_BURNING|COOP_FLAG_BLUE_FIRE; assert(!packet_valid(&elem));
      elem.flags=COOP_FLAG_IN_WORLD|COOP_FLAG_BURNING|COOP_FLAG_BLUE_FIRE; assert(packet_valid(&elem)); }
    deliver(sender,&target,&p,-1); assert(count()==8);
    p.session=102; p.seq=1; deliver(sender,&target,&p,0); assert(count()==9); // restart, same endpoint
    if (!guest) {
        p.session=104; deliver(sender,&target,&p,0); assert(count()==9); // restart cooldown
    }
    p.session=101; p.seq=20000; deliver(sender,&target,&p,0); assert(count()==9); // delayed retired session
    SOCKET other=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    // (56) host: another endpoint is a second guest (player 2, an extra guest: its state is taken into its own view)
    // while the partner lives; a guest takes nothing but the host's
    p.session=103; p.seq=1; deliver(other,&target,&p,0); assert(count()==(guest?9:10));
    assert(coop_net_partner_id()==(guest?0:1));
    Sleep(5100);
    p.seq++; deliver(other,&target,&p,0); assert(count()==(guest?9:11));
    assert(coop_net_partner_id()==(guest?0:2));  // (56) the partner silent for 5 s: player 2 is the world partner now
    if (!guest) {
        pthread_mutex_lock(&lock);
        conn[2].last_ns = plat_now_ns() - CONN_FORGET_NS - 1000000000ull;
        pthread_mutex_unlock(&lock);
        Sleep(100); // allow the last send already in progress to finish
        unsigned long nonblocking = 1;
        assert(!ioctlsocket(other, FIONBIO, &nonblocking));
        unsigned char wire[MAX_DATAGRAM + COOP_TAG + 1];
        while (recv(other, (char *)wire, sizeof wire, 0) > 0) {}
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(other, &fds);
        struct timeval wait = {0, 650000}; // long enough for one lobby send as well as state
        assert(select((int)other + 1, &fds, NULL, NULL, &wait) == 0);
        nonblocking = 0;
        assert(!ioctlsocket(other, FIONBIO, &nonblocking));
        uint64_t cookie = request_challenge(other, &target, &p);
        assert(cookie && count()==11); // the expired partner must prove its address again
        p.seq++;
        p.challenge = cookie;
        assert(coop_sendto(other, &p, sizeof p, &target) == sizeof p);
        Sleep(100);
        assert(count()==12);
    }
    printf("PASS %s: handshake, ordering, restart, retired sessions, numeric/schema validation, endpoint change\n",guest?"guest":"host");
    closesocket(other); closesocket(sender);
    return 0;
}
