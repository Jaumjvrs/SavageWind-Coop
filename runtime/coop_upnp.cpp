// The router port for a co-op host (runtime/coop_upnp.h). SSDP and HTTP run on
// a background thread, bound to the LAN address that received the SSDP reply.
#include "coop_upnp.h"
#include "coop_upnp_parse.h"

#include <stdlib.h>
#include <string.h>

extern "C" {
#include "rt.h"
#include "settings.h"
}

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

struct Session {
    std::mutex m;
    std::condition_variable cv;
    bool stop = false, done = false;
    CoopUpnpStatus st = {};
};
// (never destroyed: a session's thread may still run while the process exits)
std::mutex &g_lock = *new std::mutex;
std::shared_ptr<Session> &g_cur = *new std::shared_ptr<Session>;
std::vector<std::shared_ptr<Session>> &g_live = *new std::vector<std::shared_ptr<Session>>;

bool stopping(Session *s) {
    if (!s) return false;
    std::lock_guard<std::mutex> l(s->m);
    return s->stop;
}

struct Socket {
    SOCKET fd = INVALID_SOCKET;
    explicit Socket(SOCKET fd = INVALID_SOCKET) : fd(fd) {}
    Socket(const Socket &) = delete;
    Socket(Socket &&other) noexcept : fd(std::exchange(other.fd, INVALID_SOCKET)) {}
    ~Socket() { if (fd != INVALID_SOCKET) closesocket(fd); }
};

struct Adapter {
    in_addr lan;
    std::vector<ULONG> gateways;
};

std::vector<Adapter> local_adapters() {
    std::vector<Adapter> out;
    ULONG bytes = 15000;
    // IP_ADAPTER_ADDRESSES needs its natural alignment.
    std::vector<IP_ADAPTER_ADDRESSES> data;
    for (int attempt = 0; attempt < 3; attempt++) {
        data.resize((bytes + sizeof data[0] - 1) / sizeof data[0]);
        ULONG rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST |
                                       GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                       nullptr, data.data(), &bytes);
        if (rc == ERROR_BUFFER_OVERFLOW) continue;
        if (rc != NO_ERROR) break;
        for (auto *a = data.data(); a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            std::vector<ULONG> gateways;
            for (auto *g = a->FirstGatewayAddress; g; g = g->Next) {
                auto *sa = g->Address.lpSockaddr;
                if (sa && sa->sa_family == AF_INET && g->Address.iSockaddrLength >= (int)sizeof(sockaddr_in)) {
                    ULONG ip = ((sockaddr_in *)sa)->sin_addr.s_addr;
                    if (ip) gateways.push_back(ip);
                }
            }
            if (gateways.empty() && (a->IfType == IF_TYPE_TUNNEL || a->IfType == IF_TYPE_PPP)) continue;
            for (auto *u = a->FirstUnicastAddress; u; u = u->Next) {
                auto *sa = u->Address.lpSockaddr;
                if (!sa || sa->sa_family != AF_INET || u->Address.iSockaddrLength < (int)sizeof(sockaddr_in)) continue;
                in_addr ip = ((sockaddr_in *)sa)->sin_addr;
                ULONG host = ntohl(ip.s_addr);
                if (!host || (host >> 24) == 127 || (host >> 16) == 0xa9fe) continue;
                out.push_back({ip, gateways});
            }
        }
        break;
    }
    std::stable_partition(out.begin(), out.end(), [](const Adapter &a) { return !a.gateways.empty(); });
    return out;
}

std::string ipv4(in_addr ip) {
    char text[INET_ADDRSTRLEN];
    return inet_ntop(AF_INET, &ip, text, sizeof text) ? text : "";
}

std::string masked(const std::string &ip) {
    size_t a = ip.find('.'), b = a == std::string::npos ? a : ip.find('.', a + 1);
    return b == std::string::npos ? std::string("?") : ip.substr(0, b) + ".x.x";
}

sockaddr_in endpoint(in_addr ip, unsigned port) {
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr = ip;
    addr.sin_port = htons((u_short)port);
    return addr;
}

struct Reply {
    in_addr lan, router;
    CoopUpnpUrl location;
    int preference;
    long ms;
};

std::vector<Reply> discover(const std::vector<Adapter> &adapters, Session &session, Clock::time_point start) {
    std::vector<Socket> sockets;
    std::vector<WSAPOLLFD> poll;
    std::vector<const Adapter *> listening;
    in_addr multicast;
    inet_pton(AF_INET, "239.255.255.250", &multicast);
    sockaddr_in to = endpoint(multicast, 1900);
    const char *const targets[] = {
        "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
        "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
        coop_upnp_services[COOP_UPNP_IP1], coop_upnp_services[COOP_UPNP_PPP1],
    };
    for (const Adapter &a : adapters) {
        if (stopping(&session)) break;
        Socket sock(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
        auto local = endpoint(a.lan, 0);
        u_long nonblocking = 1;
        if (sock.fd == INVALID_SOCKET || ioctlsocket(sock.fd, FIONBIO, &nonblocking) ||
            bind(sock.fd, (sockaddr *)&local, sizeof local) ||
            setsockopt(sock.fd, IPPROTO_IP, IP_MULTICAST_IF, (const char *)&a.lan, sizeof a.lan))
            continue;
        bool sent = false;
        for (const char *target : targets) {
            char msg[512];
            int n = snprintf(msg, sizeof msg, "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n"
                             "MAN: \"ssdp:discover\"\r\nMX: 2\r\nST: %s\r\n\r\n", target);
            if (sendto(sock.fd, msg, n, 0, (sockaddr *)&to, sizeof to) == n) sent = true;
        }
        if (!sent) continue;
        poll.push_back({sock.fd, POLLRDNORM, 0});
        sockets.push_back(std::move(sock));
        listening.push_back(&a);
    }

    std::vector<Reply> replies;
    auto end = Clock::now() + 3s;
    while (!poll.empty() && Clock::now() < end && !stopping(&session)) {
        int n = WSAPoll(poll.data(), (ULONG)poll.size(), 100);
        if (n == SOCKET_ERROR) break;
        if (!n) continue;
        for (size_t i = 0; i < poll.size(); i++) {
            if (!(poll[i].revents & POLLRDNORM)) continue;
            char msg[4097];
            sockaddr_in from = {};
            int len = sizeof from;
            int bytes = recvfrom(poll[i].fd, msg, sizeof msg - 1, 0, (sockaddr *)&from, &len);
            if (bytes <= 0 || memchr(msg, 0, bytes) || from.sin_family != AF_INET) continue;
            msg[bytes] = 0;
            const Adapter &a = *listening[i];
            int preference = a.gateways.empty() ? 0 : 1;
            if (std::find(a.gateways.begin(), a.gateways.end(), from.sin_addr.s_addr) != a.gateways.end()) preference = 2;
            long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
            Reply reply = {a.lan, from.sin_addr, {}, preference, ms};
            if (!coop_upnp_ssdp(msg, ipv4(reply.router).c_str(), &reply.location)) continue;
            auto duplicate = std::find_if(replies.begin(), replies.end(), [&](const Reply &r) {
                return r.lan.s_addr == reply.lan.s_addr && r.router.s_addr == reply.router.s_addr &&
                       r.location.port == reply.location.port && !strcmp(r.location.target, reply.location.target);
            });
            if (duplicate != replies.end()) continue;
            auto where = std::find_if(replies.begin(), replies.end(), [&](const Reply &r) {
                return r.preference < reply.preference;
            });
            // Keep a bounded list, even if a device floods the discovery port.
            if (replies.size() < 32 || where != replies.end()) {
                replies.insert(where, reply);
                if (replies.size() > 32) replies.pop_back();
            }
        }
    }
    return replies;
}

// One deadline covers the connect, request and whole response. Polling also
// lets discovery and read-only setup abandon requests when hosting stops.
class HttpSocket {
    Socket sock{socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
    Clock::time_point deadline = Clock::now() + 4s;
    Session *cancel;
    std::string incoming;
    size_t pos = 0;

    bool wait(bool write) {
        while (!stopping(cancel)) {
            auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now()).count();
            if (remaining <= 0) break;
            fd_set fds, errors;
            FD_ZERO(&fds);
            FD_ZERO(&errors);
            FD_SET(sock.fd, &fds);
            FD_SET(sock.fd, &errors);
            timeval tv = {0, (long)std::min<decltype(remaining)>(remaining, 100000)};
            int n = select(0, write ? nullptr : &fds, write ? &fds : nullptr, &errors, &tv);
            if (n) return n > 0 && FD_ISSET(sock.fd, &fds) && !FD_ISSET(sock.fd, &errors);
        }
        return false;
    }

    int read_more() {
        if (!wait(false)) return -1;
        char data[4096];
        int n = recv(sock.fd, data, sizeof data, 0);
        if (n > 0) {
            if (incoming.size() + n > 2 * COOP_UPNP_HTTP_MAX || memchr(data, 0, n)) return -1;
            incoming.append(data, n);
        }
        return n;
    }

    bool take_until(const char *separator, size_t limit, std::string &out) {
        size_t at;
        while ((at = incoming.find(separator, pos)) == std::string::npos) {
            if (incoming.size() - pos > limit || read_more() <= 0) return false;
        }
        if (at - pos > limit) return false;
        out.assign(incoming, pos, at - pos);
        pos = at + strlen(separator);
        return true;
    }

    bool take_bytes(size_t n, std::string &out) {
        while (incoming.size() - pos < n)
            if (read_more() <= 0) return false;
        out.append(incoming, pos, n);
        pos += n;
        return true;
    }

public:
    explicit HttpSocket(Session *cancel) : cancel(cancel) {}

    bool send_request(const Reply &router, const CoopUpnpUrl &url, const std::string &request) {
        auto local = endpoint(router.lan, 0), remote = endpoint(router.router, url.port);
        u_long nonblocking = 1;
        if (sock.fd == INVALID_SOCKET || stopping(cancel) || ioctlsocket(sock.fd, FIONBIO, &nonblocking) ||
            bind(sock.fd, (sockaddr *)&local, sizeof local)) return false;
        if (connect(sock.fd, (sockaddr *)&remote, sizeof remote)) {
            if (WSAGetLastError() != WSAEWOULDBLOCK || !wait(true)) return false;
            int error = 0, len = sizeof error;
            if (getsockopt(sock.fd, SOL_SOCKET, SO_ERROR, (char *)&error, &len) || error) return false;
        }
        size_t sent = 0;
        while (sent < request.size()) {
            if (!wait(true)) return false;
            int n = send(sock.fd, request.data() + sent, (int)(request.size() - sent), 0);
            if (n <= 0) return false;
            sent += n;
        }
        return true;
    }

    bool read_response(CoopUpnpHttp &head, std::string &body) {
        std::string line;
        if (!take_until("\r\n\r\n", 8192, line) || !coop_upnp_http_headers(line.c_str(), &head)) return false;
        if (head.chunked) {
            for (;;) {
                size_t chunk;
                if (!take_until("\r\n", 8192, line) || !coop_upnp_chunk_size(line.c_str(), &chunk)) return false;
                if (!chunk) {
                    do {
                        if (!take_until("\r\n", 8192, line)) return false;
                    } while (!line.empty());
                    return true;
                }
                if (chunk > COOP_UPNP_HTTP_MAX - body.size() || !take_bytes(chunk, body)) return false;
                std::string crlf;
                if (!take_bytes(2, crlf) || crlf != "\r\n") return false;
            }
        }
        if (head.length >= 0) return take_bytes(head.length, body);
        int n = 0;
        while (incoming.size() - pos <= COOP_UPNP_HTTP_MAX && (n = read_more()) > 0) {}
        return n == 0 && incoming.size() - pos <= COOP_UPNP_HTTP_MAX && take_bytes(incoming.size() - pos, body);
    }
};

struct HttpResponse {
    int status = 0;
    std::string body;
};

HttpResponse http(const Reply &router, const CoopUpnpUrl &url, const char *action, const std::string &body,
                  Session *cancel) {
    std::string request = action ? "POST " : "GET ";
    request += std::string(url.target) + " HTTP/1.1\r\nHost: " + ipv4(router.router) + ":" +
               std::to_string(url.port) + "\r\nConnection: close\r\n";
    if (action) {
        request += std::string("SOAPAction: \"") + action + "\"\r\nContent-Type: text/xml; charset=\"utf-8\"\r\n"
                   "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    request += "\r\n" + body;
    CoopUpnpHttp head;
    HttpResponse response;
    HttpSocket sock(cancel);
    if (sock.send_request(router, url, request) && sock.read_response(head, response.body))
        response.status = head.status;
    return response;
}

struct Igd {
    Reply router = {};
    CoopUpnpUrl control = {};
    const char *service = nullptr;
};

struct SoapResponse {
    bool ok;
    int fault;
    std::string xml;
};

using SoapArgs = std::initializer_list<std::pair<const char *, std::string>>;

SoapResponse soap(const Igd &igd, const char *action, SoapArgs args, Session *cancel = nullptr) {
    std::string body = "<?xml version=\"1.0\"?>"
                       "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                       "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:";
    body += std::string(action) + " xmlns:u=\"" + igd.service + "\">";
    for (const auto &arg : args) {
        char value[2048];
        if (!coop_upnp_xml_escape(arg.second.c_str(), value, sizeof value)) return {false, 0, {}};
        body += std::string("<") + arg.first + ">" + value + "</" + arg.first + ">";
    }
    body += std::string("</u:") + action + "></s:Body></s:Envelope>";
    std::string header = std::string(igd.service) + "#" + action;
    auto response = http(igd.router, igd.control, header.c_str(), body, cancel);
    int fault = response.status ? coop_upnp_fault(response.body.c_str()) : 0;
    bool ok = response.status == 200 && !fault && coop_upnp_soap_response(response.body.c_str(), action);
    if (!ok && !stopping(cancel) && !(fault == 714 && !strcmp(action, "GetSpecificPortMappingEntry")))
        LOG("[coop] upnp: %s failed (HTTP %d, UPnP fault %d)\n", action, response.status, fault);
    return {ok, fault, std::move(response.body)};
}

bool find_igd(const std::vector<Adapter> &adapters, Session &s, Clock::time_point start, Igd &igd, std::string &ext) {
    for (int round = 0; round < 2 && !stopping(&s); round++) {
        auto replies = discover(adapters, s, start);
        // Bound fallback probes on a noisy LAN, while allowing a slow router to
        // use the regular four seconds for each HTTP transaction.
        auto deadline = Clock::now() + 16s;
        for (const Reply &r : replies) {
            if (stopping(&s) || Clock::now() >= deadline) break;
            std::string sender = ipv4(r.router);
            LOG("[coop] upnp: the router answered after %ld ms (%s via %s)\n", r.ms, masked(sender).c_str(), masked(ipv4(r.lan)).c_str());
            auto desc = http(r, r.location, nullptr, {}, &s);
            if (desc.status != 200) continue;
            for (int type = 0; type < COOP_UPNP_SERVICE_COUNT; type++) {
                Igd candidate{r, {}, coop_upnp_services[type]};
                const char *cursor = nullptr;
                while (!stopping(&s) && Clock::now() < deadline &&
                       coop_upnp_control(desc.body.c_str(), &cursor, type, sender.c_str(), &r.location, &candidate.control)) {
                    auto response = soap(candidate, "GetExternalIPAddress", {}, &s);
                    char address[INET_ADDRSTRLEN];
                    in_addr parsed;
                    if (response.ok && coop_upnp_external(response.xml.c_str(), address, sizeof address) &&
                        inet_pton(AF_INET, address, &parsed) == 1) {
                        igd = candidate;
                        ext = ipv4(parsed);
                        LOG("[coop] upnp: found %s on %s\n", igd.service, masked(sender).c_str());
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

enum MappingResult { MAPPING_FOUND, MAPPING_ABSENT, MAPPING_UNKNOWN };

MappingResult get_mapping(const Igd &igd, int port, CoopUpnpMapping &mapping, Session *cancel = nullptr) {
    auto r = soap(igd, "GetSpecificPortMappingEntry",
                  {{"NewRemoteHost", ""}, {"NewExternalPort", std::to_string(port)}, {"NewProtocol", "UDP"}}, cancel);
    if (r.ok && coop_upnp_mapping(r.xml.c_str(), &mapping)) return MAPPING_FOUND;
    if (r.fault == 714) return MAPPING_ABSENT;  // NoSuchEntryInArray
    if (r.ok) LOG("[coop] upnp: the router's entry for UDP %d could not be read (%zu bytes)\n", port, r.xml.size());
    return MAPPING_UNKNOWN;
}

SoapResponse add_mapping(const Igd &igd, int port, const std::string &lan, unsigned lease) {
    std::string num = std::to_string(port);
    return soap(igd, "AddPortMapping", {{"NewRemoteHost", ""}, {"NewExternalPort", num}, {"NewProtocol", "UDP"},
                {"NewInternalPort", num}, {"NewInternalClient", lan}, {"NewEnabled", "1"},
                {"NewPortMappingDescription", COOP_UPNP_DESCRIPTION}, {"NewLeaseDuration", std::to_string(lease)}});
}

bool owns_mapping(const CoopUpnpMapping &entry, const std::string &lan, int port) {
    return entry.client == lan && entry.port == (unsigned)port && coop_upnp_is_ours(entry.description);
}

void remove_ours(const Session &s, const Igd &igd, const std::string &lan) {
    int port = s.st.port;
    if (s.st.dry_run) {
        LOG("[coop] upnp: would remove the router mapping of UDP %d (dry run: not removed)\n", port);
        return;
    }
    CoopUpnpMapping entry;
    switch (get_mapping(igd, port, entry)) {
    case MAPPING_FOUND:
        if (owns_mapping(entry, lan, port)) {
            auto r = soap(igd, "DeletePortMapping",
                          {{"NewRemoteHost", ""}, {"NewExternalPort", std::to_string(port)}, {"NewProtocol", "UDP"}});
            if (r.ok) LOG("[coop] upnp: removed the router mapping of UDP %d\n", port);
        } else {
            LOG("[coop] upnp: the mapping of UDP %d changed: left alone\n", port);
        }
        break;
    case MAPPING_ABSENT:
        LOG("[coop] upnp: the router no longer has a mapping of UDP %d\n", port);
        break;
    default:
        LOG("[coop] upnp: could not verify ownership of UDP %d: left alone\n", port);
        break;
    }
}

void run(std::shared_ptr<Session> sp, std::shared_ptr<Session> prior) {
    Session &s = *sp;
    // A rapid stop/restart on the same port must finish the previous cleanup
    // before the new session can adopt or create a mapping.
    if (prior) {
        std::unique_lock<std::mutex> l(prior->m);
        prior->cv.wait(l, [&] { return prior->done; });
    }
    int port = s.st.port;
    bool dry = s.st.dry_run;
    auto start = Clock::now();
    WSADATA wsa;
    bool wsa_up = !stopping(&s) && !WSAStartup(MAKEWORD(2, 2), &wsa);
    auto adapters = wsa_up ? local_adapters() : std::vector<Adapter>{};
    Igd igd;
    int state = COOP_UPNP_NO_ROUTER;
    std::string ext, lan, other;
    bool owned = false;
    unsigned lease = 0;
    auto ms = [&] { return (long)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count(); };
    LOG("[coop] upnp: asking the router to forward UDP %d to this PC%s\n", port,
        dry ? " - dry run, the router is not changed" : "");
    if (adapters.empty()) {
        state = COOP_UPNP_NO_LAN;
        if (!stopping(&s)) LOG("[coop] upnp: this PC's network address was not found\n");
    } else if (!find_igd(adapters, s, start, igd, ext)) {
        if (!stopping(&s)) LOG("[coop] upnp: no usable router found after %ld ms\n", ms());
    } else {
        lan = ipv4(igd.router.lan);
        CoopUpnpMapping entry = {};
        MappingResult result = get_mapping(igd, port, entry, &s);
        state = COOP_UPNP_REFUSED;
        if (result != MAPPING_UNKNOWN && !stopping(&s)) {
            bool found = result == MAPPING_FOUND;
            bool mine = found && std::any_of(adapters.begin(), adapters.end(), [&](const Adapter &a) {
                return ipv4(a.lan) == entry.client;
            });
            if (found)
                LOG("[coop] upnp: UDP %d already has a mapping (%s)\n", port, entry.enabled ? "on" : "off");
            else
                LOG("[coop] upnp: UDP %d has no mapping yet\n", port);

            switch (coop_upnp_decide(found, mine, entry.port, entry.description, port)) {
            case COOP_UPNP_ADD:
                if (dry) {
                    LOG("[coop] upnp: would add UDP %d -> %s:%d \"%s\" (dry run: not added)\n",
                        port, masked(lan).c_str(), port, COOP_UPNP_DESCRIPTION);
                    owned = true;
                } else {
                    auto r = add_mapping(igd, port, lan, 0);
                    // A SOAP fault can mean the router disallows lease 0. Check
                    // the port again before retrying, including after a conflict.
                    if (!r.ok && r.fault && !stopping(&s) &&
                        get_mapping(igd, port, entry, &s) == MAPPING_ABSENT && !stopping(&s)) {
                        lease = 3600;
                        r = add_mapping(igd, port, lan, lease);
                    }
                    owned = r.ok;
                    // A timeout can lose the reply after the router applied the mapping.
                    if (!owned && !r.fault && get_mapping(igd, port, entry) == MAPPING_FOUND &&
                        owns_mapping(entry, lan, port)) {
                        owned = true;
                        lease = entry.lease;
                    }
                    if (owned) LOG("[coop] upnp: added UDP %d -> %s:%d (lease %u)\n", port, masked(lan).c_str(), port, lease);
                }
                if (owned) state = COOP_UPNP_OPEN;
                break;
            case COOP_UPNP_ADOPT:
                owned = true, state = COOP_UPNP_OPEN;
                if (!dry && owns_mapping(entry, lan, port)) lease = entry.lease;
                LOG("[coop] upnp: reusing our own mapping of UDP %d\n", port);
                break;
            case COOP_UPNP_REUSE:
                state = COOP_UPNP_OPEN;
                LOG("[coop] upnp: UDP %d already goes to this PC: using it as it is\n", port);
                break;
            default:
                state = COOP_UPNP_FOREIGN;
                {   // (security review 3: router text is shown in the lobby; keep only a real IPv4 address)
                    in_addr parsed;
                    char canon[INET_ADDRSTRLEN];
                    other = inet_pton(AF_INET, entry.client, &parsed) == 1 &&
                                    inet_ntop(AF_INET, &parsed, canon, sizeof canon) ? canon : "?";
                }
                LOG("[coop] upnp: UDP %d is already forwarded elsewhere: left alone\n", port);
                break;
            }
        } else if (!stopping(&s)) {
            LOG("[coop] upnp: could not verify the existing mapping for UDP %d: left alone\n", port);
        }
        state = coop_upnp_final_state(state, ext.c_str());
        static const char *const kinds[] = {"public", "private", "shared (CGNAT)"};
        LOG("[coop] upnp: the router's internet address: %s (%s)\n", masked(ext).c_str(),
            kinds[coop_upnp_addr_kind(ext.c_str())]);
    }
    {
        std::lock_guard<std::mutex> l(s.m);
        s.st.state = state;
        s.st.owned = owned;
        snprintf(s.st.lan, sizeof s.st.lan, "%s", lan.c_str());
        snprintf(s.st.external, sizeof s.st.external, "%s", ext.c_str());
        snprintf(s.st.other, sizeof s.st.other, "%.47s", other.c_str());
        CoopUpnpStatus copy = s.st;
        snprintf(copy.external, sizeof copy.external, "%s", ext.empty() ? "" : masked(ext).c_str());
        char t[256];
        LOG("[coop] upnp: status after %ld ms: %s\n", ms(), coop_upnp_status_text(&copy, nullptr, t, sizeof t));
    }
    if (owned && !dry && lease) {
        std::unique_lock<std::mutex> l(s.m);
        auto period = std::chrono::seconds(std::max(1u, std::min(1800u, lease / 2)));
        while (!s.cv.wait_for(l, period, [&] { return s.stop; })) {
            l.unlock();
            CoopUpnpMapping entry;
            auto result = get_mapping(igd, port, entry, &s);
            if (!stopping(&s)) {
                if (result == MAPPING_FOUND && owns_mapping(entry, lan, port) &&
                    add_mapping(igd, port, lan, 3600).ok) {
                    LOG("[coop] upnp: renewed the mapping of UDP %d\n", port);
                    period = 1800s;
                } else if (result == MAPPING_UNKNOWN ||
                           (result == MAPPING_FOUND && owns_mapping(entry, lan, port))) {
                    period = std::min(period, 60s);
                } else {
                    // Do not replace a mapping whose ownership changed or was lost.
                    l.lock();
                    s.st.owned = owned = false;
                    s.st.state = COOP_UPNP_REFUSED;
                    break;
                }
            }
            l.lock();
        }
    }
    {
        std::unique_lock<std::mutex> l(s.m);
        s.cv.wait(l, [&] { return s.stop; });
    }
    if (owned) remove_ours(s, igd, lan);
    if (wsa_up) WSACleanup();
    std::lock_guard<std::mutex> l(s.m);
    s.done = true;
    s.cv.notify_all();
}

bool env_off(const char *name) {
    const char *v = getenv(name);
    return v && !strcmp(v, "0");
}
bool env_on(const char *name) {
    const char *v = getenv(name);
    return v && *v && strcmp(v, "0");
}

}  // namespace

extern "C" void coop_upnp_start(int port) {
    std::lock_guard<std::mutex> l(g_lock);
    if (g_cur) return;  // (one hosting session at a time)
    auto s = std::make_shared<Session>();
    s->st.port = port;
    // (test instances: SAVAGEWIND_CONTROL is the developer control socket; players' games never set it)
    s->st.dry_run = env_on("SAVAGEWIND_COOP_UPNP_DRYRUN") || (env_on("SAVAGEWIND_CONTROL") && !env_off("SAVAGEWIND_COOP_UPNP_DRYRUN"));
    g_cur = s;
    if (env_off("SAVAGEWIND_COOP_UPNP") || !g_settings.coop_upnp) {
        s->st.state = COOP_UPNP_DISABLED;
        s->done = true;
        LOG("[coop] upnp: off (%s): the router is not asked about UDP %d\n",
            env_off("SAVAGEWIND_COOP_UPNP") ? "SAVAGEWIND_COOP_UPNP=0" : "Open router port automatically: Off", port);
        return;
    }
    s->st.state = COOP_UPNP_WORKING;
    std::shared_ptr<Session> prior;
    for (auto it = g_live.rbegin(); it != g_live.rend(); ++it) {
        if ((*it)->st.port != port) continue;
        prior = *it;
        break;
    }
    g_live.push_back(s);
    try {
        std::thread(run, s, prior).detach();
    } catch (...) {
        s->st.state = COOP_UPNP_NO_ROUTER;
        s->done = true;
        LOG("[coop] upnp: no thread for the router\n");
    }
}

extern "C" void coop_upnp_stop(void) {
    std::lock_guard<std::mutex> l(g_lock);
    if (g_cur) {
        std::lock_guard<std::mutex> sl(g_cur->m);
        g_cur->stop = true;
        g_cur->cv.notify_all();
    }
    g_cur.reset();
    for (size_t i = 0; i < g_live.size();) {  // (forget the sessions that have finished)
        bool done;
        { std::lock_guard<std::mutex> sl(g_live[i]->m); done = g_live[i]->done; }
        if (done) g_live.erase(g_live.begin() + i);
        else i++;
    }
}

extern "C" void coop_upnp_shutdown(void) {
    coop_upnp_stop();
    std::vector<std::shared_ptr<Session>> live;
    {
        std::lock_guard<std::mutex> l(g_lock);
        live = g_live;
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (auto &s : live) {
        std::unique_lock<std::mutex> l(s->m);
        if (!s->cv.wait_until(l, deadline, [&] { return s->done; }))
            LOG("[coop] upnp: the router did not finish in time (UDP %d)\n", s->st.port);
    }
}

extern "C" void coop_upnp_status(CoopUpnpStatus *out) {
    std::lock_guard<std::mutex> l(g_lock);
    if (!g_cur) { memset(out, 0, sizeof *out); return; }
    std::lock_guard<std::mutex> sl(g_cur->m);
    *out = g_cur->st;
}

// (0.1.1, first two-PC test: the notebook on the same Wi-Fi could not join through the router's internet
// address, only through the home network) The UDP socket is only "connected" to pick the route: nothing is sent. Kept for
// 5 s (the menu asks every redraw).
extern "C" bool coop_lan_address(char *out, int cap) {
    static char cached[INET_ADDRSTRLEN];
    static ULONGLONG at;
    CoopUpnpStatus st;
    coop_upnp_status(&st);
    if (st.lan[0] && coop_upnp_addr_kind(st.lan) == COOP_ADDR_PRIVATE) return snprintf(out, cap, "%s", st.lan) < cap;
    ULONGLONG now = GetTickCount64();
    if (!at || now - at > 5000) {
        at = now, cached[0] = 0;
        SOCKET fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd != INVALID_SOCKET) {
            sockaddr_in to = {}, me = {};
            to.sin_family = AF_INET, to.sin_port = htons(53);
            inet_pton(AF_INET, "8.8.8.8", &to.sin_addr);
            int n = sizeof me;
            char t[INET_ADDRSTRLEN];
            if (!connect(fd, (sockaddr *)&to, sizeof to) && !getsockname(fd, (sockaddr *)&me, &n) &&
                inet_ntop(AF_INET, &me.sin_addr, t, sizeof t) && coop_upnp_addr_kind(t) == COOP_ADDR_PRIVATE &&
                strncmp(t, "127.", 4) && strncmp(t, "169.254.", 8) && strncmp(t, "0.", 2))
                snprintf(cached, sizeof cached, "%s", t);
            closesocket(fd);
        }
    }
    return cached[0] && snprintf(out, cap, "%s", cached) < cap;
}

#else  // (not Windows: no UPnP here; the player forwards the port)

#include <stdio.h>
static CoopUpnpStatus g_st;
extern "C" void coop_upnp_start(int port) {
    memset(&g_st, 0, sizeof g_st);
    g_st.port = port;
    const char *off = getenv("SAVAGEWIND_COOP_UPNP");
    g_st.state = (off && !strcmp(off, "0")) || !g_settings.coop_upnp ? COOP_UPNP_DISABLED : COOP_UPNP_UNSUPPORTED;
    LOG("[coop] upnp: not available on this system: forward UDP %d by hand\n", port);
}
extern "C" void coop_upnp_stop(void) { memset(&g_st, 0, sizeof g_st); }
extern "C" void coop_upnp_shutdown(void) {}
extern "C" void coop_upnp_status(CoopUpnpStatus *out) { *out = g_st; }
extern "C" bool coop_lan_address(char *out, int cap) { (void)out, (void)cap; return false; }

#endif
