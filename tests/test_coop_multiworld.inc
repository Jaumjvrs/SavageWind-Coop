// (protocol 58) `test_coop_net multi-world`: a host with three guests sharing one world (SAVAGEWIND_COOP_MULTIWORLD on,
// the default). Roles, merged enemy damage, each guest's own enemy packet (the others' damage summed, GUEST_RUNS for
// the guest that runs it, the owner's position relayed), the owner of an enemy only guests have (the nearest), events
// relayed with their origin (pairwise ones not), run-time enemies and their names relayed, id tags.
// `test_coop_net multi-world-guest`: a guest told it shares the world: the host is its partner, events from other
// players carry their origin, its run-time ids carry its id.

static void enemy_send(SOCKET s, struct sockaddr_in *target, uint64_t session, uint32_t seq, const CoopEnemyDamage *e,
                       int n, const CoopSpawnName *name) {
    CoopEnemyPacket ep = {0};
    ep.magic=COOP_ENEMY_MAGIC; ep.version=COOP_VERSION; ep.session=session; ep.echo=local_session; ep.seq=seq;
    ep.hour=-1; ep.count=(uint8_t)n;
    memcpy(ep.e,e,sizeof *e*(size_t)n);
    if (name) ep.names[0]=*name;
    sendto(s,(char *)&ep,sizeof ep,0,(struct sockaddr *)target,sizeof *target); Sleep(60);
}
static const CoopEnemyDamage *ep_find(const CoopEnemyPacket *p, uint32_t hash) {
    for (int i = 0; i < p->count; i++)
        if (p->e[i].hash == hash) return &p->e[i];
    return NULL;
}
// the next enemy packet to socket s whose entry `hash` passes `ok` (keeps the guest's link fresh meanwhile)
static const CoopEnemyDamage *wait_enemy(SOCKET s, CoopEnemyPacket *p, uint32_t hash, int (*ok)(const CoopEnemyDamage *)) {
    for (int i = 0; i < 80; i++) {
        if (!recv_kind(s,p,sizeof *p,COOP_ENEMY_MAGIC,NULL)) return NULL;
        const CoopEnemyDamage *e = ep_find(p,hash);
        if (e && (!ok || ok(e))) return e;
    }
    return NULL;
}
static int ev_has(SOCKET s, uint16_t type_with_origin, int tries) {
    for (int i = 0; i < tries; i++) {
        CoopEventPacket vp;
        if (!recv_events(s,&vp,1)) continue;  // (older datagrams may fill the socket first)
        for (int k = 0; k < vp.count; k++)
            if (vp.e[k].type == type_with_origin) return 1;
    }
    return 0;
}
static void ev_send(SOCKET s, struct sockaddr_in *target, uint64_t session, uint32_t ack, uint32_t id, uint16_t type,
                    const void *data, int len) {
    CoopEventPacket in = {0};
    in.magic=COOP_EVENT_MAGIC; in.version=COOP_VERSION; in.session=session; in.echo=local_session; in.ack=ack;
    in.count=1; in.e[0]=(CoopEvent){.id=id,.type=type,.len=(uint16_t)len};
    memcpy(in.e[0].data,data,(size_t)len);
    sendto(s,(char *)&in,sizeof in,0,(struct sockaddr *)target,sizeof *target); Sleep(120);
}
static int en_damage13_runs(const CoopEnemyDamage *e) { return e->damage==13 && (e->flags & COOP_ENEMY_GUEST_RUNS); }
static int en_damage10_pos(const CoopEnemyDamage *e) {
    return e->damage==10 && !(e->flags & COOP_ENEMY_GUEST_RUNS) && (e->flags & COOP_ENEMY_POS) && e->pos[0]==55;
}
static int en_runs(const CoopEnemyDamage *e) { return (e->flags & COOP_ENEMY_GUEST_RUNS) != 0; }
static int en_follow_b(const CoopEnemyDamage *e) {
    return !(e->flags & COOP_ENEMY_GUEST_RUNS) && (e->flags & COOP_ENEMY_POS) && e->pos[0]==101;
}
static int en_spawned(const CoopEnemyDamage *e) { return (e->flags & COOP_ENEMY_SPAWNED) != 0; }

static int multiworld_main(void) {
    WSADATA w; assert(!WSAStartup(MAKEWORD(2,2),&w));
    SOCKET ga = loop_socket(), gb = loop_socket(), gc = loop_socket();
    struct sockaddr_in target;
    _putenv("SAVAGEWIND_COOP_PEER="); _putenv("SAVAGEWIND_COOP_PORT=0"); _putenv("SAVAGEWIND_COOP_CODE=");
    _putenv("SAVAGEWIND_COOP_MULTIWORLD=");  // (default: on)
    coop_net_init(); assert(coop_net_on());
    int len=sizeof target; getsockname(sock,(struct sockaddr *)&target,&len);
    target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(coop_net_id_tag()==0x40000000u);
    CoopState me={0}; me.pos[0]=1; me.flags=COOP_FLAG_IN_WORLD; coop_net_set_local(&me);
    CoopPacket a={0}; a.magic=COOP_MAGIC; a.version=COOP_VERSION; a.flags=1; a.session=501; a.seq=1; a.pos[0]=10;
    deliver(ga,&target,&a,0); a.echo=local_session; a.seq=2; deliver(ga,&target,&a,0);
    CoopPacket b=a; b.session=502; b.seq=1; b.pos[0]=90; deliver(gb,&target,&b,0);
    CoopPacket c=a; c.session=503; c.seq=1; c.pos[0]=300; deliver(gc,&target,&c,0);
    assert(count()==3 && coop_net_partner_id()==1);
    CoopPacket r;
    assert(recv_kind(ga,&r,sizeof r,COOP_MAGIC,from_host) && r.your_id==1 && r.role==(COOP_ROLE_PARTNER|COOP_ROLE_WORLD));
    { int seen=0; for (int i=0;i<20 && !seen;i++) { if (!recv_kind(gb,&r,sizeof r,COOP_MAGIC,from_host)) break;
        seen = r.your_id==2 && r.role==COOP_ROLE_WORLD; } assert(seen); }
    assert(coop_net_world_players(2.0)==((1u<<1)|(1u<<2)|(1u<<3)));
    // (every guest keeps sending its state: a guest silent for 5 s loses the partner role, 30 s its place)
#define ALIVE() do { a.seq++; deliver(ga,&target,&a,0); b.seq++; deliver(gb,&target,&b,0); c.seq++; deliver(gc,&target,&c,0);                      assert(coop_net_partner_id()==1); } while (0)

    ALIVE();
    // merged damage: A 10, B 7 on enemy 500; B runs it (position 55); the host has it too (3) and says B owns it
    CoopEnemyDamage ea={500,10,COOP_ENEMY_LOADED}, eb={500,7,COOP_ENEMY_LOADED|COOP_ENEMY_POS,{55,0,0},{0,0,0,1},"Wait"};
    enemy_send(ga,&target,501,1,&ea,1,NULL);
    enemy_send(gb,&target,502,1,&eb,1,NULL);
    assert(coop_net_remote_damage(500)==17 && coop_net_enemy_damage_of(2,500)==7 && coop_net_enemy_damage_of(1,500)==10);
    { float p[3],q[4]; double age; assert(coop_net_enemy_pos_of(2,500,p,q,&age) && p[0]==55 && !coop_net_enemy_pos_of(1,500,p,q,&age)); }
    CoopEnemyDamage mine={500,3,COOP_ENEMY_LOADED|(2u<<COOP_ENEMY_OWNER_SHIFT)};
    coop_net_set_enemies(&mine,1);
    CoopEnemyPacket ep;
    for (int i=0;i<3;i++) enemy_send(gb,&target,502,2+i,&eb,1,NULL);
    // B: its own damage not added (3 + A's 10), GUEST_RUNS; A: 3 + B's 7, B's position to follow
    assert(wait_enemy(gb,&ep,500,en_damage13_runs));
    for (int i=0;i<3;i++) enemy_send(gb,&target,502,5+i,&eb,1,NULL);
    assert(wait_enemy(ga,&ep,500,en_damage10_pos));
    { const CoopEnemyDamage *e = ep_find(&ep,500); assert(!(e->flags & (COOP_ENEMY_OWNER_MASK|COOP_ENEMY_COPY))); }
    // merged position for the host's game: the owner's (B's)
    { float p[3],q[4]; double age; assert(coop_net_remote_enemy_pos(500,p,q,&age,NULL,0) && p[0]==55); }

    ALIVE();
    // an enemy only B and C have (far from the host): B (at 90) is nearer to it (at 100) than C (at 300): B runs it
    b.pos[0]=90; c.pos[0]=300; ALIVE();
    CoopEnemyDamage rb={600,0,COOP_ENEMY_LOADED|COOP_ENEMY_POS,{101,0,0},{0,0,0,1},""}, rc={600,0,COOP_ENEMY_LOADED};
    for (int i=0;i<4;i++) { enemy_send(gb,&target,502,20+i,&rb,1,NULL); enemy_send(gc,&target,503,20+i,&rc,1,NULL); }
    assert(wait_enemy(gb,&ep,600,en_runs));
    for (int i=0;i<4;i++) { enemy_send(gb,&target,502,30+i,&rb,1,NULL); enemy_send(gc,&target,503,30+i,&rc,1,NULL); }
    assert(wait_enemy(gc,&ep,600,en_follow_b));

    ALIVE();
    // B's run-time enemy (id with B's pid) and its name reach C (and A) as a spawn to copy
    uint32_t sid = 0x80000000u | (2u<<26) | 5u;
    assert(coop_net_id_origin(sid)==2 && coop_net_id_origin(0xC0000007u)==0);
    CoopSpawnName nm = {sid, "Enemy_Bokoblin_Junior"};
    CoopEnemyDamage sb={sid,0,COOP_ENEMY_LOADED|COOP_ENEMY_SPAWNED|COOP_ENEMY_POS,{70,0,0},{0,0,0,1},""};
    CoopEnemyDamage both[2]={rb,sb};
    for (int i=0;i<4;i++) enemy_send(gb,&target,502,40+i,both,2,&nm);
    { char out[48]; assert(coop_net_remote_spawn_name(sid,out,sizeof out) && !strcmp(out,"Enemy_Bokoblin_Junior")); }
    { int names=0, spawn=0;
      for (int i=0;i<60 && !(names && spawn);i++) {
          enemy_send(gb,&target,502,50+i,both,2,&nm);
          if (!recv_kind(gc,&ep,sizeof ep,COOP_ENEMY_MAGIC,NULL)) break;
          const CoopEnemyDamage *e = ep_find(&ep,sid);
          if (e && en_spawned(e) && e->pos[0]==70) spawn=1;
          for (int k=0;k<COOP_SPAWN_NAMES;k++) if (ep.names[k].id==sid && !strcmp(ep.names[k].name,nm.name)) names=1;
      }
      assert(spawn && names); }

    ALIVE();
    // events: B's ordinary event is ours (origin 2) and goes on to A and C with B's id; a pairwise one (an arrow area
    // effect) is ours only
    uint16_t type; int n, origin; unsigned char data[COOP_EVENT_DATA];
    while (coop_net_next_event(&type,data,&n)) {}
    ev_send(gb,&target,502,0,1,20,"hello",6);
    assert(coop_net_next_event_from(&type,data,&n,&origin) && type==20 && origin==2 && !strcmp((char *)data,"hello"));
    { int hc=ev_has(gc,(uint16_t)(20|(2<<8)),40), ha=ev_has(ga,(uint16_t)(20|(2<<8)),40);
      if (!hc || !ha) fprintf(stderr,"relay: C %d A %d; queues A %d B %d C %d; world %d %d %d\n",hc,ha,wl[1].out_count,wl[2].out_count,
                             wl[3].out_count,link_world(1),link_world(2),link_world(3));
      assert(hc && ha); }
    unsigned char arrow[8]={COOP_AREAFX_ARROW};
    ev_send(gb,&target,502,0,2,COOP_EV_AREAFX,arrow,sizeof arrow);
    assert(coop_net_next_event_from(&type,data,&n,&origin) && type==COOP_EV_AREAFX && origin==2);
    // (C keeps receiving B's first event until it acknowledges it: acknowledge, then no area effect may come)
    ev_send(gc,&target,503,1,0,0,"",0);  // (count 1 with id 0: an empty event that is ignored)
    { int bad=0; for (int i=0;i<8;i++) { CoopEventPacket vp; if (!recv_events(gc,&vp,1)) break;
        for (int k=0;k<vp.count;k++) if ((vp.e[k].type & 0xff)==COOP_EV_AREAFX) bad=1; } assert(!bad); }
    ALIVE();
    // the host's own: an ordinary event to everyone, a pairwise one to the partner (A) only
    assert(coop_net_send_event(21,"x",2));
    assert(ev_has(gb,21,40) && ev_has(gc,21,40) && ev_has(ga,21,40));
    assert(coop_net_send_event(COOP_EV_AREAFX,arrow,sizeof arrow));
    assert(ev_has(ga,COOP_EV_AREAFX,40));
    { int bad=0; for (int i=0;i<10;i++) { CoopEventPacket vp; if (!recv_events(gb,&vp,1)) break;
        for (int k=0;k<vp.count;k++) if ((vp.e[k].type & 0xff)==COOP_EV_AREAFX) bad=1; } assert(!bad); }
    // (white box: no pairwise event waits in B's or C's queue)
    for (int L=2;L<=3;L++) for (int k=0;k<wl[L].out_count;k++) assert((wl[L].out[k].type & 0xff)!=COOP_EV_AREAFX);
    ALIVE();
    // (80) fauna: each world guest gets its own set (B's animals near B, C's near C), A (partner) its own too
    { CoopFauna fa={.area=9,.count=1}, fb=fa, fc=fa;
      fa.center[0]=10; fa.a[0]=(CoopFaunaAnimal){.id=1,.species=1,.pos={11,0,0}};
      fb.center[0]=90; fb.a[0]=(CoopFaunaAnimal){.id=2,.species=2,.pos={91,0,0}};
      fc.center[0]=300; fc.a[0]=(CoopFaunaAnimal){.id=3,.species=3,.pos={301,0,0}};
      coop_net_set_fauna(&fa); coop_net_set_fauna_for(2,&fb); coop_net_set_fauna_for(3,&fc);
      CoopFaunaPacket fp;
      assert(recv_kind(ga,&fp,sizeof fp,COOP_FAUNA_MAGIC,NULL) && fp.f.a[0].id==1 && fp.echo==501);
      assert(recv_kind(gb,&fp,sizeof fp,COOP_FAUNA_MAGIC,NULL) && fp.f.a[0].id==2 && fp.echo==502);
      assert(recv_kind(gc,&fp,sizeof fp,COOP_FAUNA_MAGIC,NULL) && fp.f.a[0].id==3 && fp.echo==503);
      coop_net_set_fauna_for(0,&fb); coop_net_set_fauna_for(8,&fb); }  // (ignored: not a guest id)
    printf("PASS multi-world: roles, merged damage, per-guest enemy packets (summed damage, guest runs, owner's position), "
           "nearest guest runs a guest-only enemy, spawn + name relay, event relay with origin, pairwise events, id tags, "
           "per-guest fauna sets\n");
    closesocket(ga); closesocket(gb); closesocket(gc);
    return 0;
}

static int multiworld_guest_main(void) {
    WSADATA w; assert(!WSAStartup(MAKEWORD(2,2),&w));
    SOCKET host = loop_socket();
    struct sockaddr_in hostaddr, target; int len=sizeof hostaddr; getsockname(host,(struct sockaddr *)&hostaddr,&len);
    char setting[100]; snprintf(setting,sizeof setting,"SAVAGEWIND_COOP_PEER=127.0.0.1:%d",ntohs(hostaddr.sin_port));
    _putenv(setting); _putenv("SAVAGEWIND_COOP_PORT=0"); _putenv("SAVAGEWIND_COOP_CODE="); _putenv("SAVAGEWIND_COOP_MULTIWORLD=");
    coop_net_init(); assert(coop_net_on());
    len=sizeof target; getsockname(sock,(struct sockaddr *)&target,&len);
    target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    CoopPacket h={0}; h.magic=COOP_MAGIC; h.version=COOP_VERSION; h.flags=1; h.session=601; h.echo=local_session;
    h.seq=1; h.pos[0]=10; h.your_id=3; h.role=COOP_ROLE_WORLD; h.players=7;
    deliver(host,&target,&h,0); assert(count()==1);
    assert(coop_net_my_id()==3 && coop_net_partner_id()==0);
    CoopState rs; double age;
    assert(coop_net_get_remote(&rs,&age) && rs.pos[0]==10);  // (the host is our world partner)
    assert(coop_net_id_tag()==(3u<<26));
    // another world player's state (relayed): a world player
    CoopPacket o=h; o.origin=1; o.seq=50; o.pos[0]=20; deliver(host,&target,&o,0);
    assert(coop_net_world_players(2.0)==((1u<<0)|(1u<<1)));
    // our events go to the host
    assert(coop_net_send_event(7,"abc",4));
    { CoopEventPacket vp; assert(recv_events(host,&vp,1) && vp.e[0].type==7); }
    // the host's events: its own (origin 0) and one it relays from player 1; one claiming to be ours is dropped
    CoopEventPacket in={0}; in.magic=COOP_EVENT_MAGIC; in.version=COOP_VERSION; in.session=601; in.echo=local_session;
    in.count=3;
    in.e[0]=(CoopEvent){.id=1,.type=20,.len=1};
    in.e[1]=(CoopEvent){.id=2,.type=(uint16_t)(21|(1<<8)),.len=1};
    in.e[2]=(CoopEvent){.id=3,.type=(uint16_t)(22|(3<<8)),.len=1};
    sendto(host,(char *)&in,sizeof in,0,(struct sockaddr *)&target,sizeof target); Sleep(150);
    uint16_t type; int n, origin; unsigned char data[COOP_EVENT_DATA];
    assert(coop_net_next_event_from(&type,data,&n,&origin) && type==20 && origin==0);
    assert(coop_net_next_event_from(&type,data,&n,&origin) && type==21 && origin==1);
    assert(!coop_net_next_event_from(&type,data,&n,&origin));
    // with the off switch, a WORLD-only role is not a partner (phase-1 extra guest) - covered by multi-guest with role 0
    printf("PASS multi-world-guest: world role makes the host the partner, events with origin, own-origin dropped, id tag\n");
    closesocket(host);
    return 0;
}

// (protocol 59) `test_coop_net multi-objects`: physics objects with a host and three guests. Each guest's claims reach
// the host (picked by the newest claim, a tie: the lower id) and every other guest in a merged view naming the owner;
// a guest whose own claim wins does not get that object back; the host's own objects reach everyone; EV_OBJ is
// relayed with its origin; a bad owner id is rejected.
static void obj_send(SOCKET s, struct sockaddr_in *target, uint64_t session, uint32_t seq, uint32_t hash, uint16_t gen,
                     float x) {
    CoopObjPacket op = {0};
    op.magic=COOP_OBJ_MAGIC; op.version=COOP_VERSION; op.session=session; op.echo=local_session; op.seq=seq; op.count=1;
    op.o[0].hash=hash; op.o[0].gen=gen; op.o[0].flags=COOP_OBJ_OWNED; op.o[0].rot[3]=1; op.o[0].pos[0]=x;
    op.o[0].pid=5;  // (the host takes the sender's id, whatever it says)
    sendto(s,(char *)&op,sizeof op,0,(struct sockaddr *)target,sizeof *target); Sleep(40);
}
static uint32_t want_hash; static int want_pid, want_gen;
static const CoopObject *op_find(const CoopObjPacket *p, uint32_t hash) {
    for (int i = 0; i < p->count; i++)
        if (p->o[i].hash == hash) return &p->o[i];
    return NULL;
}
static int obj_wanted(const void *v) {
    const CoopObject *o = op_find(v, want_hash);
    return o && o->pid == want_pid && o->gen == want_gen;
}
// the next object datagrams to s: one with hash owned by pid at gen?
static int obj_seen(SOCKET s, uint32_t hash, int pid, int gen) {
    CoopObjPacket p; want_hash=hash, want_pid=pid, want_gen=gen;
    return recv_kind(s,&p,sizeof p,COOP_OBJ_MAGIC,obj_wanted);
}
// none of the next `tries` object datagrams to s has hash (and at least one came)
static int obj_absent(SOCKET s, uint32_t hash, int tries) {
    int got = 0;
    for (int i = 0; i < tries; i++) {
        CoopObjPacket p;
        if (!recv_kind(s,&p,sizeof p,COOP_OBJ_MAGIC,NULL)) continue;
        got++;
        if (op_find(&p,hash)) return 0;
    }
    return got > 0;
}
static void drain(SOCKET s) {  // (old datagrams queued before a change)
    unsigned char wire[MAX_DATAGRAM + COOP_TAG + 1];
    DWORD tmo = 1; setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,(char *)&tmo,sizeof tmo);
    for (int i = 0; i < 2000 && recv(s,(char *)wire,sizeof wire,0) > 0; i++) {}
}
static int multiobj_main(void) {
    WSADATA w; assert(!WSAStartup(MAKEWORD(2,2),&w));
    SOCKET ga = loop_socket(), gb = loop_socket(), gc = loop_socket();
    struct sockaddr_in target;
    _putenv("SAVAGEWIND_COOP_PEER="); _putenv("SAVAGEWIND_COOP_PORT=0"); _putenv("SAVAGEWIND_COOP_CODE=");
    _putenv("SAVAGEWIND_COOP_MULTIWORLD=");  // (default: on)
    coop_net_init(); assert(coop_net_on());
    int len=sizeof target; getsockname(sock,(struct sockaddr *)&target,&len);
    target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    CoopState me={0}; me.pos[0]=1; me.flags=COOP_FLAG_IN_WORLD; coop_net_set_local(&me);
    CoopPacket a={0}; a.magic=COOP_MAGIC; a.version=COOP_VERSION; a.flags=1; a.session=501; a.seq=1;
    deliver(ga,&target,&a,0); a.echo=local_session; a.seq=2; deliver(ga,&target,&a,0);
    CoopPacket b=a; b.session=502; b.seq=1; deliver(gb,&target,&b,0);
    CoopPacket c=a; c.session=503; c.seq=1; deliver(gc,&target,&c,0);
    assert(count()==3 && coop_net_partner_id()==1);
    // validation: the owner id must be a player id
    { CoopObject o={0}; o.hash=1; o.rot[3]=1; assert(coop_object_ok(&o)); o.pid=COOP_MAX_PLAYERS; assert(!coop_object_ok(&o)); }
    uint32_t s=1;
    // the host's own object 901 reaches every guest, owner 0
    CoopObject mine={.hash=901,.gen=1,.flags=COOP_OBJ_OWNED}; mine.rot[3]=1;
    coop_net_set_objects(&mine,1);
    assert(obj_seen(ga,901,0,1) && obj_seen(gb,901,0,1) && obj_seen(gc,901,0,1));
    ALIVE();
    // A (player 1) moves 900 (claim 1): the host follows A; B and C get it with owner 1
    CoopObject ro; double age;
    for (int i=0;i<3;i++) obj_send(ga,&target,501,s++,900,1,10);
    assert(coop_net_remote_object(900,&ro,&age) && ro.pid==1 && ro.gen==1 && ro.pos[0]==10);
    for (int i=0;i<3;i++) obj_send(ga,&target,501,s++,900,1,10);
    assert(obj_seen(gb,900,1,1));
    for (int i=0;i<3;i++) obj_send(ga,&target,501,s++,900,1,10);
    assert(obj_seen(gc,900,1,1));
    ALIVE();
    // B (player 2) takes it over (claim 2): the host's pick, A and C get B's; B does not get its own object back
    for (int i=0;i<3;i++) { obj_send(ga,&target,501,s++,900,1,10); obj_send(gb,&target,502,s++,900,2,20); }
    assert(coop_net_remote_object(900,&ro,&age) && ro.pid==2 && ro.gen==2 && ro.pos[0]==20);
    for (int i=0;i<3;i++) obj_send(gb,&target,502,s++,900,2,20);
    assert(obj_seen(ga,900,2,2));
    for (int i=0;i<3;i++) obj_send(gb,&target,502,s++,900,2,20);
    assert(obj_seen(gc,900,2,2));
    for (int i=0;i<3;i++) obj_send(gb,&target,502,s++,900,2,20);
    drain(gb);
    assert(obj_absent(gb,900,6));
    ALIVE();
    // C claims the same generation (a race): the lower id (B) wins: C gets B's, B still gets nothing for it
    for (int i=0;i<4;i++) { obj_send(gb,&target,502,s++,900,2,20); obj_send(gc,&target,503,s++,900,2,30); }
    assert(coop_net_remote_object(900,&ro,&age) && ro.pid==2);
    for (int i=0;i<3;i++) { obj_send(gb,&target,502,s++,900,2,20); obj_send(gc,&target,503,s++,900,2,30); }
    assert(obj_seen(gc,900,2,2));
    for (int i=0;i<3;i++) { obj_send(gb,&target,502,s++,900,2,20); obj_send(gc,&target,503,s++,900,2,30); }
    drain(gb);
    assert(obj_absent(gb,900,6));
    ALIVE();
    // the host claims it too at that generation: a tie goes to the host (0): every guest gets the host's
    CoopObject two[2]={mine,{.hash=900,.gen=2,.flags=COOP_OBJ_OWNED}}; two[1].rot[3]=1; two[1].pos[0]=40;
    coop_net_set_objects(two,2);
    for (int i=0;i<3;i++) { obj_send(gb,&target,502,s++,900,2,20); obj_send(gc,&target,503,s++,900,2,30); }
    assert(obj_seen(gb,900,0,2)); drain(gc); assert(obj_seen(gc,900,0,2));  // (drain: name packets fill the queue too)
    coop_net_set_objects(&mine,1);
    ALIVE();
    // EV_OBJ (8) from C reaches the host (origin 3) and goes on to A and B with C's id
    uint16_t type; int n, origin; unsigned char data[COOP_EVENT_DATA];
    while (coop_net_next_event(&type,data,&n)) {}
    unsigned char evo[48]={0};
    ev_send(gc,&target,503,0,1,8,evo,sizeof evo);
    assert(coop_net_next_event_from(&type,data,&n,&origin) && type==8 && origin==3);
    assert(ev_has(ga,(uint16_t)(8|(3<<8)),40) && ev_has(gb,(uint16_t)(8|(3<<8)),40));
    printf("PASS multi-objects: claims from every guest, merged views with the owner's id, own claim not sent back, "
           "ties to the lower id (host first), host's objects to all, EV_OBJ relayed with origin, owner id validated\n");
    closesocket(ga); closesocket(gb); closesocket(gc);
    return 0;
}

// (protocol 59) `test_coop_net multi-objects-guest`: a world guest takes the host's merged objects with their owner
// ids; an entry naming us as its owner is dropped; a bad owner id rejects the entry; ours go out with our id.
static int multiobj_guest_main(void) {
    WSADATA w; assert(!WSAStartup(MAKEWORD(2,2),&w));
    SOCKET host = loop_socket();
    struct sockaddr_in hostaddr, target; int len=sizeof hostaddr; getsockname(host,(struct sockaddr *)&hostaddr,&len);
    char setting[100]; snprintf(setting,sizeof setting,"SAVAGEWIND_COOP_PEER=127.0.0.1:%d",ntohs(hostaddr.sin_port));
    _putenv(setting); _putenv("SAVAGEWIND_COOP_PORT=0"); _putenv("SAVAGEWIND_COOP_CODE="); _putenv("SAVAGEWIND_COOP_MULTIWORLD=");
    coop_net_init(); assert(coop_net_on());
    len=sizeof target; getsockname(sock,(struct sockaddr *)&target,&len);
    target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    CoopPacket h={0}; h.magic=COOP_MAGIC; h.version=COOP_VERSION; h.flags=1; h.session=601; h.echo=local_session;
    h.seq=1; h.your_id=3; h.role=COOP_ROLE_WORLD; h.players=15;
    deliver(host,&target,&h,0); assert(count()==1 && coop_net_my_id()==3);
    CoopObjPacket op={0}; op.magic=COOP_OBJ_MAGIC; op.version=COOP_VERSION; op.session=601; op.echo=local_session;
    op.seq=1; op.count=3;
    for (int i=0;i<3;i++) { op.o[i].hash=700+i; op.o[i].gen=4; op.o[i].flags=COOP_OBJ_OWNED; op.o[i].rot[3]=1; }
    op.o[0].pid=1; op.o[1].pid=3; op.o[2].pid=9;
    sendto(host,(char *)&op,sizeof op,0,(struct sockaddr *)&target,sizeof target); Sleep(120);
    CoopObject ro; double age;
    assert(coop_net_remote_object(700,&ro,&age) && ro.pid==1 && ro.gen==4);  // player 1's, via the host
    assert(!coop_net_remote_object(701,&ro,&age));                           // names us: ours, dropped
    assert(!coop_net_remote_object(702,&ro,&age));                           // owner id out of range
    // ownership moves to the host (same hash, newer packet)
    op.seq=2; op.count=1; op.o[0].pid=0; op.o[0].gen=5;
    sendto(host,(char *)&op,sizeof op,0,(struct sockaddr *)&target,sizeof target); Sleep(120);
    assert(coop_net_remote_object(700,&ro,&age) && ro.pid==0 && ro.gen==5);
    // our own objects go to the host with our id
    CoopObject mine={.hash=710,.gen=1,.flags=COOP_OBJ_OWNED}; mine.rot[3]=1;
    coop_net_set_objects(&mine,1);
    { CoopObjPacket q; want_hash=710, want_pid=3, want_gen=1; assert(recv_kind(host,&q,sizeof q,COOP_OBJ_MAGIC,obj_wanted)); }
    printf("PASS multi-objects-guest: merged objects with owner ids, own id dropped, bad id rejected, owner change, own id sent\n");
    closesocket(host);
    return 0;
}
