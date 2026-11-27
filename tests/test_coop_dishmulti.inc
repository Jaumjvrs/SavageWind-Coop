// (DISHINV) `test_coop_net multi-dish`: a host with three world guests (A 1 = partner, B 2, C 3). A cooked dish goes to
// one player only: B's offer for C is passed on by the host with C's session filled in (origin 2), never applied by
// the host, never sent to A; a wrong sender session, receiver session, sender id or receiver id is dropped; C's answer
// goes back to B the same way; the host's own offer goes to C only; an offer is never broadcast.
// `test_coop_net multi-dish-guest`: a guest (id 3) gives to player 2 with to = 0 (the host fills it in) and to the
// host with the host's session; it takes an offer from player 2 (via the host) only when it names us.
// `test_coop_net utf8`: player names are UTF-8 (coop_name_ok / coop_name_clean / coop_utf8_char).

static CoopDishOffer dm_offer(uint64_t from, uint64_t to, int from_pid, int to_pid, uint64_t id) {
    CoopDishOffer o = {.from = from, .to = to, .id = id, .area = 99, .from_pid = (uint8_t)from_pid, .to_pid = (uint8_t)to_pid,
                       .dish = {"Item_Cook_A_01", 16, 150, 30, 6, 2.0f, {0x12ab, 0, 0, 0, 0}}};
    return o;
}
// the next dish event on s with that type|origin<<8; its payload in out
static int dm_take(SOCKET s, uint16_t type_with_origin, void *out, int len, int tries) {
    for (int i = 0; i < tries; i++) {
        CoopEventPacket vp;
        if (!recv_events(s, &vp, 1)) continue;
        for (int k = 0; k < vp.count; k++)
            if (vp.e[k].type == type_with_origin && vp.e[k].len == len) {
                memcpy(out, vp.e[k].data, (size_t)len);
                return 1;
            }
    }
    return 0;
}
// how many different events with that type|origin<<8 reach s (the host resends unacknowledged ones)
static int dm_ids(SOCKET s, uint16_t type_with_origin, int tries) {
    uint32_t ids[16]; int n = 0;
    for (int i = 0; i < tries; i++) {
        CoopEventPacket vp;
        if (!recv_events(s, &vp, 1)) break;
        for (int k = 0; k < vp.count; k++) {
            if (vp.e[k].type != type_with_origin) continue;
            int j = 0; while (j < n && ids[j] != vp.e[k].id) j++;
            if (j == n && n < 16) ids[n++] = vp.e[k].id;
        }
    }
    return n;
}
static int dm_none(SOCKET s, int tries) {
    for (int i = 0; i < tries; i++) {
        CoopEventPacket vp;
        if (!recv_events(s, &vp, 1)) break;
        for (int k = 0; k < vp.count; k++) {
            int t = vp.e[k].type & 0xff;
            if (t == COOP_EV_DISH || t == COOP_EV_DISH_RESULT) return 0;
        }
    }
    return 1;
}

static int multidish_main(void) {
    WSADATA w; assert(!WSAStartup(MAKEWORD(2,2),&w));
    SOCKET ga = loop_socket(), gb = loop_socket(), gc = loop_socket();
    struct sockaddr_in target;
    _putenv("SAVAGEWIND_COOP_PEER="); _putenv("SAVAGEWIND_COOP_PORT=0"); _putenv("SAVAGEWIND_COOP_CODE=");
    _putenv("SAVAGEWIND_COOP_MULTIWORLD="); _putenv("SAVAGEWIND_COOP_RELAYRATE=");
    coop_net_init(); assert(coop_net_on());
    int len=sizeof target; getsockname(sock,(struct sockaddr *)&target,&len);
    target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    CoopState me={0}; me.pos[0]=1; me.flags=COOP_FLAG_IN_WORLD; coop_net_set_local(&me);
    CoopPacket a={0}; a.magic=COOP_MAGIC; a.version=COOP_VERSION; a.flags=1; a.session=901; a.seq=1; a.pos[0]=10;
    deliver(ga,&target,&a,0); a.echo=local_session; a.seq=2; deliver(ga,&target,&a,0);
    CoopPacket b=a; b.session=902; b.seq=1; b.pos[0]=12; deliver(gb,&target,&b,0);
    CoopPacket c=a; c.session=903; c.seq=1; c.pos[0]=14; deliver(gc,&target,&c,0);
    assert(count()==3 && coop_net_partner_id()==1);
    uint16_t type; int n, origin; unsigned char data[COOP_EVENT_DATA];
    while (coop_net_next_event(&type,data,&n)) {}
    uint64_t us, them;
    assert(coop_net_dish_session(3,&us,&them) && us==local_session && them==903);
    assert(!coop_net_dish_session(0,&us,&them) && !coop_net_dish_session(9,&us,&them));

    // 1. B's offer for C (to = 0): passed on to C with C's session, origin 2; the host does not apply it, A gets nothing
    CoopDishOffer o = dm_offer(902,0,2,3,1), got;
    assert(coop_dish_offer_ok(&o));
    ev_send(gb,&target,902,0,1,(uint16_t)(COOP_EV_DISH|(3<<8)),&o,sizeof o);
    assert(!coop_net_next_event_from(&type,data,&n,&origin));
    assert(dm_take(gc,(uint16_t)(COOP_EV_DISH|(2<<8)),&got,sizeof got,40));
    assert(got.to==903 && got.from==902 && got.from_pid==2 && got.to_pid==3 && !memcmp(&got.dish,&o.dish,sizeof o.dish));
    assert(dm_none(ga,6));
    // 2. bad ones from B: another sender's session, C's wrong session, a wrong sender / receiver id: all dropped
    CoopDishOffer bad = o; bad.from = 901;
    ev_send(gb,&target,902,0,2,(uint16_t)(COOP_EV_DISH|(3<<8)),&bad,sizeof bad);
    bad = o; bad.to = 777;
    ev_send(gb,&target,902,0,3,(uint16_t)(COOP_EV_DISH|(3<<8)),&bad,sizeof bad);
    bad = o; bad.from_pid = 1;
    ev_send(gb,&target,902,0,4,(uint16_t)(COOP_EV_DISH|(3<<8)),&bad,sizeof bad);
    bad = o; bad.to_pid = 1;
    ev_send(gb,&target,902,0,5,(uint16_t)(COOP_EV_DISH|(3<<8)),&bad,sizeof bad);
    assert(dm_ids(gc,(uint16_t)(COOP_EV_DISH|(2<<8)),6)==1 && dm_none(ga,4));
    assert(!coop_net_next_event_from(&type,data,&n,&origin));
    // 3. C's answer to B: passed on to B (origin 3) when it names B's session, dropped otherwise
    CoopDishResult r = {.from=903,.to=902,.id=1,.accepted=1,.from_pid=3,.to_pid=2}, rgot;
    assert(coop_dish_result_ok(&r));
    CoopDishResult rbad = r; rbad.to = 901;
    ev_send(gc,&target,903,0,1,(uint16_t)(COOP_EV_DISH_RESULT|(2<<8)),&rbad,sizeof rbad);
    ev_send(gc,&target,903,0,2,(uint16_t)(COOP_EV_DISH_RESULT|(2<<8)),&r,sizeof r);
    assert(dm_take(gb,(uint16_t)(COOP_EV_DISH_RESULT|(3<<8)),&rgot,sizeof rgot,40) && !memcmp(&rgot,&r,sizeof r));
    assert(!coop_net_next_event_from(&type,data,&n,&origin));
    // 4. B's offer for the host (target 0): ours, origin 2; one naming player 3 as receiver is not ours
    CoopDishOffer h = dm_offer(902,local_session,2,0,2);
    ev_send(gb,&target,902,0,6,COOP_EV_DISH,&h,sizeof h);
    assert(coop_net_next_event_from(&type,data,&n,&origin) && type==COOP_EV_DISH && origin==2 && n==sizeof h && !memcmp(data,&h,n));
    bad = h; bad.to_pid = 3;
    ev_send(gb,&target,902,0,7,COOP_EV_DISH,&bad,sizeof bad);
    bad = h; bad.from = 903;
    ev_send(gb,&target,902,0,8,COOP_EV_DISH,&bad,sizeof bad);
    assert(!coop_net_next_event_from(&type,data,&n,&origin));
    // 5. the host's own offer: to C only (C's session), never broadcast, never with a wrong session / id
    CoopDishOffer mine = dm_offer(local_session,903,0,3,1);
    assert(!coop_net_send_event(COOP_EV_DISH,&mine,sizeof mine));
    CoopDishOffer wrong = mine; wrong.to = 902; assert(!coop_net_send_event_to(3,COOP_EV_DISH,&wrong,sizeof wrong));
    wrong = mine; wrong.to_pid = 2; assert(!coop_net_send_event_to(3,COOP_EV_DISH,&wrong,sizeof wrong));
    wrong = mine; wrong.from_pid = 1; assert(!coop_net_send_event_to(3,COOP_EV_DISH,&wrong,sizeof wrong));
    assert(coop_net_send_event_to(3,COOP_EV_DISH,&mine,sizeof mine));
    assert(dm_take(gc,COOP_EV_DISH,&got,sizeof got,40) && !memcmp(&got,&mine,sizeof mine));
    assert(dm_none(ga,4) && dm_ids(gb,COOP_EV_DISH,4)==0);
    printf("PASS multi-dish: guest -> guest offer passed on with the receiver's session (origin, not applied, nobody "
           "else), bad sessions / ids dropped, the answer goes back the same way, the host's offer to one guest only\n");
    closesocket(ga); closesocket(gb); closesocket(gc);
    return 0;
}

static int multidish_guest_main(void) {
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
    uint64_t us, them;
    assert(coop_net_dish_session(2,&us,&them) && us==local_session && them==0);   // (another guest: not known here)
    assert(coop_net_dish_session(0,&us,&them) && them==801);
    assert(!coop_net_dish_session(3,&us,&them));                                  // (ourselves)
    // to player 2: to = 0, through the host (2 in the type's high byte); one naming a session is refused
    CoopDishOffer o = dm_offer(local_session,0,3,2,1), got;
    CoopDishOffer wrong = o; wrong.to = 555; assert(!coop_net_send_event_to(2,COOP_EV_DISH,&wrong,sizeof wrong));
    assert(coop_net_send_event_to(2,COOP_EV_DISH,&o,sizeof o));
    assert(dm_take(host,(uint16_t)(COOP_EV_DISH|(2<<8)),&got,sizeof got,40) && !memcmp(&got,&o,sizeof o));
    // to the host: its session
    CoopDishOffer oh = dm_offer(local_session,801,3,0,2);
    wrong = oh; wrong.to = 0; assert(!coop_net_send_event_to(0,COOP_EV_DISH,&wrong,sizeof wrong));
    assert(coop_net_send_event_to(0,COOP_EV_DISH,&oh,sizeof oh));
    assert(dm_take(host,COOP_EV_DISH,&got,sizeof got,40) && !memcmp(&got,&oh,sizeof oh));
    // an offer of player 2 passed on by the host (origin 2): ours only when it names us (id 3, our session)
    uint16_t type; int n, origin; unsigned char data[COOP_EVENT_DATA];
    while (coop_net_next_event(&type,data,&n)) {}
    CoopDishOffer in = dm_offer(902,local_session,2,3,5);
    CoopDishOffer other = in; other.to_pid = 1;
    CoopEventPacket p = {0};
    p.magic=COOP_EVENT_MAGIC; p.version=COOP_VERSION; p.session=801; p.echo=local_session; p.count=2;
    p.e[0]=(CoopEvent){.id=1,.type=(uint16_t)(COOP_EV_DISH|(2<<8)),.len=sizeof other}; memcpy(p.e[0].data,&other,sizeof other);
    p.e[1]=(CoopEvent){.id=2,.type=(uint16_t)(COOP_EV_DISH|(2<<8)),.len=sizeof in}; memcpy(p.e[1].data,&in,sizeof in);
    sendto(host,(char *)&p,sizeof p,0,(struct sockaddr *)&target,sizeof target); Sleep(150);
    assert(coop_net_next_event_from(&type,data,&n,&origin) && type==COOP_EV_DISH && origin==2 && !memcmp(data,&in,sizeof in));
    assert(!coop_net_next_event_from(&type,data,&n,&origin));
    printf("PASS multi-dish-guest: to another guest with to = 0 via the host, to the host with its session, "
           "a passed-on offer taken only when it names us\n");
    closesocket(host);
    return 0;
}

static int utf8_main(void) {
    char out[COOP_NAME_LEN];
    uint32_t cp;
    // good names
    assert(coop_name_ok("Jo\xc3\xa3o") && coop_name_ok("\xc3\x81" "drian Concei\xc3\xa7\xc3\xa3o") && coop_name_ok("Bia"));
    assert(coop_name_ok("") && coop_name_ok("\xe2\x82\xac\xf0\x9f\x98\x80"));  // (euro sign, an emoji)
    char full[COOP_NAME_LEN]; memset(full,'x',COOP_NAME_LEN-1); full[COOP_NAME_LEN-1]=0; assert(coop_name_ok(full));
    // bad names
    char unterminated[COOP_NAME_LEN]; memset(unterminated,'x',COOP_NAME_LEN); assert(!coop_name_ok(unterminated));
    char cut[COOP_NAME_LEN]; memset(cut,'x',COOP_NAME_LEN); cut[COOP_NAME_LEN-2]=(char)0xc3; cut[COOP_NAME_LEN-1]=(char)0xa3;
    assert(!coop_name_ok(cut));                                    // (a character over the field's end: no room for NUL)
    assert(!coop_name_ok("a\tb") && !coop_name_ok("a\x7f") && !coop_name_ok("\xc2\x85"));  // (C0, DEL, C1)
    assert(!coop_name_ok("\xc0\xaf") && !coop_name_ok("\xe0\x80\xaf") && !coop_name_ok("\xf0\x80\x80\xaf"));  // (overlong)
    assert(!coop_name_ok("\xed\xa0\x80") && !coop_name_ok("\xf4\x90\x80\x80"));  // (a surrogate, above U+10FFFF)
    assert(!coop_name_ok("\xc3") && !coop_name_ok("\xa3x") && !coop_name_ok("\xc3x") && !coop_name_ok("\xff"));
    assert(coop_utf8_char("\xc3\xa3", 2, &cp) == 2 && cp == 0xe3);
    assert(coop_utf8_char("\xe2\x80\xa6", 3, &cp) == 3 && cp == 0x2026);
    assert(coop_utf8_char("\xf0\x9f\x98\x80", 4, &cp) == 4 && cp == 0x1f600);
    assert(coop_utf8_char("\xe2\x80\xa6", 2, &cp) == 0);
    // cleaning: accents kept, control / broken bytes -> one '?', spaces trimmed, cut at a character boundary
    coop_name_clean("  Jo\xc3\xa3o ", out); assert(!strcmp(out, "Jo\xc3\xa3o"));
    coop_name_clean("H\xc3\xb4st\tName", out); assert(!strcmp(out, "H\xc3\xb4st?Name"));
    coop_name_clean("a\xff\xa3\xa3" "b\xc3", out); assert(!strcmp(out, "a?b?"));
    coop_name_clean("\xe0\x80\xaf" "x", out); assert(!strcmp(out, "?x"));
    coop_name_clean("Bartolomeu Fernandes Xxyz", out); assert(!strcmp(out, "Bartolomeu Fernandes Xx") && coop_name_ok(out));
    coop_name_clean("aaaaaaaaaaaaaaaaaaaaa\xc3\xa3\xc3\xa3", out);  // (21 + 2 + 2 bytes: the second one does not fit)
    assert(!strcmp(out, "aaaaaaaaaaaaaaaaaaaaa\xc3\xa3") && coop_name_ok(out));
    coop_name_clean("aaaaaaaaaaaaaaaaaaaaaa\xe2\x80\xa6", out);  // (22 + 3 bytes: cut before it)
    assert(!strcmp(out, "aaaaaaaaaaaaaaaaaaaaaa"));
    coop_name_clean(NULL, out); assert(!out[0]);
    for (int i = 0; i < 2000; i++) {  // (anything cleaned is a valid name)
        char in[40]; for (int k = 0; k < 39; k++) in[k] = (char)(rand() % 255 + 1); in[39] = 0;
        coop_name_clean(in, out); assert(coop_name_ok(out));
    }
    printf("PASS utf8: names are well-formed UTF-8 without control characters, terminated in the field; cleaning keeps "
           "accents, replaces broken sequences, cuts at a character boundary\n");
    return 0;
}
