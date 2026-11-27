// (coop-upnp) The router port's plain decisions and texts (runtime/coop_upnp.h): no OS calls, unit-tested by
// tests/test_coop_net.c "upnp". The sockets are in runtime/coop_upnp.cpp.
#include "coop_upnp.h"
#include "coop_upnp_parse.h"

#include <stdio.h>
#include <string.h>

int coop_upnp_addr_kind(const char *ip) {
    unsigned b[4];
    char tail;
    if (!ip || sscanf(ip, "%u.%u.%u.%u%c", &b[0], &b[1], &b[2], &b[3], &tail) != 4) return COOP_ADDR_INVALID;
    for (int i = 0; i < 4; i++)
        if (b[i] > 255) return COOP_ADDR_INVALID;
    if (b[0] == 10 || (b[0] == 172 && b[1] >= 16 && b[1] <= 31) || (b[0] == 192 && b[1] == 168) ||
        (b[0] == 169 && b[1] == 254) || b[0] == 127 || b[0] == 0)
        return COOP_ADDR_PRIVATE;
    if (b[0] == 100 && b[1] >= 64 && b[1] <= 127) return COOP_ADDR_SHARED;
    return COOP_ADDR_PUBLIC;
}

// (2026-11-23, a real router, miniupnpd 2.1: it returns descriptions with the tail of an older, longer one
// after ours, e.g. "Savage Wind Co-op" + "OtherGame(UDP)\x08\x01". So ours = starts with our description. The old
// helper window wrote "Savage Wind co-op NNNN" (lower-case "co-op"): not ours.)
bool coop_upnp_is_ours(const char *desc) {
    return desc && !strncmp(desc, COOP_UPNP_DESCRIPTION, sizeof COOP_UPNP_DESCRIPTION - 1);
}

int coop_upnp_decide(bool found, bool mine, int internal_port, const char *desc, int port) {
    if (!found) return COOP_UPNP_ADD;
    if (!mine || internal_port != port) return COOP_UPNP_LEAVE;  // (another PC, or another program's port here)
    if (coop_upnp_is_ours(desc)) return COOP_UPNP_ADOPT;  // (ours, left by an earlier game)
    return COOP_UPNP_REUSE;  // (to this PC, made by hand or by the old helper window: works, but not ours to remove)
}

int coop_upnp_final_state(int state, const char *external) {
    if (state != COOP_UPNP_OPEN || !external || !*external) return state;
    int kind = coop_upnp_addr_kind(external);
    return kind == COOP_ADDR_PRIVATE || kind == COOP_ADDR_SHARED ? COOP_UPNP_CGNAT : state;
}

static const char *en(const char *s) { return s; }

// (one line each: the game's description pane is one English line wide, about 80 characters)
const char *coop_upnp_status_text(const CoopUpnpStatus *s, const char *(*tr)(const char *), char *out, int cap) {
    if (!tr) tr = en;
    if (cap <= 0) return out;
    out[0] = 0;
    int port = s->port;
    switch (s->state) {
    case COOP_UPNP_IDLE: break;
    case COOP_UPNP_DISABLED:
        snprintf(out, cap, tr("Open router port is Off: forward UDP %d to this PC by hand."), port);
        break;
    case COOP_UPNP_UNSUPPORTED:
        snprintf(out, cap, tr("Forward UDP %d to this PC in your router's settings."), port);
        break;
    case COOP_UPNP_WORKING:
        snprintf(out, cap, tr("Asking your router to open UDP port %d..."), port);
        break;
    case COOP_UPNP_OPEN:
        if (s->external[0]) snprintf(out, cap, tr("Port %d open. Share this address: %s (A: copy)"), port, s->external);
        else snprintf(out, cap, tr("Router port %d: open. The router did not give its address."), port);
        break;
    case COOP_UPNP_NO_ROUTER:
        snprintf(out, cap, tr("Router did not answer (UPnP off): forward UDP %d to this PC by hand."), port);
        break;
    case COOP_UPNP_NO_LAN:
        snprintf(out, cap, tr("This PC's network address was not found: forward UDP %d by hand."), port);
        break;
    case COOP_UPNP_FOREIGN:
        snprintf(out, cap, tr("Port %d is already forwarded to another PC (%s)."), port, s->other[0] ? s->other : "?");
        break;
    case COOP_UPNP_REFUSED:
        snprintf(out, cap, tr("The router refused: forward UDP %d to this PC (%s) by hand."), port, s->lan[0] ? s->lan : "?");
        break;
    case COOP_UPNP_CGNAT:
        snprintf(out, cap, "%s", tr("Shared router address (CGNAT): friends can't reach you. Let a friend host."));
        break;
    }
    return out;
}

const char *coop_upnp_status_short(const CoopUpnpStatus *s) {
    switch (s->state) {
    case COOP_UPNP_WORKING: return "Checking...";
    case COOP_UPNP_OPEN: return s->dry_run ? "Open (test)" : "Port open";
    case COOP_UPNP_DISABLED: return "Turned off";
    case COOP_UPNP_CGNAT: return "Shared address";
    case COOP_UPNP_FOREIGN: return "In use";
    case COOP_UPNP_IDLE: return "-";
    default: return s->dry_run ? "Not open (test)" : "Not open";
    }
}

const char *const coop_upnp_services[COOP_UPNP_SERVICE_COUNT] = {
    "urn:schemas-upnp-org:service:WANIPConnection:1",
    "urn:schemas-upnp-org:service:WANIPConnection:2",
    "urn:schemas-upnp-org:service:WANPPPConnection:1",
};

typedef struct { const char *p; size_t n; } UpnpText;

static bool upnp_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static UpnpText upnp_trim(UpnpText t) {
    while (t.n && upnp_space(*t.p)) t.p++, t.n--;
    while (t.n && upnp_space(t.p[t.n - 1])) t.n--;
    return t;
}

static char upnp_lower(char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }

static bool upnp_equal(UpnpText t, const char *s, bool ignore_case) {
    if (t.n != strlen(s)) return false;
    for (size_t i = 0; i < t.n; i++)
        if (ignore_case ? upnp_lower(t.p[i]) != upnp_lower(s[i]) : t.p[i] != s[i]) return false;
    return true;
}

static bool upnp_number(UpnpText t, unsigned radix, unsigned max, unsigned *out) {
    unsigned n = 0;
    if (!t.n) return false;
    for (size_t i = 0; i < t.n; i++) {
        char c = upnp_lower(t.p[i]);
        unsigned d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : radix;
        if (d >= radix || d > max || n > (max - d) / radix) return false;
        n = n * radix + d;
    }
    *out = n;
    return true;
}

static bool upnp_private_sender(const char *sender) {
    unsigned octet[4];
    for (int i = 0; i < 4; i++) {
        size_t n = strcspn(sender, ".");
        if ((n > 1 && *sender == '0') || !upnp_number((UpnpText){sender, n}, 10, 255, &octet[i]) ||
            (i < 3 ? sender[n] != '.' : sender[n] != 0)) return false;
        sender += n + (i < 3);
    }
    unsigned a = octet[0], b = octet[1];
    return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168);
}

// Remove dot segments without decoding escapes or changing the query.
static void upnp_normalize_target(char *target) {
    char *segment = target, *write = target;
    while (*segment == '/') {
        char *end = segment + 1 + strcspn(segment + 1, "/?");
        size_t n = end - segment;
        if (n == 3 && !memcmp(segment, "/..", 3)) {
            while (write > target && *--write != '/') {}
            if (*end != '/') *write++ = '/';
        } else if (n == 2 && !memcmp(segment, "/.", 2)) {
            if (*end != '/') *write++ = '/';
        } else {
            memmove(write, segment, n);
            write += n;
        }
        segment = end;
    }
    memmove(write, segment, strlen(segment) + 1);
}

bool coop_upnp_url(const char *ref, const char *sender, const CoopUpnpUrl *base, CoopUpnpUrl *out) {
    if (!upnp_private_sender(sender) || !*ref) return false;
    for (const unsigned char *p = (const unsigned char *)ref; *p; p++)
        if (*p <= ' ' || *p >= 127 || strchr("\\\"<>#{}", *p)) return false;

    CoopUpnpUrl url = {0};
    size_t scheme = strcspn(ref, ":/?");
    bool absolute = !strncmp(ref, "//", 2) || ref[scheme] == ':';
    if (absolute) {
        if (ref[scheme] == ':' && (scheme != 4 || strlen(ref) < 7 ||
                                   !upnp_equal((UpnpText){ref, 7}, "http://", true))) return false;
        const char *host = ref + (ref[scheme] == ':' ? 7 : 2);
        const char *end = host + strcspn(host, "/?");
        const char *colon = memchr(host, ':', end - host);
        if (!upnp_equal((UpnpText){host, (colon ? colon : end) - host}, sender, false)) return false;
        url.port = 80;
        if (colon && (!upnp_number((UpnpText){colon + 1, end - colon - 1}, 10, 65535, &url.port) || !url.port))
            return false;
        ref = end;
    } else {
        if (!base) return false;
        url.port = base->port;
    }

    size_t prefix = 0;
    const char *path = "/";
    if (*ref != '/') {
        if (!absolute) {
            path = base->target;
            prefix = strcspn(path, "?");
            if (*ref != '?')
                while (prefix && path[prefix - 1] != '/') prefix--;
        } else {
            prefix = 1;
        }
    }
    size_t len = strlen(ref);
    if (prefix + len >= sizeof url.target) return false;
    memcpy(url.target, path, prefix);
    memcpy(url.target + prefix, ref, len + 1);
    upnp_normalize_target(url.target);
    *out = url;
    return true;
}

static int upnp_status_line(UpnpText line) {
    unsigned status;
    if (line.n < 12 || (memcmp(line.p, "HTTP/1.1 ", 9) && memcmp(line.p, "HTTP/1.0 ", 9)) ||
        (line.n > 12 && line.p[12] != ' ') || !upnp_number((UpnpText){line.p + 9, 3}, 10, 599, &status))
        return 0;
    return (int)status;
}

static bool upnp_header(UpnpText line, const char *name, UpnpText *value) {
    const char *colon = memchr(line.p, ':', line.n);
    if (!colon || !upnp_equal(upnp_trim((UpnpText){line.p, colon - line.p}), name, true)) return false;
    *value = upnp_trim((UpnpText){colon + 1, line.p + line.n - colon - 1});
    return true;
}

static UpnpText upnp_line(const char **p) {
    const char *line = *p, *end = strchr(line, '\n');
    *p = end ? end + 1 : line + strlen(line);
    return upnp_trim((UpnpText){line, (end ? end : *p) - line});
}

bool coop_upnp_ssdp(const char *response, const char *sender, CoopUpnpUrl *out) {
    if (upnp_status_line(upnp_line(&response)) != 200) return false;
    char ref[2048] = "";
    UpnpText line, value;
    while ((line = upnp_line(&response)).n) {
        if (!upnp_header(line, "location", &value)) continue;
        if (*ref || value.n == 0 || value.n >= sizeof ref) return false;
        memcpy(ref, value.p, value.n);
        ref[value.n] = 0;
    }
    return strlen(ref) >= 7 && upnp_equal((UpnpText){ref, 7}, "http://", true) &&
           coop_upnp_url(ref, sender, NULL, out);
}

bool coop_upnp_http_headers(const char *headers, CoopUpnpHttp *out) {
    CoopUpnpHttp http = {upnp_status_line(upnp_line(&headers)), -1, false};
    if (http.status < 200) return false;
    UpnpText line, value;
    while ((line = upnp_line(&headers)).n) {
        if (upnp_header(line, "content-length", &value)) {
            unsigned length;
            if (http.length >= 0 || !upnp_number(value, 10, COOP_UPNP_HTTP_MAX, &length)) return false;
            http.length = (int)length;
        } else if (upnp_header(line, "transfer-encoding", &value)) {
            if (http.chunked || !upnp_equal(value, "chunked", true)) return false;
            http.chunked = true;
        }
    }
    if (http.chunked && http.length >= 0) return false;
    *out = http;
    return true;
}

bool coop_upnp_chunk_size(const char *line, size_t *size) {
    unsigned n;
    if (!upnp_number(upnp_trim((UpnpText){line, strcspn(line, ";")}), 16, COOP_UPNP_HTTP_MAX, &n)) return false;
    *size = n;
    return true;
}

typedef struct { UpnpText name, local; bool close, empty; } UpnpTag;

// Only normal XML tags and the non-expanding constructs used by device descriptions.
// In particular, no DOCTYPE or external entities.
static const char *upnp_xml_skip(const char *p) {
    const char *end;
    if (!strncmp(p, "<!--", 4)) return (end = strstr(p + 4, "-->")) ? end + 3 : NULL;
    if (!strncmp(p, "<?", 2)) return (end = strstr(p + 2, "?>")) ? end + 2 : NULL;
    if (!strncmp(p, "<![CDATA[", 9)) return (end = strstr(p + 9, "]]>")) ? end + 3 : NULL;
    return NULL;
}

static bool upnp_tag_name_char(char c) {
    char lower = upnp_lower(c);
    return (lower >= 'a' && lower <= 'z') || (c >= '0' && c <= '9') || c == ':' || c == '_' ||
           c == '-' || c == '.';
}

static bool upnp_tag(const char **next, const char *end, const char **start, UpnpTag *tag) {
    const char *p = *next;
    for (;;) {
        if (p >= end || !(p = memchr(p, '<', end - p))) return false;
        if (p[1] != '!' && p[1] != '?') break;
        if (!(p = upnp_xml_skip(p))) return false;
    }
    *start = p++;
    tag->close = *p == '/';
    if (tag->close) p++;
    const char *name = p, *local = p;
    while (upnp_tag_name_char(*p)) {
        if (*p == ':') local = p + 1;
        p++;
    }
    if (p == local || (!upnp_space(*p) && *p != '>' && *p != '/')) return false;
    tag->name = (UpnpText){name, p - name};
    tag->local = (UpnpText){local, p - local};
    char quote = 0;
    for (; *p; p++) {
        if (quote) {
            if (*p == quote) quote = 0;
        } else if (*p == '"' || *p == '\'') {
            quote = *p;
        } else if (*p == '>') {
            tag->empty = p[-1] == '/';
            if (tag->empty && tag->close) return false;
            *next = p + 1;
            return *next <= end;
        } else if (*p == '<') {
            return false;
        }
    }
    return false;
}

static bool upnp_element(UpnpText xml, const char *name, UpnpText *text, const char **after) {
    UpnpTag tag;
    const char *start, *next = xml.p, *end = xml.p + xml.n;
    while (upnp_tag(&next, end, &start, &tag)) {
        if (tag.close || !upnp_equal(tag.local, name, false)) continue;
        text->p = next;
        text->n = 0;
        if (!tag.empty) {
            UpnpText stack[32] = {tag.name};
            size_t depth = 1;
            do {
                if (!upnp_tag(&next, end, &start, &tag)) return false;
                if (tag.close) {
                    UpnpText open = stack[--depth];
                    if (open.n != tag.name.n || memcmp(open.p, tag.name.p, open.n)) return false;
                } else if (!tag.empty) {
                    if (depth == sizeof stack / sizeof *stack) return false;
                    stack[depth++] = tag.name;
                }
            } while (depth);
            text->n = start - text->p;
        }
        *after = next;
        return true;
    }
    return false;
}

static const struct { char ch; const char *escaped; } upnp_entities[] = {
    {'&', "&amp;"}, {'<', "&lt;"}, {'>', "&gt;"}, {'"', "&quot;"}, {'\'', "&apos;"},
};

bool coop_upnp_xml_escape(const char *value, char *out, size_t cap) {
    while (*value) {
        const char *part = value;
        size_t n = 1;
        for (size_t i = 0; i < sizeof upnp_entities / sizeof *upnp_entities; i++) {
            if (*value != upnp_entities[i].ch) continue;
            part = upnp_entities[i].escaped;
            n = strlen(part);
            break;
        }
        if (cap <= n) return false;
        memcpy(out, part, n);
        out += n;
        cap -= n;
        value++;
    }
    if (!cap) return false;
    *out = 0;
    return true;
}

static bool upnp_entity(UpnpText entity, unsigned *ch) {
    for (size_t i = 0; i < sizeof upnp_entities / sizeof *upnp_entities; i++) {
        if (!upnp_equal(entity, upnp_entities[i].escaped, false)) continue;
        *ch = upnp_entities[i].ch;
        return true;
    }
    if (entity.n < 4 || entity.p[1] != '#') return false;
    bool hex = entity.p[2] == 'x';
    size_t prefix = hex ? 3 : 2;
    return upnp_number((UpnpText){entity.p + prefix, entity.n - prefix - 1}, hex ? 16 : 10, 0x10ffff, ch) &&
           (*ch == 9 || *ch == 10 || *ch == 13 || (*ch >= 32 && *ch < 0xfffe && (*ch < 0xd800 || *ch > 0xdfff)) ||
            *ch >= 0x10000);
}

static bool upnp_decode(UpnpText text, char *out, size_t cap) {
    const char *p = text.p, *end = p + text.n;
    while (p < end) {
        unsigned ch = (unsigned char)*p++;
        if (ch == '<') {
            const char *next = upnp_xml_skip(p - 1);
            if (!next || next > end) return false;
            if (!strncmp(p, "![CDATA[", 8)) {
                size_t n = next - 3 - (p + 8);
                if (n >= cap) return false;
                memcpy(out, p + 8, n);
                out += n;
                cap -= n;
            }
            p = next;
            continue;
        }
        size_t n = 1;
        if (ch == '&') {
            const char *semi = memchr(p, ';', end - p);
            if (!semi || !upnp_entity((UpnpText){p - 1, semi - p + 2}, &ch)) return false;
            p = semi + 1;
            n = ch < 0x80 ? 1 : ch < 0x800 ? 2 : ch < 0x10000 ? 3 : 4;
        }
        if (cap <= n) return false;
        if (n == 1) {
            *out = (char)ch;
        } else {
            for (size_t i = n - 1; i; i--, ch >>= 6) out[i] = (char)(0x80 | (ch & 0x3f));
            static const unsigned prefix[] = {0, 0, 0xc0, 0xe0, 0xf0};
            out[0] = (char)(prefix[n] | ch);
        }
        out += n;
        cap -= n;
    }
    if (!cap) return false;
    *out = 0;
    return true;
}

static bool upnp_field(UpnpText parent, const char *name, char *out, size_t cap, bool trim) {
    UpnpText text;
    const char *after;
    if (!upnp_element(parent, name, &text, &after) || !upnp_decode(text, out, cap)) return false;
    if (trim) {
        text = upnp_trim((UpnpText){out, strlen(out)});
        memmove(out, text.p, text.n);
        out[text.n] = 0;
    }
    return true;
}

bool coop_upnp_xml_text(const char *xml, const char *name, char *out, size_t cap) {
    return upnp_field((UpnpText){xml, strlen(xml)}, name, out, cap, true);
}

bool coop_upnp_control(const char *xml, const char **cursor, int type, const char *sender,
                       const CoopUpnpUrl *location, CoopUpnpUrl *out) {
    CoopUpnpUrl base = *location;
    char ref[2048];
    UpnpText doc = {xml, strlen(xml)}, base_text;
    const char *after;
    if (upnp_element(doc, "URLBase", &base_text, &after) &&
        (!upnp_decode(upnp_trim(base_text), ref, sizeof ref) ||
         (*ref && !coop_upnp_url(ref, sender, location, &base)))) return false;
    UpnpText service;
    const char *start = *cursor ? *cursor : xml, *end = xml + doc.n;
    while (upnp_element((UpnpText){start, end - start}, "service", &service, &start)) {
        *cursor = start;
        char urn[128];
        if (upnp_field(service, "serviceType", urn, sizeof urn, true) && !strcmp(urn, coop_upnp_services[type]) &&
            upnp_field(service, "controlURL", ref, sizeof ref, true) && coop_upnp_url(ref, sender, &base, out))
            return true;
    }
    return false;
}

static bool upnp_soap_body(const char *xml, UpnpText *body) {
    UpnpText envelope;
    const char *after;
    return upnp_element((UpnpText){xml, strlen(xml)}, "Envelope", &envelope, &after) &&
           upnp_element(envelope, "Body", body, &after);
}

bool coop_upnp_soap_response(const char *xml, const char *action) {
    char name[64];
    int n = snprintf(name, sizeof name, "%sResponse", action);
    UpnpText body, response;
    const char *after;
    return n > 0 && (size_t)n < sizeof name && upnp_soap_body(xml, &body) &&
           upnp_element(body, name, &response, &after);
}

bool coop_upnp_external(const char *xml, char *out, size_t cap) {
    UpnpText body, response;
    const char *after;
    return upnp_soap_body(xml, &body) && upnp_element(body, "GetExternalIPAddressResponse", &response, &after) &&
           upnp_field(response, "NewExternalIPAddress", out, cap, true);
}

int coop_upnp_fault(const char *xml) {
    UpnpText body, fault;
    const char *after;
    char code[16];
    unsigned n;
    if (upnp_soap_body(xml, &body) && upnp_element(body, "Fault", &fault, &after) &&
        upnp_field(fault, "errorCode", code, sizeof code, true) &&
        upnp_number((UpnpText){code, strlen(code)}, 10, 999, &n)) return (int)n;
    return 0;
}

bool coop_upnp_mapping(const char *xml, CoopUpnpMapping *out) {
    CoopUpnpMapping entry = {0};
    UpnpText body, response, lease;
    const char *after;
    char value[32];
    if (!upnp_soap_body(xml, &body) || !upnp_element(body, "GetSpecificPortMappingEntryResponse", &response, &after) ||
        !upnp_field(response, "NewInternalClient", entry.client, sizeof entry.client, true) || !*entry.client ||
        !upnp_field(response, "NewInternalPort", value, sizeof value, true) ||
        !upnp_number((UpnpText){value, strlen(value)}, 10, 65535, &entry.port) || !entry.port ||
        !upnp_field(response, "NewEnabled", value, sizeof value, true)) return false;
    UpnpText enabled = {value, strlen(value)};
    entry.enabled = upnp_equal(enabled, "1", false) || upnp_equal(enabled, "true", true) ||
                    upnp_equal(enabled, "yes", true);
    if (!entry.enabled && !upnp_equal(enabled, "0", false) && !upnp_equal(enabled, "false", true) &&
        !upnp_equal(enabled, "no", true)) return false;
    if (upnp_element(response, "NewLeaseDuration", &lease, &after) &&
        !upnp_number(upnp_trim(lease), 10, 0xffffffffu, &entry.lease)) return false;
    // (a description the router garbled into something unreadable: kept as "", i.e. not ours, never removed)
    if (!upnp_field(response, "NewPortMappingDescription", entry.description, sizeof entry.description, false))
        entry.description[0] = 0;
    *out = entry;
    return true;
}
