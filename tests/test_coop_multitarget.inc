// (protocol 60) `test_coop_net multi-target`: a host with three world guests. Events aimed at one player (a hit on
// that player's avatar): the host's reach that guest only; a guest's for another guest are passed on by the host with
// the sender as origin and never applied by the host; for the host they are applied and not relayed; a target of the
// sender itself or out of range is dropped. Relay rate by distance: a guest far from a player gets that player's
// states at a lower rate, a near one gets all; SAVAGEWIND_COOP_RELAYRATE=0 relays all.
// `test_coop_net multi-target-guest`: a guest's event for player 2 goes to the host with 2 in the type's high byte.

// datagrams of player `origin`'s state that reach s within ms
static int states_from(SOCKET s, int origin, int ms) {
    int n = 0;
    uint64_t end = GetTickCount64() + (uint64_t)ms;
    while (GetTickCount64() < end) {
        unsigned char wire[MAX_DATAGRAM + COOP_TAG + 1];
        DWORD tmo = 60; setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,(char *)&tmo,sizeof tmo);
        int k = recv(s,(char *)wire,sizeof wire,0);
        if (k <= 0) continue;
        k = tag_check(wire,k);
        if (k == (int)sizeof(CoopPacket)) {
            CoopPacket p; memcpy(&p,wire,sizeof p);
            if (p.magic == COOP_MAGIC && p.origin == origin) n++;
        }
    }
    return n;
}
static int no_areafx(SOCKET s, int tries) {
    for (int i = 0; i < tries; i++) {
        CoopEventPacket vp;
        if (!recv_events(s,&vp,1)) break;
        for (int k = 0; k < vp.count; k++) if ((vp.e[k].type & 0xff) == COOP_EV_AREAFX) return 0;
    }
    return 1;
}

static int multitarget_main(void) {
    WSADATA w; assert(!WSAStartup(MAKEWORD(2,2),&w));
    SOCKET ga = loop_socket(), gb = loop_socket(), gc = loop_socket();
    struct sockaddr_in target;
    _putenv("SAVAGEWIND_COOP_PEER="); _putenv("SAVAGEWIND_COOP_PORT=0"); _putenv("SAVAGEWIND_COOP_CODE=");
    _putenv("SAVAGEWIND_COOP_MULTIWORLD="); _putenv("SAVAGEWIND_COOP_RELAYRATE=");
    coop_net_init(); assert(coop_net_on());
    int len=sizeof target; getsockname(sock,(struct sockaddr *)&target,&len);
    target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    CoopState me={0}; me.pos[0]=1; me.flags=COOP_FLAG_IN_WORLD; coop_net_set_local(&me);
    // A at 10, B at 90 (80 m from A), C at 300 (210 m from B: far), all in one area
    CoopPacket a={0}; a.magic=COOP_MAGIC; a.version=COOP_VERSION; a.flags=1; a.session=701; a.seq=1; a.pos[0]=10;
    deliver(ga,&target,&a,0); a.echo=local_session; a.seq=2; deliver(ga,&target,&a,0);
    CoopPacket b=a; b.session=702; b.seq=1; b.pos[0]=90; deliver(gb,&target,&b,0);
    CoopPacket c=a; c.session=703; c.seq=1; c.pos[0]=300; deliver(gc,&target,&c,0);
    assert(count()==3 && coop_net_partner_id()==1);
#define ALIVE3() do { a.seq++; deliver(ga,&target,&a,0); b.seq++; deliver(gb,&target,&b,0); c.seq++; deliver(gc,&target,&c,0); } while (0)
    ALIVE3();
    uint16_t type; int n, origin; unsigned char data[COOP_EVENT_DATA];
    while (coop_net_next_event(&type,data,&n)) {}
    unsigned char arrow[8]={COOP_AREAFX_ARROW, 2};

    // 1. the host's hit on C's avatar: C only
    assert(coop_net_send_event_to(3,COOP_EV_AREAFX,arrow,sizeof arrow));
    assert(ev_has(gc,COOP_EV_AREAFX,40));
    for (int L=1;L<=2;L++) for (int k=0;k<wl[L].out_count;k++) assert((wl[L].out[k].type & 0xff)!=COOP_EV_AREAFX);
    assert(!coop_net_send_event_to(0,COOP_EV_AREAFX,arrow,sizeof arrow));  // (the host itself)
    assert(!coop_net_send_event_to(9,COOP_EV_AREAFX,arrow,sizeof arrow));  // (no such player)
    ALIVE3();

    // 2. B's hit on C's avatar: passed on to C with origin 2; the host does not apply it, A does not get it
    ev_send(gb,&target,702,0,1,(uint16_t)(COOP_EV_AREAFX|(3<<8)),arrow,sizeof arrow);
    assert(!coop_net_next_event_from(&type,data,&n,&origin));
    assert(ev_has(gc,(uint16_t)(COOP_EV_AREAFX|(2<<8)),40));
    for (int k=0;k<wl[1].out_count;k++) assert((wl[1].out[k].type & 0xff)!=COOP_EV_AREAFX);
    assert(no_areafx(ga,6));

    // 3. B's hit on the host's Link (target 0): ours (origin 2), relayed to nobody (pairwise)
    ev_send(gb,&target,702,0,2,COOP_EV_AREAFX,arrow,sizeof arrow);
    assert(coop_net_next_event_from(&type,data,&n,&origin) && type==COOP_EV_AREAFX && origin==2 && data[1]==2);
    for (int L=1;L<=2;L++) for (int k=0;k<wl[L].out_count;k++) assert((wl[L].out[k].type & 0xff)!=COOP_EV_AREAFX);

    // 4. B naming itself, or a player out of range: dropped (and acknowledged: the next one is still taken)
    ev_send(gb,&target,702,0,3,(uint16_t)(COOP_EV_AREAFX|(2<<8)),arrow,sizeof arrow);
    ev_send(gb,&target,702,0,4,(uint16_t)(COOP_EV_AREAFX|(9<<8)),arrow,sizeof arrow);
    assert(!coop_net_next_event_from(&type,data,&n,&origin));
    for (int L=1;L<=2;L++) for (int k=0;k<wl[L].out_count;k++) assert((wl[L].out[k].type & 0xff)!=COOP_EV_AREAFX);
    ev_send(gb,&target,702,0,5,21,"ok",3);
    assert(coop_net_next_event_from(&type,data,&n,&origin) && type==21 && origin==2);
    ALIVE3();

    // 5. relay rate: 10 states of B in a burst: A (80 m) gets them all, C (210 m) at most every 95 ms
    states_from(ga,2,300); states_from(gc,2,300);  // (drain)
    unsigned skipped0 = relay_skipped;
    for (int i=0;i<10;i++) { b.seq++; sendto(gb,(char *)&b,sizeof b,0,(struct sockaddr *)&target,sizeof target); Sleep(5); }
    int na = states_from(ga,2,400), nc = states_from(gc,2,400);
    if (na != 10 || nc > 2) fprintf(stderr,"relay rate: A got %d, C got %d (skipped %u)\n",na,nc,relay_skipped-skipped0);
    assert(na==10 && nc>=1 && nc<=2 && relay_skipped-skipped0>=8);
    // another area: far too, whatever the distance
    c.pos[0]=95; c.area=77; ALIVE3(); states_from(gc,2,300);
    for (int i=0;i<10;i++) { b.seq++; sendto(gb,(char *)&b,sizeof b,0,(struct sockaddr *)&target,sizeof target); Sleep(5); }
    nc = states_from(gc,2,400);
    assert(nc>=1 && nc<=2);
    // the same area again, 5 m apart: all
    c.area=0; ALIVE3(); states_from(gc,2,300);
    for (int i=0;i<10;i++) { b.seq++; sendto(gb,(char *)&b,sizeof b,0,(struct sockaddr *)&target,sizeof target); Sleep(5); }
    nc = states_from(gc,2,400);
    assert(nc==10);
    // off switch: far, but all relayed
    c.pos[0]=900; ALIVE3(); states_from(gc,2,300);
    _putenv("SAVAGEWIND_COOP_RELAYRATE=0"); relayrate_c = -1;
    for (int i=0;i<10;i++) { b.seq++; sendto(gb,(char *)&b,sizeof b,0,(struct sockaddr *)&target,sizeof target); Sleep(5); }
    nc = states_from(gc,2,400);
    assert(nc==10);
#undef ALIVE3
    printf("PASS multi-target: host's hit to one guest, a guest's hit on another guest passed on (origin, not applied), "
           "a hit on the host applied and not relayed, bad targets dropped, relay rate by distance / area + off switch\n");
    closesocket(ga); closesocket(gb); closesocket(gc);
    return 0;
}

static int multitarget_guest_main(void) {
    WSADATA w; assert(!WSAStartup(MAKEWORD(2,2),&w));
    SOCKET host = loop_socket();
    struct sockaddr_in hostaddr, target; int len=sizeof hostaddr; getsockname(host,(struct sockaddr *)&hostaddr,&len);
    char setting[100]; snprintf(setting,sizeof setting,"SAVAGEWIND_COOP_PEER=127.0.0.1:%d",ntohs(hostaddr.sin_port));
    _putenv(setting); _putenv("SAVAGEWIND_COOP_PORT=0"); _putenv("SAVAGEWIND_COOP_CODE="); _putenv("SAVAGEWIND_COOP_MULTIWORLD=");
    coop_net_init(); assert(coop_net_on());
    len=sizeof target; getsockname(sock,(struct sockaddr *)&target,&len);
    target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    CoopPacket h={0}; h.magic=COOP_MAGIC; h.version=COOP_VERSION; h.flags=1; h.session=801; h.echo=local_session;
    h.seq=1; h.pos[0]=10; h.your_id=3; h.role=COOP_ROLE_WORLD; h.players=15;
    deliver(host,&target,&h,0); assert(count()==1 && coop_net_my_id()==3);
    unsigned char arrow[8]={COOP_AREAFX_ARROW};
    assert(!coop_net_send_event_to(3,COOP_EV_AREAFX,arrow,sizeof arrow));  // (ourselves)
    assert(coop_net_send_event_to(2,COOP_EV_AREAFX,arrow,sizeof arrow));
    { CoopEventPacket vp; assert(recv_events(host,&vp,1) && vp.e[0].type==(uint16_t)(COOP_EV_AREAFX|(2<<8))); }
    assert(coop_net_send_event_to(0,COOP_EV_AREAFX,arrow,sizeof arrow));
    { int seen=0; for (int i=0;i<10 && !seen;i++) { CoopEventPacket vp; if (!recv_events(host,&vp,1)) break;
        for (int k=0;k<vp.count;k++) if (vp.e[k].type==COOP_EV_AREAFX) seen=1; } assert(seen); }
    printf("PASS multi-target-guest: a hit for player 2 carries 2 in the type's high byte, one for the host 0, none to ourselves\n");
    closesocket(host);
    return 0;
}
