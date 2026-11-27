// Included by coop.c after dishgive.inc. (2026-11-21, DISHINV) "Give" in the pouch's own item window: when the
// player presses A on a cooked dish in the pause inventory, the game opens its action window (SystemWindow_00 screen,
// "Comer / Receita / Cancelar"). That layout has a fourth button, Pa_Btn_03, that no item window uses: we show it
// (Type anim frame 7 = four buttons, found live), write "Dar a <partner>" on it (greyed while the partner is not
// within 10 m in the same area) and, when it is pressed, hand that dish to the partner through the dish-give path
// (dishgive.inc). Natively Btn_03 (id 0x5f) answers like Cancel (FUN_7100e59948), so pressing it just closes the
// window; we see the press in the button's own state. The dish leaves our pouch once the pause menu is closed (as with
// Options > Co-op). Off switch SAVAGEWIND_COOP_DISHINV=0. BOTW 1.9.0 (runtime addresses).
//
// Facts (Ghidra + live, 2026-11-21):
// - UI screen manager: LD64(LD64(0x7001d66d58)); +0x28 screen count, +0x30 array; slot 57 (+0x1c8) = SystemWindow_00
//   (vtable 0x7001cfffe0), created on demand (FUN_7100e2f564 reads it the same way).
// - Its open(type, item) (vtable slot, FUN_7100e590ac): +0x36c8 type (2/3 = pouch item: FUN_7100e57c68), +0x36d0 the
//   pouch record, +0x36d8 1 = the 3-button dish window (FUN_7100e5a248: Type anim frame 6) / 0 = 2 buttons (RoastItem).
// - +0x3628 "Type" anim (vfunc 0xc0(float frame): which buttons the layout shows), +0x3688 Pa_Btn_03 parts,
//   +0x3690.. buttons C, L, R, 00, 01, 02, 03 (ids 0x59..0x5f, +0x44); 00 = Cancel (bottom), 02 = top, 03 above it.
//   Button +0x3a flags: 0x10 shown (vfunc 0x60(bool)), 0x1000 cursor on it; +0x38 state (3 cursor on it, 4 pressed:
//   seen live for A on it, B presses Cancel = Btn_00 instead); vfunc 0x120(bool) greys it (a greyed button's press is
//   ignored by the window, FUN_7100e59948 -> FUN_7100f52dac). +0x36d9 / +0x36da: A / B pressed in this window
//   (FUN_7100e59638). Result global 0x7001dac9a8 (13 = none; 3 = Cancel / Btn_03), only logged.
// - Text: FUN_7100f61770(parts, "T_Btn_00", {u16 *text, u32 length}, 1, 0) (FUN_7100e84940).

#define A_UI_SCREENS      0x7001d66d58ull
#define VT_SYSWIN         0x7001cfffe0ull
#define A_SYSWIN_RESULT   0x7001dac9a8ull
#define A_PARTS_TEXT      0x7000f61770ull
#define SW_SLOT           0x1c8
#define SW_ANIM           0x3628
#define SW_PARTS_BTN03    0x3688
#define SW_BTN(i)         (0x3690 + 8 * (i))  // i: id - 0x59
#define SW_TYPE           0x36c8
#define SW_ITEM           0x36d0
#define SW_THREE          0x36d8
#define SW_A_PRESSED      0x36d9
#define SW_B_PRESSED      0x36da
#define BTN_FLAGS         0x3a
#define BTN_SHOWN         0x10
#define BTN_GIVE          6  // Btn_03 (id 0x5f)
#define DINV_FIT_DEFAULT  205  // (width units, dinv_fit)
#define DINV_MAX_UNITS    22   // the button shows at most 22 UTF-16 units (longer text is cut and drawn off-centre)

static volatile float dishinv_frame3 = 7.0f;  // the Type anim frame of 4 buttons (live 2026-11-21; dev: coop dishinv frame)
static volatile int dinv_log;  // (dev) log every change of the window's state
static void dinv_log_set(int on) { dinv_log = on; }
static void dinv_frames_set(float f3) { dishinv_frame3 = f3; }
// How much label fits the button, in width units (dinv_units: 10 = an average lower-case letter of the game's font).
// Measured 2026-11-21 on screenshots; dev: coop dishinv fit <units>.
static volatile int dinv_fit = DINV_FIT_DEFAULT;
static char dinv_override[96];  // (dev) coop dishinv label <text>: shown instead of "Dar a <name>" (fit tests)
static volatile int dinv_override_on;
static void dinv_fit_set(int units) { dinv_fit = units > 40 ? units : 40; }
static void dinv_override_set(const char *utf8) {
    dinv_override_on = 0;
    if (utf8) snprintf(dinv_override, sizeof dinv_override, "%s", utf8), dinv_override_on = 1;
}
static struct {
    uint64_t scr, item;
    int pid;  // the player this window's button gives to (fixed when the window opens; -1: nobody close then)
    CoopDish dish;
    bool ours, near, chosen;  // chosen: our give was picked in this window (leave the button alone from then on)
    uint8_t state;  // our button's state byte last frame
    uint32_t last_sig;
} dinv;
static uint64_t dinv_text, dinv_msg, dinv_label;

static bool dishinv_on(void) {
    static int on = -1;
    if (on < 0) on = look_on("SAVAGEWIND_COOP_DISHINV"), LOG("[coop] dishinv: give from the inventory's dish window: %s\n", on ? "on" : "off");
    return on && coop_dish_enabled();
}

static uint64_t dinv_screen(void) {
    uint64_t g = LD64(A_UI_SCREENS);
    if (!in_guest(g)) return 0;
    uint64_t mgr = LD64(g);
    if (!in_guest(mgr) || LD32(mgr + 0x28) <= SW_SLOT / 8) return 0;
    uint64_t arr = LD64(mgr + 0x30);
    if (!in_guest(arr)) return 0;
    uint64_t scr = LD64(arr + SW_SLOT);
    if (!in_guest(scr) || !in_guest(scr + 0x3700) || LD64(scr) != VT_SYSWIN) return 0;
    for (int i = 0; i < 7; i++)
        if (!in_guest(LD64(scr + SW_BTN(i)))) return 0;
    if (!in_guest(LD64(scr + SW_ANIM)) || !in_guest(LD64(scr + SW_PARTS_BTN03))) return 0;
    return scr;
}

static void dinv_vcall(Cpu *c, uint64_t obj, uint32_t slot, uint64_t x1, float s0, bool fl) {
    uint64_t vt = LD64(obj);
    if (!in_guest(vt)) return;
    Cpu call = *c;
    call.x[0] = obj, call.x[1] = x1;
    if (fl) {
        uint32_t bits;
        memcpy(&bits, &s0, 4);
        call.v[0] = bits;
    }
    guest_call(&call, LD64(vt + slot));
}

// Rough width of UTF-8 text in the game's proportional font (10 = an average lower-case letter).
static int dinv_char_units(uint32_t ch) {
    if (ch < 0x80 && strchr(" .,;:'!|", (int)ch)) return 5;
    if (ch == 'i' || ch == 'l' || ch == 'j' || ch == 't' || ch == 'f' || ch == 'r' || ch == 'I' || ch == 0xed || ch == 0xcd)
        return 6;  // (also í Í)
    if (ch == 'm' || ch == 'w' || ch == 'M' || ch == 'W') return 15;
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 0xc0 && ch <= 0xde)) return 12;  // (upper case, also À..Þ)
    return 10;
}
static int dinv_u16(const char *s) {  // UTF-16 code units of UTF-8 text
    int u = 0;
    while (*s) {
        uint32_t ch = '?';
        int n = coop_utf8_char(s, (int)strnlen(s, 4), &ch);
        s += n ? n : 1;
        u += ch > 0xffff ? 2 : 1;
    }
    return u;
}
static int dinv_units(const char *s) {
    int u = 0;
    while (*s) {
        uint32_t ch = '?';
        int n = coop_utf8_char(s, (int)strnlen(s, 4), &ch);
        s += n ? n : 1;
        u += dinv_char_units(ch);
    }
    return u;
}
#define DINV_ELLIPSIS "\xe2\x80\xa6"  // U+2026
#define DINV_ELLIPSIS_UNITS 10
// "Dar a <name>" (Portuguese game text) or "Give to <name>" for player pid; "parceiro" / "partner" while no name is
// known. A long name is cut at a character boundary with "…" so the label fits the button: at most dinv_fit width
// units and DINV_MAX_UNITS characters (measured 2026-11-21: "Dar a Ádrian Conceição" fits whole).
static void dinv_label_text(int pid, char *out, size_t cap) {
    char name[COOP_NAME_LEN] = "";
    if (pid >= 0 && !coop_net_player_name(pid, name, sizeof name)) name[0] = 0;
    const char *lang = language_text_folder();
    bool pt = lang && strstr(lang, "pt");
    if (!*name) {
        snprintf(out, cap, pt ? "Dar ao parceiro" : "Give to partner");
        return;
    }
    const char *pre = pt ? "Dar a " : "Give to ";
    int budget = dinv_fit - dinv_units(pre), room = DINV_MAX_UNITS - dinv_u16(pre);
    if (dinv_units(name) <= budget && dinv_u16(name) <= room) {
        snprintf(out, cap, "%s%s", pre, name);
        return;
    }
    budget -= DINV_ELLIPSIS_UNITS, room -= 1;
    char cut[COOP_NAME_LEN] = "";
    int k = 0, u = 0, w = 0;
    for (const char *p = name; *p;) {
        uint32_t ch = '?';
        int n = coop_utf8_char(p, (int)strnlen(p, 4), &ch);
        if (!n) break;
        int cw = ch > 0xffff ? 2 : 1;
        if (u + dinv_char_units(ch) > budget || w + cw > room) break;
        memcpy(cut + k, p, (size_t)n), k += n, u += dinv_char_units(ch), w += cw, p += n;
    }
    while (k && cut[k - 1] == ' ') k--;
    cut[k] = 0;
    snprintf(out, cap, "%s%s" DINV_ELLIPSIS, pre, cut);
}

static void dinv_set_text(Cpu *c, uint64_t parts, const char *utf8) {
    if (!dinv_text) dinv_text = (uint64_t)(uintptr_t)hle_alloc(128);
    if (!dinv_msg) dinv_msg = (uint64_t)(uintptr_t)hle_alloc(16);
    if (!dinv_label) {
        dinv_label = (uint64_t)(uintptr_t)hle_alloc(16);
        if (dinv_label) memcpy((void *)(uintptr_t)dinv_label, "T_Btn_00", 9);
    }
    if (!dinv_text || !dinv_msg || !dinv_label) return;
    uint16_t *w = (uint16_t *)(uintptr_t)dinv_text;  // (UTF-8 -> UTF-16; 128 bytes: up to 62 code units + NUL)
    int n = 0;
    for (const char *s = utf8; *s && n < 60;) {
        uint32_t ch = '?';
        int k = coop_utf8_char(s, (int)strnlen(s, 4), &ch);
        s += k ? k : 1;
        if (ch >= 0x10000) {
            ch -= 0x10000;
            w[n++] = (uint16_t)(0xd800 + (ch >> 10)), w[n++] = (uint16_t)(0xdc00 + (ch & 0x3ff));
        } else w[n++] = (uint16_t)ch;
    }
    w[n] = 0;
    ST64(dinv_msg, dinv_text);
    ST32(dinv_msg + 8, (uint32_t)n);
    Cpu call = *c;
    call.x[0] = parts, call.x[1] = dinv_label, call.x[2] = dinv_msg, call.x[3] = 1, call.x[4] = 0;
    guest_call(&call, A_PARTS_TEXT);
}

static bool dinv_near_now(void) {
    char area[96];
    return !dish_out.id && dinv.pid >= 0 && dish_player_near(dinv.pid, local_area(area, sizeof area));
}

// The window was (re)built for a dish: add our button.
static void dinv_augment(Cpu *c, uint64_t scr) {
    char label[96], area[96];
    uint64_t give = LD64(scr + SW_BTN(BTN_GIVE));
    dinv_vcall(c, LD64(scr + SW_ANIM), 0xc0, 0, dishinv_frame3, true);
    dinv_vcall(c, give, 0x60, 1, 0, false);
    // (more than two players) the nearest one within 10 m, fixed for this window; nobody close: the button names the
    // nearest one at any distance, greyed
    uint32_t here = local_area(area, sizeof area);
    dinv.pid = dish_nearest(here, false);
    int named = dinv.pid >= 0 ? dinv.pid : dish_nearest(here, true);
    if (dinv_override_on) snprintf(label, sizeof label, "%s", dinv_override);
    else dinv_label_text(named, label, sizeof label);
    dinv_set_text(c, LD64(scr + SW_PARTS_BTN03), label);
    dinv.near = dinv_near_now();
    dinv_vcall(c, give, 0x120, !dinv.near, 0, false);
    LOG("[coop] dishinv: window for %s: \"%s\" added (%s, player %d, %d/%d units, frame %g)\n", dinv.dish.name, label,
        dinv.near ? "partner close" : "greyed: partner not close", named, dinv_units(label), dinv_fit, dishinv_frame3);
}

static void dishinv_frame(Cpu *c) {
    if (!dishinv_on() || !local_in_world()) {
        dinv.scr = 0;
        return;
    }
    uint64_t scr = dinv_screen();
    if (!scr) {
        dinv.scr = 0;
        return;
    }
    int type = (int)LD32(scr + SW_TYPE);
    uint64_t item = LD64(scr + SW_ITEM), give = LD64(scr + SW_BTN(BTN_GIVE));
    uint16_t gflags = LD16(give + BTN_FLAGS);
    if (dinv_log) {
        uint32_t sig = (uint32_t)type * 131u + LD32(A_SYSWIN_RESULT) * 7u + LD8(scr + SW_A_PRESSED) * 3u +
                       LD8(scr + SW_B_PRESSED) * 5u;
        char b[160];
        int k = 0;
        for (int i = 3; i < 7; i++) {
            uint64_t bt = LD64(scr + SW_BTN(i));
            sig = sig * 31u + LD16(bt + BTN_FLAGS) + LD8(bt + 0x38) * 65536u;
            k += snprintf(b + k, sizeof b - k, " %x:%04x/%d", 0x59 + i, LD16(bt + BTN_FLAGS), LD8(bt + 0x38));
        }
        if (sig != dinv.last_sig)
            dinv.last_sig = sig, LOG("[coop] dishinv: win type=%d item=%llx result=%d a=%d b=%d%s\n", type,
                                     (unsigned long long)item, (int)LD32(A_SYSWIN_RESULT), LD8(scr + SW_A_PRESSED),
                                     LD8(scr + SW_B_PRESSED), b);
    }
    // A new window: every item window hides Btn_03 when it is built (FUN_7100e5a248 & co.), so a hidden Btn_03 on a
    // dish window means the game just (re)built it.
    if ((type == 2 || type == 3) && in_guest(item) && !(gflags & BTN_SHOWN)) {
        CoopDish d;
        dinv.scr = scr, dinv.item = item, dinv.ours = false, dinv.chosen = false, dinv.state = 0, dinv.pid = -1;
        // (only the 3-button dish window, Comer / Receita / Cancelar: FUN_7100e5a248; the 2-button one is for RoastItem)
        if (LD8(scr + SW_THREE) && dish_read(item, &d) && items_dish_name(d.name)) {
            CoopDishOffer check = {.from = 1, .to = 2, .id = 1, .area = 1, .to_pid = 1, .dish = d};
            if (coop_dish_offer_ok(&check)) {
                dinv.dish = d, dinv.ours = true;
                dinv_augment(c, scr);
                return;
            }
        }
        return;
    }
    if (!dinv.ours || scr != dinv.scr || item != dinv.item || !(gflags & BTN_SHOWN)) return;
    // Keep the grey state current while the window is open, but not after our give was picked: the window acts on the
    // press a few frames later and ignores it on a button greyed meanwhile (the queued give itself makes "near" false).
    if (dinv.chosen) return;
    bool near = dinv_near_now();
    if (near != dinv.near) dinv.near = near, dinv_vcall(c, give, 0x120, !near, 0, false);
    // Chosen: our button itself was pressed (button state +0x38: 3 cursor on it, 4 pressed; seen live: A on it -> 4,
    // then the window closes as for Cancel). B presses the Cancel button instead, never ours.
    // A greyed button goes to 4 too, but the window ignores it and stays open: refused here, with a notice.
    uint8_t state = LD8(give + 0x38);
    bool pressed = state == 4 && dinv.state != 4;
    dinv.state = state;
    if (pressed && !LD8(scr + SW_B_PRESSED)) {
        bool ok = near && dish_start(&dinv.dish, dinv.pid);
        dinv.chosen = ok;
        LOG("[coop] dishinv: give %s chosen: %s (player %d)\n", dinv.dish.name,
            ok ? "queued" : "refused (partner not close or busy)", dinv.pid);
        if (!ok) dish_status_set(dish_out.id ? DISH_SENDING : DISH_TOO_FAR);
    }
}

RT_ON_RESET(coop_dishinv_reset) {
    memset(&dinv, 0, sizeof dinv);
    dinv_text = dinv_msg = dinv_label = 0;
}
