/*****************************************************************************
Copyright 2026 UDR contributors

Path MTU discovery for UDR/UDT.
*****************************************************************************/

#include "udr_mtu.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

// macOS hides some IPv6 sockopts under _DARWIN_C_SOURCE; provide fallbacks.
#if defined(__APPLE__)
#ifndef IPV6_DONTFRAG
#define IPV6_DONTFRAG 62
#endif
#ifndef IPV6_PATHMTU
#define IPV6_PATHMTU 44
#endif
#endif

#if defined(__linux__)
#ifndef IP_MTU
#define IP_MTU 14
#endif
#ifndef IPV6_MTU
#define IPV6_MTU 24
#endif
#endif

// UDT handshake payload size (from UDT CHandShake::m_iContentSize)
static const int UDT_HANDSHAKE_CONTENT = 48;
// UDT minimum MSS: IPv4+UDP overhead + handshake
static const int UDT_MIN_MSS = 28 + UDT_HANDSHAKE_CONTENT;

// Safety margin for tunnels / extension headers / measurement error
static const int MTU_SAFETY_MARGIN = 8;

static int ip_header_len(int af) {
    return (af == AF_INET6) ? 40 : 20;
}

static int default_path_mtu(int af) {
    // Ethernet default; IPv6 floor is 1280 but most paths are still 1500
    (void)af;
    return 1500;
}

static int min_path_mtu(int af) {
    return (af == AF_INET6) ? 1280 : 576;
}

// Standard internet path MTU. Local NIC jumbo (9000+) must not be assumed
// end-to-end over WAN routes.
static const int INTERNET_PATH_MTU_CAP = 1500;

static int ipv4_is_loopback(const struct in_addr *a) {
    return (ntohl(a->s_addr) >> 24) == 127;
}

static int ipv4_is_linklocal(const struct in_addr *a) {
    return (ntohl(a->s_addr) >> 16) == 0xA9FE; // 169.254.0.0/16
}

static int ipv6_is_loopback(const struct in6_addr *a) {
    return IN6_IS_ADDR_LOOPBACK(a);
}

static int ipv6_is_linklocal(const struct in6_addr *a) {
    return IN6_IS_ADDR_LINKLOCAL(a);
}

// True if peer is on-link / same L2 domain where jumbo frames can be real.
// Internet destinations always return false → MTU capped at 1500.
static int peer_allows_jumbo(const struct sockaddr *peer) {
    if (!peer)
        return 0;

    if (peer->sa_family == AF_INET) {
        const struct sockaddr_in *p = (const struct sockaddr_in *)peer;
        if (ipv4_is_loopback(&p->sin_addr) || ipv4_is_linklocal(&p->sin_addr))
            return 1;

        struct ifaddrs *ifap = NULL;
        if (getifaddrs(&ifap) != 0)
            return 0;

        int onlink = 0;
        for (struct ifaddrs *ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
                continue;
            if (!ifa->ifa_netmask)
                continue;
            if (ifa->ifa_flags & IFF_LOOPBACK)
                continue;

            uint32_t addr = ntohl(((struct sockaddr_in *)ifa->ifa_addr)->sin_addr.s_addr);
            uint32_t mask = ntohl(((struct sockaddr_in *)ifa->ifa_netmask)->sin_addr.s_addr);
            uint32_t dst = ntohl(p->sin_addr.s_addr);
            if (mask != 0 && (addr & mask) == (dst & mask)) {
                onlink = 1;
                break;
            }
        }
        freeifaddrs(ifap);
        return onlink;
    }

    if (peer->sa_family == AF_INET6) {
        const struct sockaddr_in6 *p = (const struct sockaddr_in6 *)peer;
        if (ipv6_is_loopback(&p->sin6_addr) || ipv6_is_linklocal(&p->sin6_addr))
            return 1;
        // Unique-local (fc00::/7) may be LAN or VPN; only treat as jumbo-safe
        // if it shares a prefix with a local non-loopback interface.
        if (IN6_IS_ADDR_V4MAPPED(&p->sin6_addr))
            return 0;

        struct ifaddrs *ifap = NULL;
        if (getifaddrs(&ifap) != 0)
            return 0;

        int onlink = 0;
        for (struct ifaddrs *ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET6)
                continue;
            if (!ifa->ifa_netmask)
                continue;
            if (ifa->ifa_flags & IFF_LOOPBACK)
                continue;

            const struct in6_addr *ia =
                &((struct sockaddr_in6 *)ifa->ifa_addr)->sin6_addr;
            const struct in6_addr *nm =
                &((struct sockaddr_in6 *)ifa->ifa_netmask)->sin6_addr;
            const struct in6_addr *da = &p->sin6_addr;

            int match = 1;
            for (int i = 0; i < 16; i++) {
                if ((ia->s6_addr[i] & nm->s6_addr[i]) !=
                    (da->s6_addr[i] & nm->s6_addr[i])) {
                    match = 0;
                    break;
                }
            }

            // Prefix length: require >= /64 so default routes don't count
            // as on-link jumbo.
            int prefix_bits = 0;
            for (int i = 0; i < 16 && match; i++) {
                unsigned char b = nm->s6_addr[i];
                for (int bit = 7; bit >= 0; bit--) {
                    if (b & (1u << bit))
                        prefix_bits++;
                    else {
                        i = 16; // break outer
                        break;
                    }
                }
            }
            if (match && prefix_bits >= 64) {
                onlink = 1;
                break;
            }
        }
        freeifaddrs(ifap);
        return onlink;
    }

    return 0;
}

// Look up MTU of the local interface that would be used for peer (via connect+getsockname).
static int local_interface_mtu(int fd, int af) {
    struct sockaddr_storage local;
    socklen_t loclen = sizeof(local);
    memset(&local, 0, sizeof(local));
    if (getsockname(fd, (struct sockaddr *)&local, &loclen) != 0)
        return 0;

    char local_ip[INET6_ADDRSTRLEN];
    if (af == AF_INET) {
        struct sockaddr_in *in = (struct sockaddr_in *)&local;
        if (!inet_ntop(AF_INET, &in->sin_addr, local_ip, sizeof(local_ip)))
            return 0;
    } else if (af == AF_INET6) {
        struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)&local;
        if (!inet_ntop(AF_INET6, &in6->sin6_addr, local_ip, sizeof(local_ip)))
            return 0;
    } else {
        return 0;
    }

    struct ifaddrs *ifap = NULL;
    if (getifaddrs(&ifap) != 0)
        return 0;

    int mtu = 0;
    for (struct ifaddrs *ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || !ifa->ifa_name)
            continue;
        if (ifa->ifa_addr->sa_family != af)
            continue;

        char if_ip[INET6_ADDRSTRLEN];
        if (af == AF_INET) {
            struct sockaddr_in *in = (struct sockaddr_in *)ifa->ifa_addr;
            if (!inet_ntop(AF_INET, &in->sin_addr, if_ip, sizeof(if_ip)))
                continue;
        } else {
            struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)ifa->ifa_addr;
            if (!inet_ntop(AF_INET6, &in6->sin6_addr, if_ip, sizeof(if_ip)))
                continue;
        }

        if (strcmp(local_ip, if_ip) != 0)
            continue;

        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifa->ifa_name);
        if (ioctl(fd, SIOCGIFMTU, &ifr) == 0) {
            mtu = ifr.ifr_mtu;
            break;
        }

        // Some platforms need a datagram socket for SIOCGIFMTU
        int tmp = socket(af, SOCK_DGRAM, 0);
        if (tmp >= 0) {
            if (ioctl(tmp, SIOCGIFMTU, &ifr) == 0)
                mtu = ifr.ifr_mtu;
            close(tmp);
            if (mtu > 0)
                break;
        }
    }
    freeifaddrs(ifap);
    return mtu;
}

// Interface MTU by address string (for listeners with -i).
static int interface_mtu_for_ip(const char *ip, int af) {
    if (!ip || !ip[0])
        return 0;

    struct ifaddrs *ifap = NULL;
    if (getifaddrs(&ifap) != 0)
        return 0;

    int mtu = 0;
    for (struct ifaddrs *ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || !ifa->ifa_name)
            continue;
        if (ifa->ifa_addr->sa_family != af)
            continue;

        char if_ip[INET6_ADDRSTRLEN];
        if (af == AF_INET) {
            struct sockaddr_in *in = (struct sockaddr_in *)ifa->ifa_addr;
            if (!inet_ntop(AF_INET, &in->sin_addr, if_ip, sizeof(if_ip)))
                continue;
        } else if (af == AF_INET6) {
            struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)ifa->ifa_addr;
            if (!inet_ntop(AF_INET6, &in6->sin6_addr, if_ip, sizeof(if_ip)))
                continue;
        } else {
            continue;
        }

        if (strcmp(ip, if_ip) != 0)
            continue;

        int tmp = socket(af, SOCK_DGRAM, 0);
        if (tmp < 0)
            break;
        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifa->ifa_name);
        if (ioctl(tmp, SIOCGIFMTU, &ifr) == 0)
            mtu = ifr.ifr_mtu;
        close(tmp);
        break;
    }
    freeifaddrs(ifap);
    return mtu;
}

// Highest interface MTU of the given family (listener with unspecified bind).
static int max_interface_mtu(int af) {
    struct ifaddrs *ifap = NULL;
    if (getifaddrs(&ifap) != 0)
        return 0;

    int best = 0;
    int tmp = socket(af, SOCK_DGRAM, 0);
    if (tmp < 0) {
        freeifaddrs(ifap);
        return 0;
    }

    for (struct ifaddrs *ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || !ifa->ifa_name)
            continue;
        if (ifa->ifa_addr->sa_family != af)
            continue;
        if (ifa->ifa_flags & IFF_LOOPBACK)
            continue;
        if (!(ifa->ifa_flags & IFF_UP))
            continue;

        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifa->ifa_name);
        if (ioctl(tmp, SIOCGIFMTU, &ifr) == 0 && ifr.ifr_mtu > best)
            best = ifr.ifr_mtu;
    }
    close(tmp);
    freeifaddrs(ifap);
    return best;
}

static int enable_dontfrag(int fd, int af) {
    int on = 1;
    if (af == AF_INET) {
#if defined(IP_DONTFRAG)
        if (setsockopt(fd, IPPROTO_IP, IP_DONTFRAG, &on, sizeof(on)) == 0)
            return 0;
#endif
#if defined(IP_MTU_DISCOVER) && defined(IP_PMTUDISC_DO)
        {
            int val = IP_PMTUDISC_DO;
            if (setsockopt(fd, IPPROTO_IP, IP_MTU_DISCOVER, &val, sizeof(val)) == 0)
                return 0;
        }
#endif
#if defined(IP_DONTFRAGMENT) // Windows-style, rarely on Unix
        if (setsockopt(fd, IPPROTO_IP, IP_DONTFRAGMENT, &on, sizeof(on)) == 0)
            return 0;
#endif
    } else if (af == AF_INET6) {
#if defined(IPV6_DONTFRAG)
        if (setsockopt(fd, IPPROTO_IPV6, IPV6_DONTFRAG, &on, sizeof(on)) == 0)
            return 0;
#endif
#if defined(IPV6_MTU_DISCOVER) && defined(IPV6_PMTUDISC_DO)
        {
            int val = IPV6_PMTUDISC_DO;
            if (setsockopt(fd, IPPROTO_IPV6, IPV6_MTU_DISCOVER, &val, sizeof(val)) == 0)
                return 0;
        }
#endif
    }
    return -1;
}

static int query_os_pmtu(int fd, int af) {
    int mtu = 0;
    socklen_t len = sizeof(mtu);

    if (af == AF_INET) {
#ifdef IP_MTU
        if (getsockopt(fd, IPPROTO_IP, IP_MTU, &mtu, &len) == 0 && mtu > 0)
            return mtu;
#else
        (void)fd;
        (void)len;
#endif
    } else if (af == AF_INET6) {
#ifdef IPV6_MTU
        if (getsockopt(fd, IPPROTO_IPV6, IPV6_MTU, &mtu, &len) == 0 && mtu > 0)
            return mtu;
#endif
#ifdef IPV6_PATHMTU
        // BSD/macOS: IPV6_PATHMTU returns ip6_mtuinfo
        struct {
            struct sockaddr_in6 ip6m_addr;
            uint32_t ip6m_mtu;
        } mtuinfo;
        socklen_t mlen = sizeof(mtuinfo);
        memset(&mtuinfo, 0, sizeof(mtuinfo));
        if (getsockopt(fd, IPPROTO_IPV6, IPV6_PATHMTU, &mtuinfo, &mlen) == 0 &&
            mtuinfo.ip6m_mtu > 0)
            return (int)mtuinfo.ip6m_mtu;
#endif
#if !defined(IPV6_MTU) && !defined(IPV6_PATHMTU)
        (void)fd;
        (void)len;
#endif
    }
    return 0;
}

// Probe whether an IP packet of size ip_bytes can leave the host (and ideally the path).
// Returns 1 ok, 0 too big, -1 hard error.
static int probe_ip_size(int fd, int af, int ip_bytes) {
    int hdr = ip_header_len(af);
    int udp_payload = ip_bytes - hdr - 8; // UDP header
    if (udp_payload < 1)
        return 0;

    char *buf = (char *)calloc(1, (size_t)udp_payload);
    if (!buf)
        return -1;

    // Mark probe so it is unlikely to look like a UDT handshake if it arrives
    if (udp_payload >= 8) {
        memcpy(buf, "UDRMTU\0", 7);
    }

    ssize_t n = send(fd, buf, (size_t)udp_payload, 0);
    int err = errno;
    free(buf);

    if (n >= 0)
        return 1;

    if (err == EMSGSIZE || err == EOPNOTSUPP
#ifdef EFBIG
        || err == EFBIG
#endif
    )
        return 0;

    // Transient: treat as failure to probe this size upward
    if (err == EHOSTUNREACH || err == ENETUNREACH || err == ECONNREFUSED)
        return 1; // packet left stack; refusal means it was delivered to peer IP stack

    return -1;
}

int path_mtu_to_udt_mss(int path_mtu, int af) {
    // UDT: UDP payload (pkt) = MSS - 28
    // on-wire IP size = pkt + IP_hdr + UDP(8) = MSS - 28 + IP_hdr + 8
    //                = MSS + IP_hdr - 20
    // want on-wire <= path_mtu - safety
    // MSS <= path_mtu - safety - IP_hdr + 20
    int usable = path_mtu - MTU_SAFETY_MARGIN;
    if (usable < min_path_mtu(af))
        usable = min_path_mtu(af);

    int mss = usable - ip_header_len(af) + 20;
    if (mss < UDT_MIN_MSS)
        mss = UDT_MIN_MSS;
    return mss;
}

int discover_path_mtu(const struct sockaddr *peer, socklen_t peerlen, int verbose) {
    if (!peer)
        return default_path_mtu(AF_INET);

    int af = peer->sa_family;
    int floor_mtu = min_path_mtu(af);
    int fallback = default_path_mtu(af);

    int fd = socket(af, SOCK_DGRAM, 0);
    if (fd < 0) {
        if (verbose)
            fprintf(stderr, "[udr mtu] socket: %s\n", strerror(errno));
        return fallback;
    }

    // Short timeouts so discovery never stalls a transfer long
#if defined(SO_SNDTIMEO)
    {
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 300000; // 300ms
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
#endif

    if (connect(fd, peer, peerlen) != 0) {
        if (verbose)
            fprintf(stderr, "[udr mtu] connect for probe: %s\n", strerror(errno));
        close(fd);
        return fallback;
    }

    (void)enable_dontfrag(fd, af);

    int if_mtu = local_interface_mtu(fd, af);
    int os_mtu = query_os_pmtu(fd, af);
    int allow_jumbo = peer_allows_jumbo(peer);

    // Search ceiling:
    // - Internet / off-link: hard cap at 1500 even if the NIC has jumbo frames.
    //   DF probes succeed at 9000 locally and that does NOT mean the WAN path can.
    // - On-link / loopback: may use interface MTU up to 9000.
    int hi = INTERNET_PATH_MTU_CAP;
    if (allow_jumbo) {
        hi = INTERNET_PATH_MTU_CAP;
        if (if_mtu > hi)
            hi = if_mtu;
        if (os_mtu > hi)
            hi = os_mtu;
        if (hi > 9000)
            hi = 9000;
    } else {
        // Still respect a *lower* interface or OS hint (PPPoE 1492, etc.)
        if (if_mtu > 0 && if_mtu < hi)
            hi = if_mtu;
        if (os_mtu > 0 && os_mtu < hi)
            hi = os_mtu;
        // Never raise above 1500 for WAN
        if (hi > INTERNET_PATH_MTU_CAP)
            hi = INTERNET_PATH_MTU_CAP;
    }

    if (hi < floor_mtu)
        hi = floor_mtu;

    int lo = floor_mtu;

    if (verbose) {
        fprintf(stderr,
                "[udr mtu] discover %s: interface=%d os_hint=%d jumbo=%s search=%d..%d\n",
                af == AF_INET6 ? "IPv6" : "IPv4",
                if_mtu, os_mtu, allow_jumbo ? "yes(on-link)" : "no(wan-cap-1500)",
                lo, hi);
    }

    // Binary search largest IP packet size that send() accepts with DF
    int best = lo;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int r = probe_ip_size(fd, af, mid);
        if (r > 0) {
            best = mid;
            lo = mid + 1;
        } else if (r == 0) {
            hi = mid - 1;
        } else {
            // Hard error — stop and keep best so far
            if (verbose)
                fprintf(stderr, "[udr mtu] probe %d: %s (stopping search)\n",
                        mid, strerror(errno));
            break;
        }

        // Refresh OS estimate if ICMP updated it (can only lower the cap)
        int updated = query_os_pmtu(fd, af);
        if (updated > 0 && updated < hi) {
            hi = updated;
            if (!allow_jumbo && hi > INTERNET_PATH_MTU_CAP)
                hi = INTERNET_PATH_MTU_CAP;
            if (best > hi)
                best = hi;
        }
    }

    close(fd);

    if (best < floor_mtu)
        best = floor_mtu;

    // Final safety: WAN paths never advertise jumbo from a local jumbo NIC
    if (!allow_jumbo && best > INTERNET_PATH_MTU_CAP) {
        if (verbose)
            fprintf(stderr, "[udr mtu] clamping %d -> %d (internet path, local jumbo ignored)\n",
                    best, INTERNET_PATH_MTU_CAP);
        best = INTERNET_PATH_MTU_CAP;
    }

    if (verbose)
        fprintf(stderr, "[udr mtu] selected path MTU %d (%s)\n",
                best, af == AF_INET6 ? "IPv6" : "IPv4");

    return best;
}

int apply_udt_mss_for_peer(UDTSOCKET sock, const struct sockaddr *peer,
                           socklen_t peerlen, int verbose) {
    if (sock == UDT::INVALID_SOCK || !peer)
        return -1;

    int af = peer->sa_family;
    int pmtu = discover_path_mtu(peer, peerlen, verbose);
    int mss = path_mtu_to_udt_mss(pmtu, af);

    if (UDT::ERROR == UDT::setsockopt(sock, 0, UDT_MSS, &mss, sizeof(mss))) {
        if (verbose)
            fprintf(stderr, "[udr mtu] setsockopt UDT_MSS %d failed: %s\n",
                    mss, UDT::getlasterror().getErrorMessage());
        return -1;
    }

    if (verbose) {
        fprintf(stderr, "[udr mtu] set UDT_MSS=%d from path MTU %d (%s)\n",
                mss, pmtu, af == AF_INET6 ? "IPv6" : "IPv4");
    }
    return mss;
}

int apply_udt_mss_for_listener(UDTSOCKET sock, int af, const char *bind_ip,
                               int verbose) {
    if (sock == UDT::INVALID_SOCK)
        return -1;

    // Listeners accept internet peers: advertise MSS for standard 1500-byte
    // paths (IPv4/IPv6-aware). Local jumbo NIC MTU is NOT used here — the
    // connecting peer probes the real path and handshake takes min(MSS).
    // Optionally lower if the bound interface is < 1500 (e.g. VPN/PPPoE).
    int pmtu = INTERNET_PATH_MTU_CAP;
    int if_mtu = 0;
    if (bind_ip && bind_ip[0])
        if_mtu = interface_mtu_for_ip(bind_ip, af);
    if (if_mtu <= 0)
        if_mtu = max_interface_mtu(af);
    if (if_mtu > 0 && if_mtu < pmtu)
        pmtu = if_mtu;

    int mss = path_mtu_to_udt_mss(pmtu, af);

    if (UDT::ERROR == UDT::setsockopt(sock, 0, UDT_MSS, &mss, sizeof(mss))) {
        if (verbose)
            fprintf(stderr, "[udr mtu] listener UDT_MSS %d failed: %s\n",
                    mss, UDT::getlasterror().getErrorMessage());
        return -1;
    }

    if (verbose) {
        fprintf(stderr, "[udr mtu] listener UDT_MSS=%d from local MTU %d (%s)\n",
                mss, pmtu, af == AF_INET6 ? "IPv6" : "IPv4");
    }
    return mss;
}
