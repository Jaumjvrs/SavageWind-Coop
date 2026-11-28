// Part of runtime/coop.c (included there, same translation unit): the `coop` dev command and reset.
static uint64_t player_pos(float p[3]) {
    uint64_t pinfo = g_site.player_info ? LD64(g_site.player_info) : 0;
    if (!pinfo) return 0;
    memcpy(p, (void *)(uintptr_t)(pinfo + 0x88), 12);
    return pinfo;
}

static void cmd_coop(int argc, char **argv, ControlReply *r) {
    const char *usage = "Usage: coop spawn|spawnsync|preload <actor> [x y z] | outfit | armor <name> | despawn | status";
    if (argc >= 2 && !strcmp(argv[1], "split")) {
        if (argc == 3 && !strcmp(argv[2], "status")) {
            control_field_int(r, "pid", coop_split_pid());
            control_field_int(r, "count", coop_split_count());  // (4-player split screen) players started
            control_field_int(r, "last", coop_split_last_player());
            control_field_int(r, "pending", coop_split_pending());
            control_field_int(r, "failed", coop_split_failed());
            control_field_int(r, "audio", g_settings.split_audio);
            control_field_str(r, "audio_device", g_settings.split_audio_device);
            return;
        }
        // (coop-splitaudio, dev) the playback devices; Player 2 sound as the menu sets it (applies at the next add)
        if (argc == 3 && !strcmp(argv[2], "devices")) {
            char names[16][128], def[128] = "", list[2200] = "";
            int n = split_audio_devices(names, 16);
            for (int i = 0; i < n; i++)
                snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s", i ? " | " : "", names[i]);
            split_audio_default_name(def, sizeof def);
            control_field_int(r, "count", n);
            control_field_str(r, "devices", list);
            control_field_str(r, "default", def);
            LOG("[splitaudio] %d playback devices: %s (default now: %s)\n", n, list, def);
            return;
        }
        if (argc >= 4 && !strcmp(argv[2], "audio")) {
            int mode = !strcmp(argv[3], "off") ? SPLIT_AUDIO_OFF : !strcmp(argv[3], "device") ? SPLIT_AUDIO_DEVICE
                     : !strcmp(argv[3], "shared") ? SPLIT_AUDIO_SHARED : -1;
            if (mode < 0 || (mode != SPLIT_AUDIO_DEVICE && argc != 4)) {
                control_fail(r, "Usage: coop split audio off | shared | device [name words...]");
                return;
            }
            g_settings.split_audio = mode;
            if (mode == SPLIT_AUDIO_DEVICE) {
                char name[128] = "";
                for (int i = 4; i < argc; i++)
                    snprintf(name + strlen(name), sizeof name - strlen(name), "%s%s", i > 4 ? " " : "", argv[i]);
                snprintf(g_settings.split_audio_device, sizeof g_settings.split_audio_device, "%s", name);
            }
            control_field_int(r, "audio", mode);
            control_field_str(r, "audio_device", g_settings.split_audio_device);
            return;
        }
        bool add = argc >= 3 && !strcmp(argv[2], "add");
        bool remove = argc == 3 && !strcmp(argv[2], "remove");
        int layout = COOP_SPLIT_TOP, pad = 2;
        if (add && argc >= 4 && !strcmp(argv[3], "left")) layout = COOP_SPLIT_LEFT;
        else if (add && argc >= 4 && strcmp(argv[3], "top")) add = false;
        if (add && argc == 5)
            pad = strlen(argv[4]) == 1 ? argv[4][0] - '0' : 0;
        if ((!add && !remove) || argc > 5 || pad < 1 || pad > 4) {
            control_fail(r, "Usage: coop split add [top|left] [1-4] | remove | status");
        } else if (!coop_split_request(add, layout, pad)) {
            control_fail(r, "Split-screen request unavailable");
        } else {
            control_field_str(r, "state", "queued");
        }
        return;
    }
    if (argc == 5 && !strcmp(argv[1], "lightning")) {
        if (!coop_net_is_host() || !lightning_shock_orig) {
            control_fail(r, "Native co-op lightning is unavailable or this game is not the host");
            return;
        }
        CoopLightning strike = {.at_ns = 1, .area = 1};
        for (int i = 0; i < 3; i++) {
            char *end;
            strike.pos[i] = strtof(argv[i + 2], &end);
            if (end == argv[i + 2] || *end) {
                control_fail(r, "Usage: coop lightning <x> <y> <z>");
                return;
            }
        }
        if (!coop_lightning_ok(&strike)) {
            control_fail(r, "Invalid lightning position");
            return;
        }
        pthread_mutex_lock(&lightning_lock);
        memcpy(lightning_dev.pos, strike.pos, sizeof strike.pos);
        lightning_dev.pending = true;
        pthread_mutex_unlock(&lightning_lock);
        control_field_str(r, "state", "queued");
        return;
    }
    if (argc == 2 && !strcmp(argv[1], "bloodmoon")) {
        if (!coop_net_on() || !coop_net_is_host()) {
            control_fail(r, "Only the co-op host can request a blood moon");
            return;
        }
        __atomic_store_n(&bloodmoon_dev, true, __ATOMIC_RELEASE);
        control_field_str(r, "state", "queued");
        return;
    }
    if (argc >= 2 && !strcmp(argv[1], "puppet")) {  // puppet off | fixed x y z [yaw] | follow dx dy dz [yaw]
        bool fixed = argc >= 3 && !strcmp(argv[2], "fixed"), follow = argc >= 3 && !strcmp(argv[2], "follow");
        if (argc == 3 && !strcmp(argv[2], "off")) { puppet.mode = PUPPET_OFF; control_field_str(r, "mode", "off"); return; }
        if (!(fixed || follow) || (argc != 6 && argc != 7)) {
            control_fail(r, "Usage: coop puppet off | fixed x y z [yaw] | follow dx dy dz [yaw]");
            return;
        }
        if (!req.actor) { control_fail(r, "Spawn an actor first (coop spawnsync)"); return; }
        float v[4] = {0, 0, 0, 0};
        for (int i = 0; i < argc - 3; i++) {
            char *end;
            v[i] = strtof(argv[3 + i], &end);
            if (*end) { control_fail(r, "Cannot read \"%s\"", argv[3 + i]); return; }
        }
        puppet.mode = PUPPET_OFF;
        puppet.actor = req.actor;
        memcpy(puppet.pos, v, 12);
        puppet.yaw = v[3];
        puppet.frames = 0;
        __atomic_store_n(&puppet.mode, fixed ? PUPPET_FIXED : PUPPET_FOLLOW, __ATOMIC_RELEASE);
        control_field_str(r, "mode", fixed ? "fixed" : "follow");
        return;
    }
    if (argc >= 3 && argc <= 5 && !strcmp(argv[1], "anim")) {
        puppet.anim_actor = argc == 5 ? strtoull(argv[4], NULL, 0) : 0;
        if (!req.actor && !puppet.anim_actor) { control_fail(r, "Spawn an actor first (coop spawnsync)"); return; }
        if (strlen(argv[2]) >= sizeof puppet.anim) { control_fail(r, "Name too long"); return; }
        snprintf(puppet.anim, sizeof puppet.anim, "%s", argv[2]);
        puppet.anim_slot = argc >= 4 ? atoi(argv[3]) : 0;
        puppet.play = 1;
        control_field_str(r, "anim", puppet.anim);
        return;
    }
    if (argc == 2 && !strcmp(argv[1], "showbody")) {
        if (!req.actor) { control_fail(r, "Spawn an actor first (coop spawnsync)"); return; }
        char a[32]; snprintf(a, sizeof a, "0x%llx", (unsigned long long)LD64(req.actor + ACTOR_MODEL));
        control_field_str(r, "model", a);
        puppet.show_body = 1;
        return;
    }
    if ((argc == 3 || argc == 4) && !strcmp(argv[1], "armor")) {
        if (!avatar.actor && !req.actor && argc == 3) { control_fail(r, "Spawn an actor first"); return; }
        if (strlen(argv[2]) >= sizeof puppet.armor_name) { control_fail(r, "Name too long"); return; }
        if (argc == 4) {  // `coop armor <name> player|<actor address>`: choose the wearer (test)
            uint64_t pinfo = g_site.player_info ? LD64(g_site.player_info) : 0;
            puppet.armor_parent = !strcmp(argv[3], "player") ? (pinfo ? LD64(pinfo + 0x60) : 0)
                                                             : strtoull(argv[3], NULL, 0);
            if (!puppet.armor_parent) { control_fail(r, "No such wearer"); return; }
        }
        if (!puppet.armor_parent) puppet.armor_parent = avatar.actor ? avatar.actor : req.actor;
        snprintf(puppet.armor_name, sizeof puppet.armor_name, "%s", argv[2]);
        puppet.armor = 1;
        control_field_str(r, "state", "queued");
        return;
    }
    if ((argc == 3 || argc == 4) && !strcmp(argv[1], "getdemo")) {  // coop getdemo <item> [panel|flow|dialog] (dev)
        if (argc == 4) getdemo_push_mode(argv[2], !strcmp(argv[3], "flow") ? 1 : !strcmp(argv[3], "dialog") ? 2 : 0);
        else getdemo_push(argv[2]);
        control_field_str(r, "getdemo", "queued");
        return;
    }
    if (argc == 2 && !strcmp(argv[1], "trunk")) {  // coop trunk (dev): felled trunks / log pieces near us, to the log
        trunk_dump_dev = 1;
        control_field_str(r, "trunk", "queued");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "trunk") && !strcmp(argv[2], "tip")) {  // coop trunk tip (dev): our trunks tip over
        trunk_tip_dev = 1;
        control_field_str(r, "trunk", "tip queued");
        return;
    }
    if (argc == 2 && (!strcmp(argv[1], "tree") || !strcmp(argv[1], "treecut") || !strcmp(argv[1], "treehit"))) {
        tree_dev = !strcmp(argv[1], "treehit") ? 3 : !strcmp(argv[1], "treecut") ? 2 : 1;
        control_field_str(r, "tree", "queued");
        return;
    }
    if (argc >= 3 && !strcmp(argv[1], "mapmark")) {  // coop mapmark dump | place <No> x y z | del <No> x y z | set 0..2
        int op = !strcmp(argv[2], "dump") ? 1 : !strcmp(argv[2], "place") ? 2 : !strcmp(argv[2], "del") ? 3
               : !strcmp(argv[2], "set") ? 4 : 0;
        if (!op || ((op == 2 || op == 3) && argc != 7) || (op == 4 && argc != 4)) {
            control_fail(r, "Usage: coop mapmark dump | place <No> x y z | del <No> x y z | set 0|1|2");
            return;
        }
        if (argc >= 4) mapmark_dev.no = atoi(argv[3]);
        if (op == 4 && (mapmark_dev.no < 0 || mapmark_dev.no > 2)) { control_fail(r, "set 0|1|2"); return; }
        if (argc == 7) for (int i = 0; i < 3; i++) mapmark_dev.pos[i] = strtof(argv[4 + i], NULL);
        mapmark_dev.op = op;
        mapmark_dev.pending = 1;
        control_field_str(r, "mapmark", "queued");
        return;
    }
    if ((argc == 3 || argc == 5) && !strcmp(argv[1], "mapicon")) {  // coop mapicon <x> <y> <z> | clear | flush (dev)
        mapicon_dev.clear = argc == 3 ? (!strcmp(argv[2], "flush") ? 2 : !strcmp(argv[2], "dump") ? 3 : 1) : 0;
        if (argc == 5) for (int i = 0; i < 3; i++) mapicon_dev.pos[i] = strtof(argv[2 + i], NULL);
        mapicon_dev.pending = 1;
        control_field_str(r, "mapicon", "queued");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "armortick")) {
        puppet.armor_tick = !strcmp(argv[2], "on");
        control_field_str(r, "armortick", puppet.armor_tick ? "on" : "off");
        return;
    }
    if (argc == 4 && !strcmp(argv[1], "thaw")) {  // coop thaw <actor> <job mask>: dev test (actor 0 = none)
        thaw_dev_actor = strtoull(argv[2], NULL, 0);
        thaw_mask = (int)strtol(argv[3], NULL, 0);
        control_field_int(r, "mask", thaw_mask);
        control_field_int(r, "wrapped", thaw_orig[0] != NULL);
        return;
    }
    if (argc >= 3 && argc <= 7 && !strcmp(argv[1], "call")) {  // coop call <fn> [x0..x3]: dev test
        uint64_t fn = strtoull(argv[2], NULL, 0);
        if (fn < 0x7000000000ull || fn >= 0x7003000000ull) { control_fail(r, "Not a main-module address"); return; }
        memset(devcall.x, 0, sizeof devcall.x);
        for (int i = 3; i < argc; i++) devcall.x[i - 3] = strtoull(argv[i], NULL, 0);
        devcall.fn = fn;
        devcall.pending = 1;
        control_field_str(r, "state", "queued (see the log: [coop] call ...)");
        return;
    }
    if (argc == 2 && !strcmp(argv[1], "pa")) {  // coop pa (dev): PlayerInfo and the player actor, for `coop watch`
        uint64_t pinfo = g_site.player_info ? LD64(g_site.player_info) : 0;
        char buf[64];
        snprintf(buf, sizeof buf, "%llx", (unsigned long long)pinfo);
        control_field_str(r, "pinfo", buf);
        snprintf(buf, sizeof buf, "%llx", (unsigned long long)(pinfo ? LD64(pinfo + 0x60) : 0));
        control_field_str(r, "pa", buf);
        return;
    }
    if ((argc == 3 || argc == 4) && !strcmp(argv[1], "watch")) {  // coop watch <addr hex> <len> | off: dev
        if (argc == 3) { watch.len = 0; control_field_str(r, "state", "off"); return; }
        uint64_t a = strtoull(argv[2], NULL, 16);
        uint32_t n = (uint32_t)strtoul(argv[3], NULL, 0) & ~3u;
        if (!in_guest(a) || !n || n > sizeof watch.prev) { control_fail(r, "Bad address or length (max 0x400)"); return; }
        watch.len = 0, watch.addr = a, watch.lines = 0, watch.fresh = 1, watch.len = n;
        control_field_str(r, "state", "on (see the log: [coop] watch ...)");
        return;
    }
    if (argc >= 3 && argc <= 4 && !strcmp(argv[1], "looks")) {  // coop looks tint <n> | show <n|-1> | names <0|1> | tags | mats (dev)
        int v = argc == 4 ? atoi(argv[3]) : 0;
        if (argc == 4 && !strcmp(argv[2], "tint") && v >= 0 && v < COOP_TINT_COUNT) g_settings.coop_tint = v;
        else if (argc == 4 && !strcmp(argv[2], "show") && v >= -1 && v < COOP_TINT_COUNT) looks_dev_tint = v;
        else if (argc == 4 && !strcmp(argv[2], "names")) g_settings.coop_names = v != 0;
        else if (argc == 3 && !strcmp(argv[2], "mats")) looks_mats_until = plat_now_ns() + 300000000ull, looks_mats_seq++;
        else if (!(argc == 3 && !strcmp(argv[2], "tags"))) { control_fail(r, "coop looks tint <0-7> | show <-1..7> | names <0|1> | tags | mats"); return; }
        control_field_int(r, "tag_count", looks_dev_log());
        control_text(r, "looks: colour %d, shown %d, names %d", g_settings.coop_tint, looks_dev_tint, g_settings.coop_names);
        return;
    }
    if (argc == 4 && !strcmp(argv[1], "look") && !strcmp(argv[2], "anim")) {  // coop look anim <n|off> (dev)
        look_dev.anim = strcmp(argv[3], "off") ? atoi(argv[3]) : -1;
        control_field_str(r, "anim", look_dev.anim >= 0 && look_dev.anim < COOP_ANIM_COUNT ? coop_anim_as[look_dev.anim] : "off");
        return;
    }
    if ((argc == 4 || argc == 5) && !strcmp(argv[1], "look") && !strcmp(argv[2], "tree")) {  // coop look tree <AS|off> [ms] (dev)
        uint16_t i = strcmp(argv[3], "off") ? as_name_index(argv[3]) : 0;
        if (strcmp(argv[3], "off") && !i) { control_fail(r, "Not an AS of the player's list"); return; }
        look_tree.until_ns = plat_now_ns() + (uint64_t)(argc == 5 ? atoi(argv[4]) : 1500) * 1000000ull;
        look_tree.seq++;
        look_tree.as = i;
        control_field_str(r, "tree", i ? coop_as_names[i] : "off");
        return;
    }
    if ((argc == 4 || argc == 5) && !strcmp(argv[1], "look") && !strcmp(argv[2], "eat")) {  // coop look eat <1..7> [ms] (dev, coop/eat.inc)
        int k = atoi(argv[3]);
        if (k < 1 || k > COOP_EAT_MAX) { control_fail(r, "coop look eat <1..%d> [ms]", COOP_EAT_MAX); return; }
        eat_dev.until_ns = plat_now_ns() + (uint64_t)(argc == 5 ? atoi(argv[4]) : 2500) * 1000000ull;
        eat_dev.kind = (uint8_t)k;
        eat_dev.go = 1;
        control_field_str(r, "eat", eat_case[k]);
        return;
    }
    if (argc >= 3 && argc <= 5 && !strcmp(argv[1], "padas")) {  // coop padas <AS 0.0|off> [AS 1.0|- [AS 1.1]] (dev)
        uint16_t v[3] = {0, 0, 0};
        for (int k = 0; k < 3 && k + 2 < argc; k++) {
            if (!strcmp(argv[k + 2], "off") || !strcmp(argv[k + 2], "-")) continue;
            v[k] = as_name_index(argv[k + 2]);
            if (!v[k]) { control_fail(r, "Not an AS of the player's list: %s", argv[k + 2]); return; }
        }
        pad_dev.as[1] = v[1], pad_dev.as[2] = v[2], pad_dev.as[0] = v[0];
        control_field_str(r, "padas", v[0] ? coop_as_names[v[0]] : "off");
        return;
    }
    if (argc == 4 && !strcmp(argv[1], "look") && !strcmp(argv[2], "face")) {  // coop look face <n|off> (dev)
        look_dev_face = strcmp(argv[3], "off") ? atoi(argv[3]) : -1;  // (a number, or a face AS name)
        for (int i = 1; i < COOP_FACE_COUNT; i++)
            if (!strcmp(argv[3], coop_face_as[i])) look_dev_face = i;
        control_field_str(r, "face", look_dev_face > 0 && look_dev_face < COOP_FACE_COUNT ? coop_face_as[look_dev_face] : "off");
        return;
    }
    if (argc == 4 && !strcmp(argv[1], "look") && !strcmp(argv[2], "upper")) {  // coop look upper <n|off> (dev)
        look_dev.anim_upper = strcmp(argv[3], "off") ? atoi(argv[3]) : -1;
        control_field_str(r, "upper", look_dev.anim_upper > 0 && look_dev.anim_upper < COOP_ANIM_UPPER_COUNT ? coop_anim_upper_as[look_dev.anim_upper] : "off");
        return;
    }
    if (argc >= 4 && !strcmp(argv[1], "look")) {  // coop look armor|gear <slot> <name|-|off> [bone] [dx dy dz] (dev)
        int slot = atoi(argv[3]);
        const char *name = argc >= 5 ? argv[4] : "off";
        bool off = !strcmp(name, "off"), none = !strcmp(name, "-");
        if (strlen(name) >= 32) { control_fail(r, "Name too long"); return; }
        if (!strcmp(argv[2], "armor") && slot >= 0 && slot < COOP_ARMOR_SLOTS) {
            snprintf(look_dev.armor[slot], sizeof look_dev.armor[slot], "%s", off ? "" : name);
            control_field_str(r, "armor", look_dev.armor[slot]);
            return;
        }
        if (!strcmp(argv[2], "gear") && slot >= 0 && slot < COOP_GEAR_COUNT) {
            CoopGear g;
            memset(&g, 0, sizeof g);
            if (none) g.bone = COOP_BONE_COUNT;
            else if (!off) {
                snprintf(g.name, sizeof g.name, "%s", name);
                g.bone = (uint8_t)(argc >= 6 ? atoi(argv[5]) : COOP_BONE_WEAPON_R);
                if (g.bone < 1 || g.bone >= COOP_BONE_COUNT) { control_fail(r, "Bone 1..%d", COOP_BONE_COUNT - 1); return; }
                g.rel[0] = g.rel[5] = g.rel[10] = 1;
                for (int i = 0; i < 3 && argc >= 9; i++) g.rel[i * 4 + 3] = strtof(argv[6 + i], NULL);
            }
            look_dev.gear[slot] = g;
            control_field_str(r, "gear", g.name);
            return;
        }
        control_fail(r, "Usage: coop look armor <0-2> <name|-|off> | gear <0-3> <name|-|off> [bone 1-6] [dx dy dz]");
        return;
    }
    if (argc == 2 && !strcmp(argv[1], "look")) {  // coop look: what we send and what the avatar shows (to the log)
        look_report = 1;
        control_field_str(r, "look", "logged");
        return;
    }
    if ((argc == 4 || argc == 5) && !strcmp(argv[1], "event")) {  // coop event <flow> <entry> [actor hex] (dev)
        snprintf(event_dev.flow, sizeof event_dev.flow, "%s", argv[2]);
        snprintf(event_dev.entry, sizeof event_dev.entry, "%s", argv[3]);
        event_dev.actor = argc == 5 ? strtoull(argv[4], NULL, 16) : 0;
        event_dev.pending = 1;
        control_field_str(r, "state", "queued (see the log: [coop] event ...)");
        return;
    }
    if (argc == 4 && !strcmp(argv[1], "evanim") && !strcmp(argv[2], "bits")) {  // coop evanim bits <mask> (dev)
        evanim_bits = (uint32_t)strtoul(argv[3], NULL, 0);
        control_field_int(r, "bits", (int)evanim_bits);
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "evanim")) {  // coop evanim on|off (dev: EVANIM)
        evanim_c = !strcmp(argv[2], "on");
        control_field_int(r, "on", evanim_c);
        return;
    }
    if (argc == 2 && !strcmp(argv[1], "evanim")) {  // coop evanim: avatar flags / AS time / job calls, to the log (dev)
        evanim_dev = 1;
        control_field_str(r, "state", "queued (see the log: [coop] evanim dev ...)");
        return;
    }
    if (argc == 2 && !strcmp(argv[1], "evdump")) {  // coop evdump: the running event's name strings (dev)
        evdump_dev = 1;
        control_field_str(r, "state", "queued (see the log: [coop] evdump ...)");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "as")) {  // coop as <actor hex>: its AS and output signals, to the log (dev)
        as_dev_actor = strtoull(argv[2], NULL, 16);
        control_field_str(r, "state", "queued (see the log: [coop] as ...)");
        return;
    }
    if (argc == 2 && !strcmp(argv[1], "horse")) {  // coop horse: what we send and what the horse puppets show, to the log
        horse_report = 1;
        control_field_str(r, "state", "queued (see the log: [coop] horse report ...)");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "horse") && !strcmp(argv[2], "reins")) {  // coop horse reins (dev): toggle the
        horse_reins_dev = !horse_reins_dev;                                       // reins on the avatar's hands, not ridden
        control_field_str(r, "state", horse_reins_dev ? "reins dev on" : "reins dev off");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "horse") && !strcmp(argv[2], "eat")) {  // coop horse eat (dev): the sent horse grazes
        horse_eat_dev = 1;
        control_field_str(r, "state", "queued");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "horse") && !strcmp(argv[2], "track")) {  // coop horse track (dev): send the
        horse_track = 1;                                                          // nearest horse (10 m), not ridden
        control_field_str(r, "state", "queued (see the log: [coop] horse: ...)");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "horse") && !strcmp(argv[2], "coat")) {  // coop horse coat (dev): coat fields and
        horse_coat_dev = 1;                                                      // slot-0 AS of every horse within 60 m
        control_field_str(r, "state", "queued (see the log: [coop] horse coat dev ...)");
        return;
    }
    if (argc == 2 && !strcmp(argv[1], "cycle")) {  // coop cycle (dev): the Master Cycle puppet's wheel / handle bones
        cyc_report = 1;
        control_field_str(r, "state", "queued (see the log: [coop] cycle report ...)");
        return;
    }
    if (argc == 5 && !strcmp(argv[1], "cycle") && !strcmp(argv[2], "fake")) {  // coop cycle fake <m/s> <deg/s> (dev):
        cyc_fake_speed = strtof(argv[3], NULL);                                 // the shown cycle puppet drives a circle
        cyc_fake_turn = strtof(argv[4], NULL);
        if (!isfinite(cyc_fake_speed) || fabsf(cyc_fake_speed) > 30) cyc_fake_speed = 0;
        if (!isfinite(cyc_fake_turn) || fabsf(cyc_fake_turn) > 360) cyc_fake_turn = 0;
        control_field_str(r, "state", cyc_fake_speed != 0 || cyc_fake_turn != 0 ? "cycle fake on" : "cycle fake off");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "mount") && !strcmp(argv[2], "track")) {  // coop mount track (dev): send the nearest
        horse_track = 2;  // sand seal / Sidon / Master Cycle / deer / bear (16 m) as if just ridden (coop-horse2)
        control_field_str(r, "state", "queued (see the log: [coop] horse: ...)");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "aimdev")) {  // coop aimdev <pitch deg|999> (dev): turn the avatar's Spine_1
        aim_dev_pitch = strtof(argv[2], NULL);
        control_field_str(r, "state", aim_dev_pitch > 360 ? "aimdev off" : "aimdev on");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "ragdev")) {  // coop ragdev <seconds> (dev): send our pose as if ragdolling
        double s = strtod(argv[2], NULL);
        rag_dev_until_ns = s > 0 && s < 600 ? plat_now_ns() + (uint64_t)(s * 1e9) : 0;
        control_field_str(r, "state", rag_dev_until_ns ? "ragdev on" : "ragdev off");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "bones")) {  // coop bones <player|avatar> (dev): a few bones, to the log
        bones_dev = !strcmp(argv[2], "avatar") ? 2 : 1;
        control_field_str(r, "state", "queued (see the log: [coop] bones ...)");
        return;
    }
    if (argc == 2 && !strcmp(argv[1], "waterobj")) {  // coop waterobj (dev): find the water state object, to the log
        waterobj_dev = 1;
        control_field_str(r, "state", "queued (see the log: [coop] waterobj ...)");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "grasscut")) {  // coop grasscut <radius> (dev): cut the grass around our Link
        float rad = strtof(argv[2], NULL);
        if (!(rad > 0 && rad <= 20)) { control_fail(r, "radius 0..20"); return; }
        grass_dev_r = rad;
        control_field_str(r, "state", "queued (see the log: [coop] grass: ...)");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "grassrustle")) {  // coop grassrustle <radius> (dev): rustle grass around our Link
        float rad = strtof(argv[2], NULL);
        if (!(rad > 0 && rad <= 20)) { control_fail(r, "radius 0..20"); return; }
        grass_dev_rustle_r = rad;
        control_field_str(r, "state", "queued (see the log: [coop] grass: ...)");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "asin")) {  // coop asin <actor hex|player|avatar>: its AS input table, to the log
        uint64_t pinfo = player_info_ptr();
        asdev.dump = !strcmp(argv[2], "player") ? (pinfo ? LD64(pinfo + 0x60) : 0)
                   : !strcmp(argv[2], "avatar") ? avatar.actor : strtoull(argv[2], NULL, 16);
        control_field_str(r, "state", "queued (see the log: [coop] asin ...)");
        return;
    }
    if (argc == 5 && !strcmp(argv[1], "asw")) {  // coop asw <actor hex|avatar> <kind> <value>: one input write (dev)
        asdev.w_actor = !strcmp(argv[2], "avatar") ? avatar.actor : strtoull(argv[2], NULL, 16);
        asdev.w_kind = atoi(argv[3]);
        snprintf(asdev.w_val, sizeof asdev.w_val, "%s", argv[4]);
        asdev.write = 1;
        control_field_str(r, "state", "queued");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "astrace")) {  // coop astrace <frames>: bone heights per frame, to the log (dev)
        asdev.trace = atoi(argv[2]);
        control_field_str(r, "state", "queued (see the log: [coop] astrace ...)");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "asrec")) {  // coop asrec <frames>: the player's 0.0 / 1.0 layer records per frame (dev)
        asdev.rec = atoi(argv[2]);
        control_field_str(r, "state", "queued (see the log: [coop] asrec ...)");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "asmirror")) {  // coop asmirror on|off: the avatar mimics our own Link (dev)
        asdev.mirror = !strcmp(argv[2], "on") ? 2 : 0;
        control_field_str(r, "asmirror", asdev.mirror ? "on" : "off");
        return;
    }
    if (argc == 2 && !strcmp(argv[1], "equip")) {  // coop equip: dev, to the log
        equip_dev = 1;
        control_field_str(r, "state", "queued (see the log: [coop] equip ...)");
        return;
    }
    if (argc == 4 && !strcmp(argv[1], "target")) {  // coop target <enemy hex> avatar|player|show (dev, combat2)
        target_dev.mode = !strcmp(argv[3], "avatar") ? 1 : !strcmp(argv[3], "player") ? 2 : 0;
        target_dev.actor = strtoull(argv[2], NULL, 16);
        control_field_str(r, "state", "queued (see the log: [coop] target ...)");
        return;
    }
    if ((argc == 4 || argc == 5) && !strcmp(argv[1], "inject")) {  // coop inject <actor hex> <damage> [kind]: dev
        inject_dev.damage = atoi(argv[3]);
        inject_dev.kind = argc == 5 ? atoi(argv[4]) : 15;
        inject_dev.actor = strtoull(argv[2], NULL, 16);
        control_field_str(r, "state", "queued (see the log: [coop] inject ...)");
        return;
    }
    if (argc >= 3 && argc <= 8 && !strcmp(argv[1], "hit")) {  // coop hit <dmg> [level kind flags [dx dz]]: hit our Link (dev)
        hit_dev.dir[0] = argc >= 8 ? strtof(argv[6], NULL) : 0;
        hit_dev.dir[2] = argc >= 8 ? strtof(argv[7], NULL) : 0;
        hit_dev.dir[1] = 0;
        hit_dev.dmg = atoi(argv[2]);
        hit_dev.level = argc >= 4 ? (int)strtol(argv[3], NULL, 0) : 1;
        hit_dev.kind = argc >= 5 ? (int)strtol(argv[4], NULL, 0) : 0x11;
        hit_dev.flags = argc >= 6 ? (int)strtol(argv[5], NULL, 0) : 0x14;
        hit_dev.pending = 1;
        control_field_str(r, "state", "queued (see the log: [coop] pvp: dev hit ...)");
        return;
    }
    if (argc == 4 && !strcmp(argv[1], "foeshot")) {  // coop foeshot <class 0 rock 1 fire 2 ice 3 shock> <dmg> (dev)
        foeshot_dev.cls = atoi(argv[2]) & COOP_SHOT_CLASS;
        foeshot_dev.dmg = atoi(argv[3]);
        foeshot_dev.pending = 1;
        control_field_str(r, "state", "queued (see the log: [coop] shots: ...)");
        return;
    }
    if (argc == 2 && !strcmp(argv[1], "guardfx")) {  // coop guardfx: the guard sparkles in front of our Link (dev)
        guardfx_dev = 1;
        control_field_str(r, "state", "queued (see the log: [coop] guard fx ...)");
        return;
    }
    if ((argc == 4 || argc == 6) && !strcmp(argv[1], "hitwatch")) {  // coop hitwatch <addr> <len> [addr2 len2] (dev)
        hitwatch.on = 0;
        for (int w = 0; w < 2; w++) {
            hitwatch.addr[w] = argc >= 4 + w * 2 ? strtoull(argv[2 + w * 2], NULL, 16) : 0;
            uint32_t n = argc >= 4 + w * 2 ? (uint32_t)strtoul(argv[3 + w * 2], NULL, 0) & ~3u : 0;
            hitwatch.len[w] = n > HITWATCH_MAX ? HITWATCH_MAX : n;
        }
        hitwatch.left = 0, hitwatch.lines = 0, hitwatch.fresh = true, hitwatch.on = 1;
        control_field_str(r, "state", "on (see the log: [coop] hitwatch ...)");
        return;
    }
    if (argc == 5 && !strcmp(argv[1], "push")) {  // coop push <mode 0-3> <m/s> <ms> (dev): the wind push, facing way
        push_dev.mode = atoi(argv[2]) & 3, push_dev.speed = strtof(argv[3], NULL), push_dev.ms = atoi(argv[4]);
        if (!isfinite(push_dev.speed) || push_dev.ms < 1 || push_dev.ms > 5000) { control_fail(r, "coop push <mode> <m/s> <ms>"); return; }
        push_dev.pending = 1;
        control_field_str(r, "state", "queued (see the log: [coop] wind: ...)");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "pvptrace")) {  // coop pvptrace <seconds> (dev): blade vs body per frame
        pvptrace_dev.until_ns = plat_now_ns() + (uint64_t)(atof(argv[2]) * 1e9);
        control_field_str(r, "state", "on (see the log: [coop] pvptrace ...)");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "flinch")) {  // coop flinch <actor hex>: dev test of the damage reaction
        flinch_dev_actor = strtoull(argv[2], NULL, 16);
        control_field_str(r, "state", "queued (see the log: [coop] flinch ...)");
        return;
    }
    if ((argc == 3 || argc == 4) && !strcmp(argv[1], "fnprof")) {  // dev: fnprof start <addr> [span] | stop <file>
        void fnprof_filter_start(uint64_t lo, uint64_t hi);
        void fnprof_filter_stop(const char *out);
        if (!strcmp(argv[2], "stop") && argc == 4) { fnprof_filter_stop(argv[3]); control_field_str(r, "state", "written"); return; }
        if (!strcmp(argv[2], "start") && argc == 4) {
            uint64_t a = strtoull(argv[3], NULL, 16);
            fnprof_filter_start(a, a + 0x3000);  // (the actor object; components further out are missed)
            control_field_str(r, "state", "on");
            return;
        }
    }
    // (dev) coop dishinv log 0|1 (the dish window's state changes in the log) | frame <f> (the Type anim frame used with
    // our extra button)
    if (argc >= 3 && !strcmp(argv[1], "dishinv")) {
        if (argc == 4 && !strcmp(argv[2], "log")) {
            dinv_log_set(atoi(argv[3]));
            control_field_str(r, "state", "ok");
            return;
        }
        if (argc == 4 && !strcmp(argv[2], "frame")) {
            dinv_frames_set((float)atof(argv[3]));
            control_field_str(r, "state", "ok (next window)");
            return;
        }
        // fit <units>: the label width budget; label <text...> | label off: a test label instead of "Dar a <name>"
        if (argc == 4 && !strcmp(argv[2], "fit")) {
            dinv_fit_set(atoi(argv[3]));
            control_field_str(r, "state", "ok (next window)");
            return;
        }
        if (argc >= 4 && !strcmp(argv[2], "label")) {
            char t[96] = "";
            for (int i = 3; i < argc; i++)
                snprintf(t + strlen(t), sizeof t - strlen(t), "%s%s", i > 3 ? " " : "", argv[i]);
            dinv_override_set(argc == 4 && !strcmp(argv[3], "off") ? NULL : t);
            control_field_str(r, "state", "ok (next window)");
            return;
        }
        control_field_str(r, "state", "usage: coop dishinv log 0|1 | frame <f> | fit <units> | label <text>|off");
        return;
    }
    // (dev, DISHINV) coop name <text...>: our player name from now on (as the setting would set it; partners get it with
    // the next name packet). Used by a two-game test for long / accented names.
    if (argc >= 3 && !strcmp(argv[1], "name")) {
        char t[96] = "";
        for (int i = 2; i < argc; i++)
            snprintf(t + strlen(t), sizeof t - strlen(t), "%s%s", i > 2 ? " " : "", argv[i]);
        coop_net_set_name(t);
        char now[COOP_NAME_LEN] = "";
        coop_net_player_name(coop_net_my_id(), now, sizeof now);
        LOG("[coop] dev name: \"%s\"\n", now);
        control_field_str(r, "state", now);
        return;
    }
    // (dev) coop dishmake <Item_Cook_..> <hearts x4> <seconds> <effect id|-1> <level> <price> [ingredient actors (<= 5)]:
    // that dish into our own pouch through the game's cooked-item get (the cooking pot's pickup path)
    if (argc >= 8 && argc <= 12 && !strcmp(argv[1], "dishmake")) {
        if (__atomic_load_n(&dish_dev_make_pending, __ATOMIC_ACQUIRE)) {
            control_field_str(r, "state", "busy");
            return;
        }
        CoopDish d = {0};
        snprintf(d.name, sizeof d.name, "%s", argv[2]);
        d.hearts = atoi(argv[3]), d.seconds = atoi(argv[4]), d.effect = atoi(argv[5]);
        d.level = (float)atof(argv[6]), d.price = atoi(argv[7]);
        for (int i = 8; i < argc; i++) {
            d.ingredient[i - 8] = items_ingredient_id(argv[i]);
            if (!d.ingredient[i - 8]) {
                control_field_str(r, "state", "unknown ingredient");
                return;
            }
        }
        CoopDishOffer check = {.from = 1, .to = 2, .id = 1, .area = 1, .to_pid = 1, .dish = d};
        if (!coop_dish_offer_ok(&check) || !items_dish_name(d.name)) {
            control_field_str(r, "state", "bad dish (name or values)");
            return;
        }
        dish_dev_make = d;
        __atomic_store_n(&dish_dev_make_pending, 1, __ATOMIC_RELEASE);
        control_field_str(r, "state", "queued (see [coop] dishgive in the log)");
        return;
    }
    if (argc >= 2 && argc <= 3 && !strcmp(argv[1], "dish")) {
        char *end = NULL;
        long action = argc == 2 ? -1 : strtol(argv[2], &end, 10);
        if (argc == 3 && (action < 1 || action > COOP_DISH_MAX || *end)) {
            control_field_str(r, "state", "usage: coop dish [1-60]");
            return;
        }
        __atomic_store_n(&dish_dev_action, (int)action, __ATOMIC_RELEASE);
        control_field_str(r, "state", "queued (see [coop] dishgive in the log)");
        return;
    }
    if (argc >= 2 && argc <= 3 && !strcmp(argv[1], "pouch")) {  // pouch counts (names containing argv[2]), to the log
        snprintf(pouch_dump_filter, sizeof pouch_dump_filter, "%s", argc == 3 ? argv[2] : "");
        pouch_dump = 1;
        control_field_str(r, "state", "queued (see the log: [coop] pouch ...)");
        return;
    }
    if (argc == 4 && !strcmp(argv[1], "setflag")) {  // coop setflag <name> 0|1: set a bool game flag (dev, tests)
        snprintf(setflag_dev.name, sizeof setflag_dev.name, "%s", argv[2]);
        setflag_dev.value = atoi(argv[3]) != 0;
        setflag_dev.pending = 1;
        control_field_str(r, "state", "queued");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "door")) {  // coop door <Link door AS> (dev): open the door by our Link as the avatar would
        snprintf(door_dev.as, sizeof door_dev.as, "%s", argv[2]);
        door_dev.pending = 1;
        control_field_str(r, "as", door_dev.as);
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "hinge")) {  // coop hinge 0|1 (dev): hinged-object claim rules off/on (A/B test)
        hinge_dev = atoi(argv[2]) != 0;
        LOG("[coop] objects: hinge rules %s (dev)\n", hinge_dev ? "on" : "off");
        control_field_str(r, "state", hinge_dev ? "on" : "off");
        return;
    }
    if (argc == 2 && !strcmp(argv[1], "fauna")) {  // coop fauna (dev): census of the small animals near our Link
        fauna_census_dev = 1;
        control_field_str(r, "state", "queued (see the log: [coop] fauna census ...)");
        return;
    }
    if (argc == 4 && !strcmp(argv[1], "elemfoe")) {  // coop elemfoe fire|ice|shock <hash> (dev)
        for (unsigned i = 0; i < ELEMFOE_FX_COUNT; i++) {
            if (strcmp(argv[2], elemfoe_fx[i].name)) continue;
            elemfoe_dev.hash = (uint32_t)strtoul(argv[3], NULL, 10);
            elemfoe_dev.kind = i + 1;
            control_field_str(r, "state", "queued (see the log: [coop] elemfoe ...)");
            return;
        }
    }
    if ((argc == 2 || argc == 4) && !strcmp(argv[1], "fire")) {
        int mode = FIRE_LIST;
        if (argc == 4 && !strcmp(argv[2], "held")) {
            if (!strcmp(argv[3], "on")) mode = FIRE_HELD_ON;
            else if (!strcmp(argv[3], "off")) mode = FIRE_HELD_OFF;
            else if (!strcmp(argv[3], "blue")) mode = FIRE_HELD_BLUE;
            else if (!strcmp(argv[3], "status")) mode = FIRE_HELD_STATUS;
            else { control_fail(r, "Usage: coop fire held on|off|blue|status"); return; }
        } else if (argc == 4) {
            mode = !strcmp(argv[2], "on") ? FIRE_ON : FIRE_OFF;
        }
        fire_dev.mode = mode;
        fire_dev.hash = argc == 4 ? (uint32_t)strtoul(argv[3], NULL, 10) : 0;
        fire_dev.pending = 1;
        control_field_str(r, "state", "queued (see the log: [coop] fire ...)");
        return;
    }
    if ((argc == 2 || argc == 3 || argc == 5) && !strcmp(argv[1], "grassfire")) {  // coop grassfire [light [dx dz]] (dev)
        gf_dev.mode = argc >= 3 && !strcmp(argv[2], "light");
        gf_dev.dx = argc == 5 ? (float)atof(argv[3]) : 4.0f, gf_dev.dz = argc == 5 ? (float)atof(argv[4]) : 0.0f;
        gf_dev.pending = 1;
        control_field_str(r, "state", "queued (see the log: [coop] grassfire ...)");
        return;
    }
    if (argc >= 2 && !strcmp(argv[1], "water")) {  // coop water | spin <hash> <rad_s> [secs] | trigger <hash> [0|1] | level <hash> <y>
        wp_probe = true;
        if (argc == 2) {
            wp_dev.list_req = 1;
            control_field_str(r, "state", "queued (see the log: [coop] water ...)");
            return;
        }
        if ((argc == 5 || argc == 6) && !strcmp(argv[2], "spin")) {
            wp_dev.spin_hash = (uint32_t)strtoul(argv[3], NULL, 10);
            wp_dev.spin_rad = strtof(argv[4], NULL);
            wp_dev.spin_secs = argc == 6 ? strtof(argv[5], NULL) : 4.0f;
            if (!isfinite(wp_dev.spin_rad) || fabsf(wp_dev.spin_rad) > 30.0f ||
                !isfinite(wp_dev.spin_secs) || wp_dev.spin_secs <= 0.0f || wp_dev.spin_secs > 30.0f) {
                control_fail(r, "coop water spin <hash> <rad_s -30..30> [secs 0..30]");
                return;
            }
            wp_dev.spin_req = 1;
            control_field_str(r, "state", "queued (see the log: [coop] water ...)");
            return;
        }
        if ((argc == 4 || argc == 5) && !strcmp(argv[2], "trigger")) {
            wp_dev.trig_hash = (uint32_t)strtoul(argv[3], NULL, 10);
            wp_dev.trig_val = argc == 5 ? (atoi(argv[4]) != 0 ? 1 : 0) : -1;
            wp_dev.trig_req = 1;
            control_field_str(r, "state", "queued (see the log: [coop] water ...)");
            return;
        }
        if (argc == 5 && !strcmp(argv[2], "level")) {
            wp_dev.level_hash = (uint32_t)strtoul(argv[3], NULL, 10);
            wp_dev.level_y = strtof(argv[4], NULL);
            if (!isfinite(wp_dev.level_y) || fabsf(wp_dev.level_y) > 10000.0f) {
                control_fail(r, "coop water level <hash> <y>");
                return;
            }
            wp_dev.level_req = 1;
            control_field_str(r, "state", "queued (see the log: [coop] water ...)");
            return;
        }
        control_fail(r, "Usage: coop water | spin <hash> <rad_s> [secs] | trigger <hash> [0|1] | level <hash> <y>");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "critterkill")) {  // coop critterkill <actor hex> (dev): delete it here
        critter_kill_dev = strtoull(argv[2], NULL, 16);
        control_field_str(r, "state", "queued (see the log: [coop] critters ...)");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "bodyfind")) {  // coop bodyfind <actor hex> (dev, RE): rigid bodies to the log
        bodyfind_dev.actor = strtoull(argv[2], NULL, 16);
        bodyfind_dev.pending = 1;
        control_field_str(r, "state", "queued (see the log: [coop] bodyfind ...)");
        return;
    }
    if ((argc == 6 || argc == 7) && !strcmp(argv[1], "rbmove")) {  // coop rbmove <actor> dx dy dz [vel] (dev, RE)
        rbmove_dev.actor = strtoull(argv[2], NULL, 16);
        for (int k = 0; k < 3; k++) rbmove_dev.d[k] = (float)atof(argv[3 + k]);
        rbmove_dev.vel = argc == 7 && !strcmp(argv[6], "vel");
        rbmove_dev.unfix = argc == 7 && !strcmp(argv[6], "unfix");
        rbmove_dev.pending = 1;
        control_field_str(r, "state", "queued");
        return;
    }
    if ((argc == 3 || argc == 4) && !strcmp(argv[1], "coltrace")) {  // coop coltrace <seconds> [nodrive] (dev)
        coltrace_dev.nodrive = argc == 4 && !strcmp(argv[3], "nodrive");
        coltrace_dev.next_ns = 0;
        coltrace_dev.until_ns = plat_now_ns() + (uint64_t)(atof(argv[2]) * 1e9);
        control_field_str(r, "state", "on (see the log: [coop] coltrace ...)");
        return;
    }
    if (argc == 4 && !strcmp(argv[1], "objtrace")) {  // coop objtrace <hash> <seconds> (dev): shared object trace
        objtrace_dev.next_ns = 0;
        objtrace_dev.until_ns = plat_now_ns() + (uint64_t)(atof(argv[3]) * 1e9);
        objtrace_dev.hash = (uint32_t)strtoul(argv[2], NULL, 10);
        control_field_str(r, "state", "on (see the log: [coop] objtrace ...)");
        return;
    }
    if ((argc == 3 || argc == 4) && !strcmp(argv[1], "rbscan")) {  // coop rbscan <filter> [radius m] (dev, RE)
        snprintf(rbscan_dev.filter, sizeof rbscan_dev.filter, "%s", argv[2]);
        rbscan_dev.radius = argc == 4 ? (float)atof(argv[3]) : 200.0f;
        rbscan_dev.pending = 1;
        control_field_str(r, "state", "queued (see the log: [coop] rbscan ...)");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "shotprobe")) {  // coop shotprobe <s> (dev, RE: shots.inc)
        shotprobe_until_ns = plat_now_ns() + (uint64_t)(atof(argv[2]) * 1e9);
        control_field_str(r, "state", "on (see the log: [coop] shotprobe ...)");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "shottrace")) {  // coop shottrace <s> (dev: enemy shots, shots.inc)
        shottrace_until_ns = plat_now_ns() + (uint64_t)(atof(argv[2]) * 1e9);
        control_field_str(r, "state", "on (see the log: [coop] shots / shottrace ...)");
        return;
    }
    if ((argc == 3 || argc == 4) && !strcmp(argv[1], "arrowtrace")) {  // coop arrowtrace <s> [filter] (dev, RE)
        snprintf(arrowtrace_dev.filter, sizeof arrowtrace_dev.filter, "%s", argc == 4 ? argv[3] : "Arrow");
        arrowtrace_dev.n = 0, arrowtrace_dev.frame = 0;
        arrowtrace_dev.until_ns = plat_now_ns() + (uint64_t)(atof(argv[2]) * 1e9);
        control_field_str(r, "state", "on (see the log: [coop] arrowtrace ...)");
        return;
    }
    if ((argc == 4 || argc == 5) && !strcmp(argv[1], "swhit")) {  // coop swhit <actor hex> <mode> [frames] (dev)
        swhit_dev.actor = strtoull(argv[2], NULL, 16);
        sw_hit_mode = atoi(argv[3]);
        swhit_dev.frames = argc == 5 ? atoi(argv[4]) : 1;
        control_field_str(r, "state", "queued (see the log: [coop] swhit ...)");
        return;
    }
    if ((argc == 2 || argc == 5) && !strcmp(argv[1], "switch")) {  // coop switch [hold <hash> <s>] (dev)
        switch_dev.hash = argc == 5 && !strcmp(argv[2], "hold") ? (uint32_t)strtoul(argv[3], NULL, 10) : 0;
        switch_dev.secs = argc == 5 ? (float)atof(argv[4]) : 0;
        switch_dev.pending = 1;
        control_field_str(r, "state", "queued (see the log: [coop] switch ...)");
        return;
    }
    if ((argc == 3 || argc == 4) && !strcmp(argv[1], "aiobj")) {  // coop aiobj <actor hex> [f32] (dev, RE)
        uint64_t a = strtoull(argv[2], NULL, 16);
        int found = 0;
        if (in_guest(a) && in_guest(a + 0x1000)) aiobj_walk(a, 0, a, argc == 4 ? (float)atof(argv[3]) : 1e30f, &found);
        LOG("[coop] aiobj done (%d)\n", found);
        control_field_str(r, "state", "see the log: [coop] aiobj ...");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "bodies")) {  // coop bodies <actor hex>: paths to rigid bodies (dev, RE)
        bodies_report(strtoull(argv[2], NULL, 16));
        control_field_str(r, "state", "see the log: [coop] bodies ...");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "flag")) {  // coop flag <name>: a bool game flag's value, to the log (dev)
        flag_report(argv[2]);
        control_field_str(r, "state", "see the log: [coop] flag ...");
        return;
    }
    if (argc == 2 && !strcmp(argv[1], "runes")) {  // coop runes: Stasis manager + Cryonis blocks/copies, to the log (dev)
        ice_report = 1;
        control_field_str(r, "state", "queued (see the log: [coop] stasis / cryonis ...)");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "stasisfoe")) {
        stasisfoe_dev_hash = (uint32_t)strtoul(argv[2], NULL, 0);
        control_field_str(r, "state", "queued native lock on enemy");
        return;
    }
    if ((argc == 3 || argc == 4) && !strcmp(argv[1], "stasisfake")) {  // coop stasisfake <hash> [s] (dev)
        stasis_fake.until_ns = plat_now_ns() + (uint64_t)((argc == 4 ? atof(argv[3]) : 8.0) * 1e9);
        stasis_fake.hash = (uint32_t)strtoul(argv[2], NULL, 0);
        control_field_str(r, "state", "queued (see the log: [coop] stasis ...)");
        return;
    }
    if (argc == 2 && !strcmp(argv[1], "icemake")) {  // coop icemake (dev): a Cryonis block 3 m north of our Link
        ice_dev_make = 1;
        control_field_str(r, "state", "queued (see the log: [coop] cryonis ...)");
        return;
    }
    if (argc == 2 && !strcmp(argv[1], "icecopy")) {  // coop icecopy (dev, n5): a copy as if the other player made a block 3 m north
        ice_dev_copy = 1;
        control_field_str(r, "state", "queued (see the log: [coop] cryonis ...)");
        return;
    }
    if (argc == 2 && !strcmp(argv[1], "icebreak")) {  // coop icebreak (dev): break our Cryonis blocks
        ice_dev_break = 1;
        control_field_str(r, "state", "queued (see the log: [coop] cryonis ...)");
        return;
    }
    if (argc == 2 && !strcmp(argv[1], "gyro")) {  // coop gyro: the motion-control platforms here, to the log (dev)
        gyro_report = 1;
        control_field_str(r, "state", "queued (see the log: [coop] gyro ...)");
        return;
    }
    if (argc == 4 && !strcmp(argv[1], "gyroset")) {  // coop gyroset <x deg> <z deg> (dev): as if the other player tilted
        float ax = (float)atof(argv[2]) * 0.0174533f, az = (float)atof(argv[3]) * 0.0174533f;
        float cx = cosf(ax), sx = sinf(ax), cz = cosf(az), sz = sinf(az);
        float m[9] = {cz, -sz * cx, sz * sx, sz, cz * cx, -cz * sx, 0, sx, cx};  // Rz * Rx, rows
        memcpy(gyro_dev.r, m, sizeof m);
        gyro_dev.until_ns = plat_now_ns() + 30000000000ull;
        gyro_dev.pending = 1;
        control_field_str(r, "state", "queued for 30 s (see the log: [coop] gyro ...)");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "rotset")) {  // coop rotset <radians> (dev): as if the other player turned the parts
        float a = (float)atof(argv[2]);
        for (int i = 0; i < brot_n; i++) brot[i].mine = false, brot[i].moving = false, brot[i].remote = a, brot[i].have_remote = true;
        control_field_str(r, "state", brot_n ? "set (see the log: [coop] beast ...)" : "no moving parts here");
        return;
    }
    if ((argc == 3 || argc == 4) && !strcmp(argv[1], "watchai")) {  // coop watchai <actor hex> [seconds] (dev, RE; "watch" is the older raw-memory one)
        watch_dev.actor = strtoull(argv[2], NULL, 16);
        watch_dev.until_ns = plat_now_ns() + (uint64_t)((argc == 4 ? atof(argv[3]) : 20.0) * 1e9);
        watch_dev.pending = 1;
        control_field_str(r, "state", "watching (see the log: [coop] watch ...)");
        return;
    }
    if ((argc == 3 || argc == 4) && !strcmp(argv[1], "s32")) {  // coop s32 <name> [value]: dump / set an s32 game flag (dev)
        snprintf(s32_dev.name, sizeof s32_dev.name, "%s", argv[2]);
        s32_dev.set = argc == 4;
        s32_dev.value = argc == 4 ? atoi(argv[3]) : 0;
        s32_dev.pending = 1;
        control_field_str(r, "state", "queued (see the log: [coop] s32 ...)");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "giveback")) {  // (dev) drops.inc race: give one <name> back from our pouch
        snprintf(drop_giveback_dev, sizeof drop_giveback_dev, "%s", argv[2]);
        control_field_str(r, "state", "queued (see the log: [coop] drops: ... gave our ... back)");
        return;
    }
    if (argc == 2 && !strcmp(argv[1], "drops")) {  // the shared dropped items (drops.inc), to the log
        drops_dev = 1;
        control_field_str(r, "state", "queued (see the log: [coop] drops: ...)");
        return;
    }
    if (argc == 2 && !strcmp(argv[1], "enemies")) {  // the shared enemies' table, to the log
        enemy_report = 1;
        control_field_str(r, "state", "queued (see the log: [coop] enemy ...)");
        return;
    }
    if (argc == 2 && !strcmp(argv[1], "npc")) {  // (npc.inc) the shared townspeople, to the log
        npc_report = 1;
        control_field_str(r, "state", "queued (see the log: [coop] npcpos ... / npc: N tracked)");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "enemylook")) {  // coop enemylook 0|1 (dev): ENEMYLOOK off / on
        enemylook_mode = atoi(argv[2]) ? 1 : 0;
        control_field_str(r, "enemylook", enemylook_mode ? "on" : "off");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "evscan")) {  // coop evscan <text>: names in the running event's memory (dev)
        snprintf(evscan_dev.pat, sizeof evscan_dev.pat, "%s", argv[2]);
        evscan_dev.pending = 1;
        control_field_str(r, "state", "queued (see the log: [coop] evscan ...)");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "storybusy")) {  // coop storybusy <s> (dev, FAKE): act as if in an event (talking)
        double s = strtod(argv[2], NULL);
        story_busy_until = s > 0 && s < 600 ? plat_now_ns() + (uint64_t)(s * 1e9) : 0;
        control_field_str(r, "state", "busy (a cutscene of the other player waits until then)");
        return;
    }
    if (argc == 2 && !strcmp(argv[1], "aimdiag")) {  // coop aimdiag (dev): the aim / charge diagnostic on (= AIMDIAG=1)
        aim_diag_dev = 1;
        control_field_str(r, "state", "on (see the log: [coop] aimdiag / chargediag)");
        return;
    }
    // coop champ probe | native urbosa | fake <shield|count N|range|ready|urbosa|mipha|revali|break N|off> [ms] (dev,
    // coop/champ.inc): our Champion counters logged; the game's own Urbosa's Fury call; a FAKE ability effect sent
    if (argc >= 3 && !strcmp(argv[1], "champ")) {
        if (!strcmp(argv[2], "probe")) { champ_dev.probe = 1; control_field_str(r, "state", "queued (see the log: [coop] champ: probe)"); return; }
        if (argc == 4 && !strcmp(argv[2], "native") && !strcmp(argv[3], "urbosa")) {
            champ_dev.native = 1;
            control_field_str(r, "state", "queued (see the log: [coop] champ: dev)");
            return;
        }
        if (argc >= 4 && !strcmp(argv[2], "fake")) {
            const char *w = argv[3];
            int lv = argc >= 5 ? atoi(argv[4]) : 0, ms = argc >= 6 ? atoi(argv[5]) : argc == 5 ? atoi(argv[4]) : 3000;
            if (lv < 1 || lv > 3) lv = 3;
            if (ms < 100 || ms > 60000) ms = 3000;
            uint16_t held = !strcmp(w, "shield") ? COOP_CHAMP_SHIELD | COOP_CHAMP_COUNT | (uint16_t)(3u << COOP_CHAMP_COUNT_SHIFT) :
                            !strcmp(w, "count") ? COOP_CHAMP_COUNT | (uint16_t)(lv << COOP_CHAMP_COUNT_SHIFT) :
                            !strcmp(w, "range") ? COOP_CHAMP_RANGE : !strcmp(w, "ready") ? COOP_CHAMP_READY : 0;
            int use = !strcmp(w, "urbosa") ? COOP_CHAMP_URBOSA + 1 : !strcmp(w, "mipha") ? COOP_CHAMP_MIPHA + 1 :
                      !strcmp(w, "revali") ? COOP_CHAMP_REVALI + 1 : !strcmp(w, "break") ? COOP_CHAMP_BREAK + 1 : 0;
            if (held) champ_dev.held = held, champ_dev.until_ns = plat_now_ns() + (uint64_t)ms * 1000000ull;
            else if (use) champ_dev.lv = (uint8_t)lv, champ_dev.use = use;
            else if (!strcmp(w, "off")) champ_dev.held = 0, champ_dev.until_ns = 0;
            else { control_fail(r, "coop champ fake <shield|count N|range|ready|urbosa|mipha|revali|break N|off> [ms]"); return; }
            control_field_str(r, "state", "FAKE sent (see the log: [coop] champ:)");
            return;
        }
        control_fail(r, "coop champ probe | native urbosa | fake <what> [ms]");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "fx")) {  // coop fx <key>|props (dev): fire an effect key on the avatar / log properties
        if (strlen(argv[2]) >= sizeof fx_dev.key) { control_fail(r, "Name too long"); return; }
        snprintf(fx_dev.key, sizeof fx_dev.key, "%s", argv[2]);
        fx_dev.pending = 1;
        control_field_str(r, "state", "queued (see the log: [coop] fx ... / xprobe ...)");
        return;
    }
    // coop snd <key> | both <key> | props | step | swing <key> (dev, coop/avatarsnd.inc): a sound key on the avatar (both:
    // effect + sound sets), the sound properties of the avatar / player / step helper / weapon puppet, a footstep, a
    // key on the weapon puppet
    if (argc == 4 && !strcmp(argv[1], "snd") && !strcmp(argv[2], "mute")) {  // coop snd mute <mask> (dev)
        snd_mute_dev = (unsigned)strtoul(argv[3], NULL, 0), snd_mute_seq++;
        control_field_str(r, "state", "set (see the log: [coop] snd: dev mute ...)");
        return;
    }
    if ((argc == 3 || argc == 4) && !strcmp(argv[1], "snd")) {
        const char *key = argc == 4 ? argv[3] : argv[2];
        if (strlen(key) >= sizeof snd_dev.key) { control_fail(r, "Name too long"); return; }
        snd_dev.what = argc == 3 ? (!strcmp(argv[2], "props") ? SND_DEV_PROPS : !strcmp(argv[2], "step") ? SND_DEV_STEP : SND_DEV_KEY)
                     : !strcmp(argv[2], "both") ? SND_DEV_BOTH : !strcmp(argv[2], "swing") ? SND_DEV_SWING : -1;
        if (snd_dev.what < 0) { control_fail(r, "coop snd <key> | both <key> | props | step | swing <key>"); return; }
        snprintf(snd_dev.key, sizeof snd_dev.key, "%s", key);
        snd_dev.pending = 1;
        control_field_str(r, "state", "queued (see the log: [coop] snd ...)");
        return;
    }
    if (argc == 2 && !strcmp(argv[1], "players")) {  // coop players (coop-multi): ids, partner, extra avatars
        multi_report(r);
        return;
    }
    if (argc == 2 && !strcmp(argv[1], "heap")) {  // coop heap: the actor heap's free memory, to the log
        heap_dev = 1;
        control_field_str(r, "state", "queued (see the log: [coop] heap ...)");
        return;
    }
    if (argc >= 2 && argc <= 4 && !strcmp(argv[1], "actors")) {  // coop actors [filter|*] [radius m]: to the log
        snprintf(actlist.filter, sizeof actlist.filter, "%s", argc >= 3 && strcmp(argv[2], "*") ? argv[2] : "");
        actlist.radius = argc == 4 ? strtof(argv[3], NULL) : 100.0f;
        actlist.pending = 1;
        control_field_str(r, "state", "queued (see the log: [coop] actor ...)");
        return;
    }
    if ((argc == 2 || argc == 3) && !strcmp(argv[1], "pmprofile")) {
        // Test: make an actor's profile read "PauseMenuPlayer" (the armor code accepts such a wearer,
        // FUN_7101173368). Profile SafeString = *(actor+0x570) + 0x68; its text pointer is at +8.
        uint64_t a = argc == 3 ? strtoull(argv[2], NULL, 0) : (avatar.actor ? avatar.actor : puppet.armor_parent);
        uint64_t param = a ? LD64(a + 0x570) : 0;
        if (!param || !code_matches()) { control_fail(r, "No actor"); return; }
        ST64(param + 0x70, 0x700165ae9bull);  // the game's own "PauseMenuPlayer" string
        control_field_str(r, "profile", (const char *)(uintptr_t)LD64(param + 0x70));
        return;
    }
    if (argc == 2 && !strcmp(argv[1], "outfit")) {
        if (!avatar.actor && !req.actor) { control_fail(r, "Nothing to dress (spawn an actor first)"); return; }
        puppet.outfit = 1;
        control_field_str(r, "state", "queued");
        return;
    }
    if (argc == 2 && !strcmp(argv[1], "refuse")) {  // coop refuse (dev): one refused creation -> the refusal wait
        refuse_dev = 1;
        control_field_str(r, "state", "queued (see the log: [coop] refuse / spawn hold)");
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "brkdel")) {  // coop brkdel <hash> (dev): delete a break-only thing here
        brk_del_dev = (uint32_t)strtoul(argv[2], NULL, 10);
        control_field_str(r, "state", "queued (see the log: [coop] break: dev delete)");
        return;
    }
    if (argc == 2 && !strcmp(argv[1], "despawn")) {
        if (!avatar.actor && !req.actor) { control_fail(r, "Nothing to despawn (spawn an actor first)"); return; }
        puppet.despawn = 1;
        control_field_str(r, "state", "queued");
        return;
    }
    if (argc >= 2 && !strcmp(argv[1], "status")) {
        static const char *names[] = {"idle", "queued", "done", "failed"};
        control_field_str(r, "state", names[req.state]);
        control_field_str(r, "actor", req.name);
        control_field_int(r, "requests", req.count);
        char a[32]; snprintf(a, sizeof a, "0x%llx", (unsigned long long)req.link);
        control_field_str(r, "link", a);
        snprintf(a, sizeof a, "0x%llx", (unsigned long long)req.actor);
        control_field_str(r, "actor_ptr", a);
        snprintf(a, sizeof a, "0x%llx", (unsigned long long)avatar.actor);
        control_field_str(r, "avatar", a);
        control_field_int(r, "avatar_frames", avatar.frames);
        control_field_int(r, "avatar_area", (int)avatar.area);
        if (req.state == COOP_FAILED) control_field_str(r, "why", req.why);
        control_field_int(r, "puppet_mode", puppet.mode);
        control_field_int(r, "puppet_frames", puppet.frames);
        control_field_int(r, "spawn_hold", shold.held);
        control_field_int(r, "spawn_refusal_wait", shold.failhold);
        control_field_int(r, "spawn_hold_demo_as", shold.demo_as);
        control_field_str(r, "local_as", shold_local_as);
        return;
    }
    bool preload = argc >= 2 && !strcmp(argv[1], "preload");
    bool sync = argc >= 2 && !strcmp(argv[1], "spawnsync");
    if (argc < 3 || (strcmp(argv[1], "spawn") && !preload && !sync) || (argc != 3 && argc != 6)) {
        control_fail(r, "%s", usage);
        return;
    }
    if (req.state == COOP_QUEUED) { control_fail(r, "A request is still waiting for the game thread"); return; }
    if (strlen(argv[2]) >= sizeof req.name) { control_fail(r, "Actor name too long"); return; }
    float p[3];
    if (argc == 6) {
        for (int i = 0; i < 3; i++) {
            char *end;
            p[i] = strtof(argv[3 + i], &end);
            if (*end) { control_fail(r, "Cannot read \"%s\"", argv[3 + i]); return; }
        }
    } else if (!player_pos(p)) { control_fail(r, "No world loaded"); return; }
    snprintf(req.name, sizeof req.name, "%s", argv[2]);
    memcpy(req.pos, p, sizeof p);
    req.has_pos = true;
    req.preload = preload;
    req.sync = sync;
    req.why[0] = 0;
    __atomic_store_n(&req.state, COOP_QUEUED, __ATOMIC_RELEASE);
    control_field_str(r, "state", "queued");
    control_field_num(r, "x", p[0]);
    control_field_num(r, "y", p[1]);
    control_field_num(r, "z", p[2]);
}

void control_coop_init(void) {
    static const ControlCmd dev[] = {
        {"coop", "spawn|spawnsync|preload <actor> [x y z] | outfit | armor <name> | despawn | puppet ... | status", "Co-op test: create, dress or delete an actor", cmd_coop, 0},
    };
    control_register(dev, 1);
}

RT_ON_RESET(coop_reset) {
    memset(&req, 0, sizeof req);
    memset(&puppet, 0, sizeof puppet);
    if (multi_sampled) multi_restore(&multi_pristine);
    for (int p = 0; p < COOP_MAX_PLAYERS; p++) {
        free(multi_ctx[p]);
        multi_ctx[p] = NULL;
    }
    multi_wanted = 0;
    nodepos.last = -1, nodepos.last_ns = 0;
    memset(&last_local, 0, sizeof last_local);
    magbeam_local_active = false;
    static int resetclean_c = -1;
    bool clean_new = env_on("SAVAGEWIND_COOP_RESETCLEAN", &resetclean_c);
    anim_mem = 0;
    outfit_out = 0;
    create_mem = 0;
    bone_mem = 0;
    if (clean_new) dye_str_mem = dye_load_mem = 0;
    hurt_color_mem = 0;
    mat_uniform_mem = 0;
    looks_reset();
    fx_key_mem = 0;
    wfx_windows = 0;
    grass_mem = 0;
    tree_hit_mem = 0;
    tree_n = tree_hit_n = tree_dev = 0;
    memset(stumps_seen, 0, sizeof stumps_seen);
    stumps_next = 0;
    rb_mem = 0;
    actlist_mem = 0;
    flinch_mem = 0;
    tgt_link_mem = 0;
    brot_mem = 0;
    mapicon_mem = 0;
    mapmark_mem = 0;
    guardfx_mem = 0;
    evt_mem = 0;
    pickup_win_mem = 0;
    getdemo_dlg_mem = 0;
    world_mem = 0;
    world_reset();
    gate_n = 0;
    shrine_boarded.hash = 0;
    shrine_follow_ns = 0;
    shrine_riding = false;
    gyro_mtx = 0, gyro_n = 0, gyro_area = 0;
    gyro_hk_clear();
    waterpuz_reset();
    stasis_out = stasis_msg_mem = stasis_shown = stasisfx_scratch = 0;
    memset(&stasisfx_local, 0, sizeof stasisfx_local);
    memset(&stasisfoe, 0, sizeof stasisfoe);
    __atomic_store_n(&stasisfoe_job_id, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&stasisfoe_aim_id, 0, __ATOMIC_RELAXED);
    stasisfoe_aim_grace = 0;
    stasis_request_ns = 0;
    stasisfoe_dev_hash = 0;
    memset(&stasis_cd, 0, sizeof stasis_cd);
    ice_mine_n = ice_copy_n = 0, ice_area = 0;
    if (clean_new) ice_gone_n = 0;
    arrow_fly_reset();
}
