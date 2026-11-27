// (n6, protocol 61) `test_coop_net multi-handover`: one enemy, a host with more enemies loaded than one packet holds (21) and two
// world guests that both have it loaded. Guest A (1) runs it, then the host hands it to guest B (2). In every guest
// packet the enemy must be there, so that each guest's rule (enemies.inc enemy_owner: a guest runs an enemy when the
// host says GUEST_RUNS or when the host's entry is missing / not LOADED) names exactly one runner, before and after
// the hand-over. Seen in a 3-game test before the fix: the host's own list (16 entries) left the enemy out, both guests
// heard nothing of it and both ran it. SAVAGEWIND_COOP_ENEMYPICK=0 brings the old order back (this test then fails).

#define HO_ENEMY 777u
static int ho_guest_runs(const CoopEnemyPacket *p) {  // a guest's rule (enemies.inc enemy_owner, guest branch)
    const CoopEnemyDamage *e = ep_find(p, HO_ENEMY);
    bool host_runs = e && (e->flags & COOP_ENEMY_LOADED) && !(e->flags & COOP_ENEMY_GUEST_RUNS);
    return !host_runs;
}
static int ho_has(const void *v) { return ep_find(v, HO_ENEMY) != NULL; }
// the next enemy packet to s (the guests' reports kept fresh meanwhile by `keep`)
static int ho_next(SOCKET s, CoopEnemyPacket *p, void (*keep)(void)) {
    for (int i = 0; i < 40; i++) {
        keep();
        if (recv_kind(s, p, sizeof *p, COOP_ENEMY_MAGIC, NULL)) return 1;
    }
    return 0;
}
static SOCKET ho_a, ho_b;
static struct sockaddr_in ho_target;
static uint32_t ho_seq = 100;
static CoopEnemyDamage ho_ra, ho_rb;  // what A and B report about the enemy
static CoopPacket ho_sa, ho_sb;
static void ho_keep(void) {
    ho_sa.seq++; deliver(ho_a, &ho_target, &ho_sa, 0);
    ho_sb.seq++; deliver(ho_b, &ho_target, &ho_sb, 0);
    ho_seq++;
    enemy_send(ho_a, &ho_target, 501, ho_seq, &ho_ra, 1, NULL);
    enemy_send(ho_b, &ho_target, 502, ho_seq, &ho_rb, 1, NULL);
}
// both guests' newest packets: exactly one runner, and it is `want` (1: A, 2: B)
static int ho_one_runner(int want, const char *when) {
    CoopEnemyPacket pa, pb;
    int ok = 0;
    for (int round = 0; round < 6 && !ok; round++) {
        drain(ho_a); drain(ho_b);
        if (!ho_next(ho_a, &pa, ho_keep) || !ho_next(ho_b, &pb, ho_keep)) continue;
        int ra = ho_guest_runs(&pa), rb = ho_guest_runs(&pb);
        ok = ho_has(&pa) && ho_has(&pb) && ra + rb == 1 && (want == 1 ? ra : rb);
        if (!ok)
            fprintf(stderr, "%s: A has it %d runs %d (count %d), B has it %d runs %d (count %d)\n", when, ho_has(&pa),
                    ra, pa.count, ho_has(&pb), rb, pb.count);
    }
    return ok;
}

static int multihandover_main(void) {
    WSADATA w; assert(!WSAStartup(MAKEWORD(2,2),&w));
    ho_a = loop_socket(), ho_b = loop_socket();
    _putenv("SAVAGEWIND_COOP_PEER="); _putenv("SAVAGEWIND_COOP_PORT=0"); _putenv("SAVAGEWIND_COOP_CODE=");
    _putenv("SAVAGEWIND_COOP_MULTIWORLD=");
    coop_net_init(); assert(coop_net_on());
    int len=sizeof ho_target; getsockname(sock,(struct sockaddr *)&ho_target,&len);
    ho_target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    CoopState me={0}; me.pos[0]=1; me.flags=COOP_FLAG_IN_WORLD; coop_net_set_local(&me);
    CoopPacket a={0}; a.magic=COOP_MAGIC; a.version=COOP_VERSION; a.flags=1; a.session=501; a.seq=1; a.pos[0]=50;
    deliver(ho_a,&ho_target,&a,0); a.echo=local_session; a.seq=2; deliver(ho_a,&ho_target,&a,0);
    CoopPacket b=a; b.session=502; b.seq=1; b.pos[0]=60; deliver(ho_b,&ho_target,&b,0);
    ho_sa=a, ho_sb=b;
    assert(count()==2 && coop_net_partner_id()==1);
    // the host's list: 20 enemies it runs (nearer to the host), then the one A runs (21st: beyond one packet)
    CoopEnemyDamage list[COOP_ENEMY_LOCAL_MAX];
    int n = 0;
    for (int i = 0; i < 20; i++)
        list[n++] = (CoopEnemyDamage){1000u + (uint32_t)i, 0, COOP_ENEMY_LOADED | COOP_ENEMY_POS, {1, 0, 0}, {0, 0, 0, 1}, "Wait"};
    list[n++] = (CoopEnemyDamage){HO_ENEMY, 0, COOP_ENEMY_LOADED | COOP_ENEMY_GUEST_RUNS | (1u << COOP_ENEMY_OWNER_SHIFT)};
    coop_net_set_enemies(list, n);
    ho_ra = (CoopEnemyDamage){HO_ENEMY, 0, COOP_ENEMY_LOADED | COOP_ENEMY_POS, {55, 0, 0}, {0, 0, 0, 1}, "Wait"};
    ho_rb = (CoopEnemyDamage){HO_ENEMY, 0, COOP_ENEMY_LOADED};
    for (int i = 0; i < 4; i++) ho_keep();
    assert(ho_one_runner(1, "A runs it"));
    { CoopEnemyPacket pb; int pos = 0;  // (B follows A's position)
      for (int i = 0; i < 10 && !pos; i++) if (ho_next(ho_b, &pb, ho_keep)) {
          const CoopEnemyDamage *e = ep_find(&pb, HO_ENEMY);
          pos = e && (e->flags & COOP_ENEMY_POS) && e->pos[0] == 55; }
      assert(pos); }
    // the hand-over: the host names B; A stops sending a position, B starts
    list[n - 1].flags = COOP_ENEMY_LOADED | COOP_ENEMY_GUEST_RUNS | (2u << COOP_ENEMY_OWNER_SHIFT);
    coop_net_set_enemies(list, n);
    ho_ra = (CoopEnemyDamage){HO_ENEMY, 0, COOP_ENEMY_LOADED};
    ho_rb = (CoopEnemyDamage){HO_ENEMY, 0, COOP_ENEMY_LOADED | COOP_ENEMY_POS, {66, 0, 0}, {0, 0, 0, 1}, "Wait"};
    for (int i = 0; i < 2; i++) ho_keep();
    assert(ho_one_runner(2, "B runs it"));
    // back to the host (owner 0): neither guest runs it
    list[n - 1].flags = COOP_ENEMY_LOADED | COOP_ENEMY_POS;
    coop_net_set_enemies(list, n);
    ho_rb = (CoopEnemyDamage){HO_ENEMY, 0, COOP_ENEMY_LOADED};
    for (int i = 0; i < 2; i++) ho_keep();
    { int ok = 0;
      for (int round = 0; round < 6 && !ok; round++) {
          CoopEnemyPacket pa, pb; drain(ho_a); drain(ho_b);
          if (ho_next(ho_a, &pa, ho_keep) && ho_next(ho_b, &pb, ho_keep))
              ok = ho_has(&pa) && ho_has(&pb) && !ho_guest_runs(&pa) && !ho_guest_runs(&pb);
      }
      assert(ok); }
    // (protocol 61) whom an enemy looks at, by player id: A runs 777 and it looks at B (id 2): B's packet says so
    // (relayed with A's position); the host's own 1000 looks at its partner A (LOOK_PARTNER + id 1): A gets both, B
    // only the id; the host's 1001 with the old flags only (no id): B gets no look
    list[n - 1].flags = COOP_ENEMY_LOADED | COOP_ENEMY_GUEST_RUNS | (1u << COOP_ENEMY_OWNER_SHIFT);
    list[0].flags |= COOP_ENEMY_LOOK | COOP_ENEMY_LOOK_PARTNER | COOP_ENEMY_LOOK_PID | (1u << COOP_ENEMY_LOOK_PID_SHIFT);
    list[1].flags |= COOP_ENEMY_LOOK | COOP_ENEMY_LOOK_PARTNER;
    coop_net_set_enemies(list, n);
    ho_ra = (CoopEnemyDamage){HO_ENEMY, 0, COOP_ENEMY_LOADED | COOP_ENEMY_POS | COOP_ENEMY_LOOK | COOP_ENEMY_LOOK_BATTLE |
                              COOP_ENEMY_LOOK_PID | (2u << COOP_ENEMY_LOOK_PID_SHIFT), {55, 0, 0}, {0, 0, 0, 1}, "Wait"};
    ho_rb = (CoopEnemyDamage){HO_ENEMY, 0, COOP_ENEMY_LOADED};
    for (int i = 0; i < 2; i++) ho_keep();
    { int ok = 0;
      for (int round = 0; round < 8 && !ok; round++) {
          CoopEnemyPacket pa, pb; drain(ho_a); drain(ho_b);
          if (!ho_next(ho_a, &pa, ho_keep) || !ho_next(ho_b, &pb, ho_keep)) continue;
          const CoopEnemyDamage *b777 = ep_find(&pb, HO_ENEMY), *a1000 = ep_find(&pa, 1000), *b1000 = ep_find(&pb, 1000),
                                *b1001 = ep_find(&pb, 1001);
          uint32_t want = COOP_ENEMY_LOOK | COOP_ENEMY_LOOK_PID | COOP_ENEMY_LOOK_BATTLE | COOP_ENEMY_POS;
          ok = b777 && (b777->flags & want) == want && !(b777->flags & COOP_ENEMY_LOOK_PARTNER) &&
               ((b777->flags & COOP_ENEMY_LOOK_PID_MASK) >> COOP_ENEMY_LOOK_PID_SHIFT) == 2 && b777->pos[0] == 55 &&
               a1000 && (a1000->flags & COOP_ENEMY_LOOK_PARTNER) && (a1000->flags & COOP_ENEMY_LOOK_PID) &&
               b1000 && (b1000->flags & COOP_ENEMY_LOOK) && !(b1000->flags & COOP_ENEMY_LOOK_PARTNER) &&
               ((b1000->flags & COOP_ENEMY_LOOK_PID_MASK) >> COOP_ENEMY_LOOK_PID_SHIFT) == 1 &&
               b1001 && !(b1001->flags & (COOP_ENEMY_LOOK | COOP_ENEMY_LOOK_PID));
          if (!ok) fprintf(stderr, "look: b777 %x a1000 %x b1000 %x b1001 %x\n", b777 ? b777->flags : 0,
                           a1000 ? a1000->flags : 0, b1000 ? b1000->flags : 0, b1001 ? b1001->flags : 0);
      }
      assert(ok); }
    // the host's own game: A's entry as received keeps the id; an id without LOOK is dropped
    { uint32_t f = 0; double age = 99;
      assert(coop_net_enemy_flags_of(1, HO_ENEMY, &f, &age) && (f & COOP_ENEMY_LOOK_PID) &&
             ((f & COOP_ENEMY_LOOK_PID_MASK) >> COOP_ENEMY_LOOK_PID_SHIFT) == 2);
      ho_ra.flags = COOP_ENEMY_LOADED | COOP_ENEMY_POS | COOP_ENEMY_LOOK_PID | (3u << COOP_ENEMY_LOOK_PID_SHIFT);
      for (int i = 0; i < 3; i++) ho_keep();
      assert(coop_net_enemy_flags_of(1, HO_ENEMY, &f, &age) && !(f & (COOP_ENEMY_LOOK_PID | COOP_ENEMY_LOOK_PID_MASK))); }
    printf("PASS multi-handover: an enemy a guest runs reaches every guest although the host has 21 enemies loaded; "
           "exactly one runner before and after a hand-over between guests, none after it went back to the host; "
           "(61) look-at player ids relayed unchanged, LOOK_PARTNER only to the partner, no id without LOOK\n");
    closesocket(ho_a); closesocket(ho_b);
    return 0;
}
