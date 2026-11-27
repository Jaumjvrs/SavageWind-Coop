#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COOP_UPNP_HTTP_MAX (64 * 1024)

typedef struct {
    unsigned port;
    char target[2048];
} CoopUpnpUrl;   // The host is always the numeric SSDP sender, kept separately.

typedef struct {
    char client[256];
    char description[1024];
    unsigned port, lease;
    bool enabled;
} CoopUpnpMapping;

enum { COOP_UPNP_IP1, COOP_UPNP_IP2, COOP_UPNP_PPP1, COOP_UPNP_SERVICE_COUNT };
extern const char *const coop_upnp_services[COOP_UPNP_SERVICE_COUNT];

// Resolve an HTTP URL or reference on this RFC1918 sender, optionally relative to base.
bool coop_upnp_url(const char *ref, const char *sender, const CoopUpnpUrl *base, CoopUpnpUrl *out);
bool coop_upnp_ssdp(const char *response, const char *sender, CoopUpnpUrl *out);
// Each call advances cursor past a service of the requested type with a usable control URL.
bool coop_upnp_control(const char *xml, const char **cursor, int type, const char *sender,
                       const CoopUpnpUrl *location, CoopUpnpUrl *out);
bool coop_upnp_xml_text(const char *xml, const char *name, char *out, size_t cap);
bool coop_upnp_xml_escape(const char *value, char *out, size_t cap);
bool coop_upnp_soap_response(const char *xml, const char *action);
bool coop_upnp_external(const char *xml, char *out, size_t cap);
int coop_upnp_fault(const char *xml);  // Zero means no parseable UPnP fault code.
bool coop_upnp_mapping(const char *xml, CoopUpnpMapping *out);

typedef struct {
    int status;
    int length;   // -1 means no Content-Length.
    bool chunked;
} CoopUpnpHttp;

bool coop_upnp_http_headers(const char *headers, CoopUpnpHttp *out);
bool coop_upnp_chunk_size(const char *line, size_t *size);

#ifdef __cplusplus
}
#endif
