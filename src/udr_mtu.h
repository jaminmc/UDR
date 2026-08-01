/*****************************************************************************
Copyright 2026 UDR contributors

Path MTU discovery helpers for choosing UDT_MSS on IPv4/IPv6 routes.
*****************************************************************************/

#ifndef UDR_MTU_H
#define UDR_MTU_H

#include <sys/socket.h>
#include <udt.h>

// Discover the path MTU (IP-layer packet size in bytes) toward peer.
// Uses interface MTU, OS PMTU hints when available, and DF-bit probe search.
// Returns a sane default if probing fails (1500 IPv4 / 1280 IPv6 floor).
int discover_path_mtu(const struct sockaddr *peer, socklen_t peerlen, int verbose);

// Convert a path MTU + address family into a UDT_MSS value.
// UDT always subtracts 28 (IPv4+UDP) from MSS for the UDP payload, so IPv6
// must use a lower MSS to keep the on-wire packet within path MTU.
int path_mtu_to_udt_mss(int path_mtu, int af);

// Best-effort UDT_MSS for a connected/connecting peer address.
// Returns the MSS applied, or <=0 on failure (socket left unchanged).
int apply_udt_mss_for_peer(UDTSOCKET sock, const struct sockaddr *peer,
                           socklen_t peerlen, int verbose);

// Best-effort UDT_MSS for a listening socket given address family and optional
// bind address string (IPv4/IPv6 literal). Uses interface MTU / family default.
int apply_udt_mss_for_listener(UDTSOCKET sock, int af, const char *bind_ip,
                               int verbose);

#endif
