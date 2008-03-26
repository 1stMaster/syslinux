#ifndef _GPXE_SOCKET_H
#define _GPXE_SOCKET_H

/** @file
 *
 * Socket addresses
 *
 */

/**
 * @defgroup commtypes Communication semantics
 *
 * @{
 */

/** Connection-based, reliable streams */
#define SOCK_STREAM	( ( int ) TCP_SOCK_STREAM )
extern char TCP_SOCK_STREAM[];

/** Connectionless, unreliable streams */
#define SOCK_DGRAM	( ( int ) UDP_SOCK_DGRAM )
extern char UDP_SOCK_DGRAM[];

/** @} */

/**
 * Name communication semantics
 *
 * @v semantics		Communication semantics (e.g. SOCK_STREAM)
 * @ret name		Name of communication semantics
 */
static inline __attribute__ (( always_inline )) const char *
socket_semantics_name ( int semantics ) {
	/* Cannot use a switch() because of the {TCP_UDP}_SOCK_XXX hack */
	if ( semantics == SOCK_STREAM ) {
		return "SOCK_STREAM";
	} else if ( semantics == SOCK_DGRAM ) {
		return "SOCK_DGRAM";
	} else {
		return "SOCK_UNKNOWN";
	}
}

/**
 * @defgroup addrfam Address families
 *
 * @{
 */
#define AF_INET		1	/**< IPv4 Internet addresses */
#define AF_INET6	2	/**< IPv6 Internet addresses */
/** @} */

/**
 * Name address family
 *
 * @v family		Address family (e.g. AF_INET)
 * @ret name		Name of address family
 */
static inline __attribute__ (( always_inline )) const char *
socket_family_name ( int family ) {
	switch ( family ) {
	case AF_INET:		return "AF_INET";
	case AF_INET6:		return "AF_INET6";
	default:		return "AF_UNKNOWN";
	}
}

/** A socket address family */
typedef uint16_t sa_family_t;

/** Length of a @c struct @c sockaddr */
#define SA_LEN 32

/**
 * Generalized socket address structure
 *
 * This contains the fields common to socket addresses for all address
 * families.
 */
struct sockaddr {
	/** Socket address family
	 *
	 * This is an AF_XXX constant.
	 */
        sa_family_t sa_family;
	/** Padding
	 *
	 * This ensures that a struct @c sockaddr_tcpip is large
	 * enough to hold a socket address for any TCP/IP address
	 * family.
	 */
	char pad[ SA_LEN - sizeof ( sa_family_t ) ];
};

#endif /* _GPXE_SOCKET_H */
