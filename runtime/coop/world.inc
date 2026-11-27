// Part of runtime/coop.c (included there, same translation unit): time of day; shared chests, items, progress and quest flags.
// ---- time of day (2026-11-08): the host's clock is the shared one. Both send their hour; the guest moves its clock
// to the host's when they are more than TIME_DRIFT hours apart (circularly), checked every 2 s. Both clocks run at
// the same speed, so this fires at the start and when the host's time jumps (sleeping at a fire, a cutscene).
// The guest cannot move time on its own (its clock goes back to the host's).
// SAVAGEWIND_COOP_TIME=0 turns this off.
float test_warp_hour(bool *held);       // runtime/services/test_warp.c
void test_warp_set_hour(float hour);
#define TIME_DRIFT 0.25f
static void time_frame(void) {
    static uint64_t next_ns;
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_TIME") && !strcmp(getenv("SAVAGEWIND_COOP_TIME"), "0"));
    float hour = test_warp_hour(NULL);
    coop_net_set_hour(local_in_world() ? hour : -1.0f);
    uint64_t now = plat_now_ns();
    if (!on || coop_net_is_host() || now < next_ns || hour < 0 || !local_in_world()) return;
    next_ns = now + 2000000000ull;
    float theirs;
    double age;
    if (!coop_net_remote_hour(&theirs, &age) || age > 2.0) return;
    float d = fabsf(theirs - hour);
    if (d > 12.0f) d = 24.0f - d;
    if (d <= TIME_DRIFT) return;
    test_warp_set_hour(theirs);
    LOG("[coop] time: %.2f -> %.2f (the host's clock)\n", hour, theirs);
}

// ---- shared world progress and items (2026-11-08, protocol 12; rule: items are shared with a player who was
// close when they were got). The game records a chest opened / a placed item or weapon picked up as a bool game flag
// "<MainField|DungeonNNN>_<actor name>_<HashId>", and shrine/tower/Divine Beast progress as named flags
// (Open_/Clear_/CompleteTreasure_DungeonNNN, MapTower_NN, Clear_Remains*). We diff all bool flags 4 times a
// second; a flag that turns on and is one of those (names known from the placed actors around us, whose flag name
// we build and hash, and a fixed list) opens an "acquisition": the items that reach the pouch in the next seconds
// came with it. Then one reliable event names the flag (and where our player is), and one more per item.
// The other side, if its player is within SHARE_RADIUS of that spot in the same area, sets the flag (a picked item
// vanishes there too: its actor is deleted), and gets the same items. Farther away: nothing; their copy stays.
// Enemy loot is not shared this way (no flag): each side already gets its own from a shared kill.
// SAVAGEWIND_COOP_SHARE=0 turns this off; SAVAGEWIND_COOP_SHARE_RADIUS=<m> (default 40).
// Game side: gdt manager *(0x7001e3b188), read-buffer TriggerParam *(mgr+0xc00),
// its bool flags {u32 count, u32 cap, Flag **} at +0x08, a flag's value bit 0 of +0xb and name crc32 at +0xc;
// setBool helper 0x7000d39ba0(bool, SafeString *name, 0). Pouch: *(0x7001e18570), item list (OffsetList) at +0x68,
// item = node - 8, +0x18 type, +0x20 value, name char* +0x30; add 0x7000e81250(SafeString *name, int count).
#define A_GDT_MGR 0x7001e3b188ull
#define A_GDT_SET_BOOL 0x7000d39ba0ull
#define A_POUCH_MGR 0x7001e18570ull
#define A_POUCH_ADD 0x7000e81250ull
#define EV_WORLD 1
#define EV_ITEM 2
#define FLAG_MAX 65536
#define NAMED_MAX 2048
#define POUCH_MAX 512
#define ACQ_ITEMS 8
#define ACQ_WAIT_NS 6000000000ull    // items may come seconds after the flag (a chest's opening scene)
#define ACQ_SETTLE_NS 1000000000ull  // after the last item: send
typedef struct { float pos[3]; uint32_t hashid; char flag[96]; uint32_t area; } EvWorld;  // area: local_area() crc
typedef struct { int32_t count; char name[64]; } EvItem;
_Static_assert(sizeof(EvWorld) <= COOP_EVENT_DATA && sizeof(EvItem) <= COOP_EVENT_DATA, "event sizes");
static struct Named { uint32_t crc, hashid; uint64_t seen_ns; char name[96]; } named[NAMED_MAX];
static int named_n;
static uint8_t flag_val[FLAG_MAX];
static int flag_n;  // 0: no baseline (a load, an area change)
static struct PouchCount { char name[64]; int32_t count; } pouch_prev[POUCH_MAX];
static int pouch_prev_n = -1;  // -1: no baseline
static struct {
    bool open;
    uint64_t opened_ns, last_item_ns;
    EvWorld w;
    EvItem items[ACQ_ITEMS];
    int n;
    bool early;  // (gate.inc chest_mine: our chest's items, sent as soon as they reached the pouch)
} acq;
// (gate.inc chest_mine) the chest flag whose items went out at the pick-up: its own turning on later sends nothing
static uint32_t chest_early_sent_crc;
static uint64_t chest_early_sent_ns;
static struct { uint32_t crc; uint64_t at_ns; } applied[64];  // flags we set for the peer (not ours to send back)
static int applied_next;
static bool peer_world_taken;  // the last EV_WORLD from the peer was taken here (its EV_ITEMs follow it)
static uint64_t world_mem;     // guest scratch: SafeString + text (reset on reboot)

static bool share_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_SHARE") && !strcmp(getenv("SAVAGEWIND_COOP_SHARE"), "0"));
    return on;
}
static float share_radius(void) {
    static float r = -1;
    if (r < 0) {
        const char *s = getenv("SAVAGEWIND_COOP_SHARE_RADIUS");
        r = s && atof(s) > 0 ? (float)atof(s) : 40.0f;
    }
    return r;
}
static uint32_t crc_of(const char *s) { return (uint32_t)crc32(0, (const Bytef *)s, (uInt)strlen(s)); }

static void named_add(const char *name, uint32_t hashid, uint64_t now) {
    uint32_t crc = crc_of(name);
    int k = 0;
    while (k < named_n && named[k].crc != crc) k++;
    if (k == named_n) {
        if (named_n < NAMED_MAX) named_n++;
        else {  // full: replace the longest unseen
            k = 0;
            for (int i = 1; i < named_n; i++) if (named[i].seen_ns < named[k].seen_ns) k = i;
        }
        named[k].crc = crc, named[k].hashid = hashid;
        snprintf(named[k].name, sizeof named[k].name, "%s", name);
    }
    named[k].seen_ns = now;
}

static bool beast_on(void);  // (beast.inc)
static bool quests_on(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("SAVAGEWIND_COOP_QUESTS") && !strcmp(getenv("SAVAGEWIND_COOP_QUESTS"), "0"));
    return on;
}

// The fixed progress flags: shrines, towers, Divine Beasts, and quests (seen_ns UINT64_MAX: never replaced).
static void named_story(void) {
    static const char *const kinds[] = {"Open_Dungeon", "Clear_Dungeon", "CompleteTreasure_Dungeon"};
    static const char *const beasts[] = {"Clear_RemainsWind", "Clear_RemainsWater", "Clear_RemainsFire",
                                         "Clear_RemainsElectric"};
    static const char *const quest_deps[] = {
        "AncientLabo_AncientAssistant001_SSLv2Get", "BloodyMoonReliefFirstTime", "Boko_Away", "CourageFirstDemoKill",
        "Demo004_0", "Demo004_0_1st", "Demo164_0", "Demo166_0", "Dungeon_Clear_1", "Dungeon_Clear_4",
        "Enemy_Dragon_Electric_DoorOpen", "Enemy_Dragon_Fire_DoorOpen", "Enemy_Dragon_Ice_DoorOpen",
        "Enemy_Dragon_Ice_DropItem", "Guide_Scope", "IsGet_Obj_Camera", "IsGet_Obj_SheikSensor", "IsGet_PlayerStole2",
        "IsGet_PlayerStole2_Vagrant", "IsPlayed_Demo004_0", "IsPlayed_Demo104_0", "IsPlayed_Demo164_0",
        "IsPlayed_Demo166_0", "IsPlayed_Demo318_0", "IsStart_Obj_SheikSensor", "Kakariko001_GaveChampionShirt",
        "MapTower_02_Info", "MapTower_07_Demo", "MapTower_DemoFirst", "MiniMapSensor_Demo", "NPC_Goron017_Golem",
        "NPC_Goron023_Go", "Npc_HatenoSee00_Get",
        "Npc_King_Parasail002_Appear", "Npc_King_Parasail004_Appear", "Npc_Zora005_Go", "Npc_oasis050_Go",
        "Open_StartPoint", "PowerFirstDemoKill", "RemainsElectric_2nd", "RevivalGreatFairy3",
        "Rito_NPC010p_Go_UMiiVillage", "Water_Relic_ArmorSend", "WisdomFirstDemoKill",
    };
    static const char *const quest_steps[] = {
        "100enemy:Ready,Activated,Finish,ToDeku,Active,Active2,Active3", "Animal_Forest:Ready,Activated,Finish",
        "BalladOfHeroGerudo:Ready,Activated,Finish,Seek1stDungeon,Seek2ndDungeon,Seek3rdDungeon,ToRemains",
        "BalladOfHeroGoron:Ready,Activated,Finish,Seek1stDungeon,Seek3rdDungeon,Seek2ndDungeon,ToRemains",
        "BalladOfHeroRito:Ready,Activated,Finish,Seek1stDungeon,Seek2ndDungeon,Seek3rdDungeon,ToRemains",
        "BalladOfHeroZora:Ready,Activated,Finish,Seek1stDungeon,Seek2ndDungeon,Seek3rdDungeon,ToRemains",
        "BalladOfHeroes:Ready,Activated,Finish,Step01,Retire,Retire_Dungeon04,Step02,Step02_Dungeon01,Step02_Dungeon02,Step02_Dungeon03,Step2_Dungeon4,Step03,Step04,Step05,Step06",
        "BloodyMoonRelief:Ready,Activated,Finish", "Bottle_Mes:Ready,Activated,Finish,Step10,Step30",
        "Carnivorous_Boy:Ready,Activated,Finish",
        "CarryingBlueFireEXMini:Ready,Activated,Finish,Permit,Carry,Fired,Camera,Repaired",
        "Cliff_Maze:Ready,Activated,Finish", "CompleteDungeon:Ready,Activated,Finish",
        "CursedStatue:Ready,Activated,Finish,Game", "DarkWoods:Ready,Activated,Finish",
        "Desert_Maze:Ready,Activated,Finish", "DokuroEye:Ready,Activated,Finish",
        "DontDamageFlower:Ready,Activated,Finish", "Drag_Hero:Ready,Activated,Finish,Step1",
        "Electric_Relic:Ready,Activated,Finished,Defeat,GetBack,Search,Arrival,Deliver,Strategy,Invasion",
        "FairyFountain:Ready,Activated,Finish", "FindDungeon:Ready,Activated,Finish,1stClear,AllClear",
        "Find_4Relic:Ready,Activated,Finish,1stClear,2ndClear,3rdClear,4thClear", "Find_Impa:Ready,Activated,Finish",
        "Fire_Relic:Ready,Activated,Finished,Dungeon,NorthMine,Storage,GoGoronCity,GoDeathMt,Bridge,Battle,BattlePlaying",
        "FironeMini_GiantHorse:Ready,Activated,Finish",
        "FironeMini_HeartPond:Ready,Activated,Finish,GetFlower,PresentFlower",
        "FironeMini_HorseEnemy:Ready,Activated,Finish,Exterminate",
        "FironeMini_TerribleThunder:Ready,Activated,Finish,Remove", "FirstOhenro:Ready,Activated,Finish,Step2",
        "FirstTower:Ready,Activated,Finish", "FourJewel:Ready,Activated,Finish",
        "Gaman:Ready,Activated,Finish,Step1,Step1_2,Step1_2_1,Step1_2_2,Step1_2_3,Step1_2_4,Step1_2_5,Step1_2_6,Step1_2_7,Step1_2_8,Step1_2_9,Step1_2_10,Step1_2_11,Step1_2_12,Step1_2_13,Step1_2_Failed,Step1_2_Successed,Step2,Gaman_Step2_2,Step2_3,Step2_4,Step2_5,Step2_6,Step2_7,Step2_8,Step2_9,Step2_10,Step2_11,Step2_12,Step2_13,Step2_14,Step2_15,Step2_16,Step2_17,Step2_18,Step2_19,Step2_20,Step2_21,Gaman_Step2_Failed",
        "GanonQuest:Ready,Activated,Finished", "GerudoMiniJewel:Ready,Activated,Finished",
        "Gerudo_CarryIce:Ready,Activated,Finished,Gerudo_CarryIce_bar,Get,Delivered", "Gerudo_CarryIce_mini:Ready",
        "Gerudo_Ch_FindingValetta:Ready,Activated,Finish,Step1,Step2", "Gerudo_Ch_Helmet:Ready,Activated,Finish",
        "Gerudo_Ch_Poison:Ready,Activated,Finish,Step1,Step2", "Gerudo_Ch_SandWarm:Ready,Activated,Finish",
        "Gerudo_Ch_SecretClub:Ready,Activated,Finish", "Gerudo_Ch_SnowBoots:Ready,Activated,Finish",
        "Gerudo_Ch_SnowMT:Ready,Activated,Finish", "Gerudo_HorseBuyer:Ready,Activated,Finish",
        "Gerudo_tsukamidake:Ready,Activated,Finish", "Get_MasterSword:Ready,Activated,Finish",
        "Giant_ZoraMini:Ready,Activated,Finish,Step1", "GodTree:Ready,Activated,Finish",
        "GoronCamp:Ready,Activated,Finish,GameReady,ResetGame",
        "GoronCamp_mini:Ready,Activated,Finish,GameReady,ResetGame",
        "GoronCityMini_BeatGolem:Ready,Activated,Finish,Beated", "GoronMini_ImportGem:Ready,Activated,Finish",
        "GoronMini_WallCrackTBox:Ready,Activated,Finish,GetOn,Get",
        "GoronsRock:Ready,Activated,Finish,Step005,Step010,Step020,Step030,Step040",
        "GotoZoraVillage:Ready,Activated,Finish,Step1", "HateeluMini_Treasure:Ready,Activated,Finish",
        "HatenoMini_BlueFire:Ready,Activated,Finish,Meet,Carry", "HatenoMini_CameraBoy:Ready,Activated,Finish,Shot",
        "HatenoMini_DevilSeal:Ready,Activated,Finish,GiveUtsuwa",
        "HatenoMini_GoatThief:Ready,Activated,Finished,Extermination",
        "HatenoMini_LoveInsects:Ready,Activated,Finish,Answer,Report",
        "HatenoMini_MyHome:Ready,Activated,Finish,Wood,Repurchase,Furniture",
        "HatenoMini_ThreeTree:Ready,Activated,Finished",
        "HatenoMini_WeaponMania:Ready,Activated,Finish,Weapon01,Weapon02,Weapon03,Weapon04,Weapon05,Weapon06,Weapon07",
        "HatenoMini_WhiteDragon:Ready,Activated,Finish,Step1,Play",
        "Hateno_SheikPad_PowerUp:Ready,Activated,Finish,Explain", "HigakkareMini_RedDragon:Ready,Activated,Finish,Step1",
        "HigakkareMini_StrangeMan:Ready,Activated,Finish", "HutagoHatago_Ch_001:Ready,Activated,Finish",
        "HyruleDepthMini_WhiteHorse:Ready,Activated,Finish", "HyrulePlainMini_Balloon:Ready,Activated,Finish",
        "Kakariko_Ch_004:Ready,Activated,Finish", "Kakariko_Ch_005:Ready,Activated,Finish",
        "Kakariko_Ch_006:Ready,Activated,Finish,Light", "Kakariko_Ch_Cooking2:Ready,Activated,Finish",
        "Kakariko_Ch_Cooking3:Ready,Activated,Finish", "Kakariko_Ch_Cooking4:Ready,Activated,Finish",
        "Kakariko_Cha_001:Ready,Activated,Finish", "Kakariko_Cha_003:Ready,Activated,Finish,10kokko",
        "Kaka_Kokko:01,02,03,04,05,06,07,08,09,10",  // (2026-11-21) cuccos back in the pen, one per bird
        "KnightDoll:Ready,Activated,Finish", "KorokMini_KorokShiren:Ready,Activated,Finish,Step010",
        "KorokMini_RiddleShiren:Ready,Activated,Finish,Step010,Step020,Step030,Step040",
        "KorokMini_RodShiren:Ready,Activated,Finish", "KorokMini_UMAShiren:Ready,Activated,Finish",
        "MacuseIseki:Ready,Activated,Finish", "MagneticFld:Ready,Activated,Finish",
        "MarittaMini_BigWhales:Ready,Activated,Finish",
        "MinakkareMini_Dragonfly:Ready,Activated,Finish,LittleSister,Dragonfly,Through,Give",
        "MinamihateeluMini_touzoku:Ready,Activated,Finish,FindTreasure", "MiniGame100enemy:Ready",
        "MiniGame_HorseRace:Ready", "MiniGame_KitakkareBF:1stClear", "MiniGame_ParasailArchery:Ready,Activated,Playing",
        "MouthofDragon:Ready,Activated,Finish,Step1", "MtMotelMini_Landscape:Ready,Activated,Finished",
        "My_Hero:Ready,Activated,Finish", "NakedIsland:Ready,Activated,Finish",
        "Oasis_Drug_Challenge:Ready,Activated,Finish", "OldKorok_Help:Ready,Activated,Finish,Maracus",
        "OneHundred:Ready,Activated,Finish,Step1", "PictureMemory:Ready,Activated,Finish,GetShirt,First1,Find11,Last1",
        "Relief_Landing:Ready,Activated,Finish", "Remains_Fancier:Ready,Activated,Finish,Step1",
        "RinelSearch:Ready,Activated,Finish", "RitoMini_Cook:Ready,Activated,Finish",
        "RitoMini_Flint:Ready,Activated,Finish", "RitoMini_IceGolem:Ready,Activated,Finish,Beated",
        "RitoRabitMountain:Ready,Activated,Finish", "RitoSongMystery:Ready,Activated,Finished,Shrine,Sister",
        "RitoUmayadoMini_HotRecipe:Ready,Activated,Finish",
        "Rito_BrosRock:Ready,Activated,Finish,Rito_BrosRock_Step1,Rito_BrosRock_Step2",
        "Rito_KeelSearch:Ready,Activated,Finish,Step1", "RiversideMini_CastleWeapon:Ready,Activated,Finish",
        "RiversideMini_RoyalRecipe:Ready,Activated,Finish", "SandStorm:Ready,Activated,Finish",
        "SanrokuMini_Lizard:Ready,Activated,Finish", "SearchStone:Ready,Activated,Finish,Step1",
        "SecretofObject:Ready,Activated,Finish", "SeekerEye:Ready,Activated,Finish",
        "SetugenUmayadoMini_Umahonephoto:Ready,Activated,Finish", "Shadow_Sign:Ready,Activated,Finish",
        "ShieldofKolog:Ready,Activated,Finish,Step010,Retire", "ShieldofKolog_mini:Ready,Activated,Finish,Retire,Goal",
        "Solitary_Maze:Ready,Activated,Finish", "StatueofZora:Ready,Activated,Finish,Step1",
        "StolenBook:Ready,Activated,Finish", "SunazarashiRace:Ready,Activated,Finish,Step1,Step2,Step3",
        "SunazarashiRace_mini:Ready", "TabantaBridgeMini_Sundial:Ready,Activated,Finish,Play",
        "Threebros_giant:Ready,Activated,Finish", "Thunder_Sword:Ready,Activated,Finish,Step010",
        "TreasureHunt01:Ready,Activated,Finish,Step01", "TreasureHunt02:Ready,Activated,Finish,Step01",
        "TreasureHunt03:Ready,Activated,Finish,Step01", "TreasureHunt04:Ready,Activated,Finish,Step01",
        "TreasureHunt_touzoku01:Ready,Activated,Finish,Step01", "TreasureHunt_touzoku02:Ready,Activated,Finish,Step01",
        "TreasureHunt_touzoku03:Ready,Activated,Finish,Step01", "TreasureHunt_touzoku04:Ready,Activated,Finish,Step01",
        "TreasureHunt_touzoku05:Ready,Activated,Finish,Step01", "TreasureHunt_touzoku06:Ready,Activated,Finish,Step01",
        "TreasureHunt_touzoku07:Ready,Activated,Finish,Step01", "TreasureHunt_touzoku08:Ready,Activated,Finish,Step01",
        "TutorialDungeon:Ready,Finish", "TwoWheels:Ready,Activated,Finish",
        "UMiiMini_GiveCake:Ready,Activated,Finish,Give",
        "UMiiMini_MakeVillage:Ready,Activated,Finish,Invite01,Invite02,Invite03,Invite04,Wood01,LookFor01,Wood02,LookFor02,Wood03,LookFor03,Wood04,LookFor04,LookFor05,Invite05,Marry",
        "UMiiMini_RichmansHobby:Ready,Activated,Finish", "UotoriMini_RecipeSea:Ready,Activated,Finish",
        "UotoriMini_RecoverBay:Ready,Activated,Finish,Exterminate",
        "UotoriMini_SinkTreasure:Ready,Activated,Finish,Salvage",
        "Water_Relic:Ready,Activated,Finished,Step5,Step1,Step1_1,Step1_2,Step2,Step3,Step4",
        "Wind_Relic:Ready,Activated,Finished,Dungeon,Saki,Step1,Parashawl,ParashawlPlaying,ParashawlFinished,Battle,BattlePlaying,BattleFinished",
        "ZoraMini_DiveChallenge:Ready,Activated,Finish,Step1,Step2", "ZoraMini_FlowedWife:Ready,Activated,Finish",
        "ZoraMini_HarvestingStone:Ready,Activated,Finish", "ZoraMini_ReliefSearch:Ready,Activated,Finish,SearchRelief",
        "Zora_FlogMini:Ready,Activated,Finish", "bf2_collabo:Ready,Activated,Finish",
        "isso_treasure_mini:Ready,Activated,Finish,open,Step2",
    };
    char s[96];
    for (int d = 0; d <= COOP_SHRINE_MAX; d++)
        for (size_t k = 0; k < sizeof kinds / sizeof *kinds; k++)
            snprintf(s, sizeof s, "%s%03d", kinds[k], d), named_add(s, 0, UINT64_MAX);
    for (int t = 1; t <= 15; t++) {
        snprintf(s, sizeof s, "MapTower_%02d", t), named_add(s, 0, UINT64_MAX);
        snprintf(s, sizeof s, "Location_MapTower%02d", t), named_add(s, 0, UINT64_MAX);
    }
    for (int b = 0; b < 4; b++) named_add(beasts[b], 0, UINT64_MAX);
    // (2026-11-12) Divine Beast progress: entered, all terminals on (opens the main control unit), Blight defeated,
    // the beast map app. Not Appear_PGanon* (the boss fight start: each game's own, near the control unit).
    static const char *const beast_names[] = {"Water", "Fire", "Wind", "Electric"};
    if (beast_on())
        for (int b = 0; b < 4; b++) {
            snprintf(s, sizeof s, "Enter_Remains%s", beast_names[b]), named_add(s, 0, UINT64_MAX);
            snprintf(s, sizeof s, "Access_AllTerminal%s", beast_names[b]), named_add(s, 0, UINT64_MAX);
            snprintf(s, sizeof s, "Die_PGanon%s", beast_names[b]), named_add(s, 0, UINT64_MAX);
            snprintf(s, sizeof s, "IsGet_App_%sRelic", beast_names[b]), named_add(s, 0, UINT64_MAX);
        }
    for (size_t i = 0; i < sizeof quest_deps / sizeof *quest_deps; i++) named_add(quest_deps[i], 0, UINT64_MAX);
    castle_named();  // (2026-11-14) the final boss's progress (castle.inc)
    memories_named();  // (2026-11-14) Captured Memories found (memories.inc)
    if (!quests_on()) return;
    for (size_t i = 0; i < sizeof quest_steps / sizeof *quest_steps; i++) {
        const char *q = quest_steps[i], *colon = strchr(q, ':');
        if (!colon) continue;
        int qlen = (int)(colon - q);
        const char *p = colon + 1;
        while (*p) {
            const char *comma = strchr(p, ',');
            int slen = comma ? (int)(comma - p) : (int)strlen(p);
            snprintf(s, sizeof s, "%.*s_%.*s", qlen, q, slen, p);
            named_add(s, 0, UINT64_MAX);
            p += slen + (comma ? 1 : 0);
        }
    }
}

static bool share_actor_name(const char *n) {
    return !strncmp(n, "TBox_", 5) || !strncmp(n, "Item_", 5) || !strncmp(n, "Weapon_", 7) || !strncmp(n, "Armor_", 6);
}
