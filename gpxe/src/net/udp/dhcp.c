/*
 * Copyright (C) 2006 Michael Brown <mbrown@fensystems.co.uk>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <byteswap.h>
#include <gpxe/if_ether.h>
#include <gpxe/netdevice.h>
#include <gpxe/device.h>
#include <gpxe/xfer.h>
#include <gpxe/open.h>
#include <gpxe/job.h>
#include <gpxe/retry.h>
#include <gpxe/tcpip.h>
#include <gpxe/ip.h>
#include <gpxe/uuid.h>
#include <gpxe/dhcp.h>
#include <gpxe/timer.h>
#include <gpxe/settings.h>
#include <gpxe/dhcp.h>
#include <gpxe/dhcpopts.h>
#include <gpxe/dhcppkt.h>

/** @file
 *
 * Dynamic Host Configuration Protocol
 *
 */

/**
 * DHCP operation types
 *
 * This table maps from DHCP message types (i.e. values of the @c
 * DHCP_MESSAGE_TYPE option) to values of the "op" field within a DHCP
 * packet.
 */
static const uint8_t dhcp_op[] = {
	[DHCPDISCOVER]	= BOOTP_REQUEST,
	[DHCPOFFER]	= BOOTP_REPLY,
	[DHCPREQUEST]	= BOOTP_REQUEST,
	[DHCPDECLINE]	= BOOTP_REQUEST,
	[DHCPACK]	= BOOTP_REPLY,
	[DHCPNAK]	= BOOTP_REPLY,
	[DHCPRELEASE]	= BOOTP_REQUEST,
	[DHCPINFORM]	= BOOTP_REQUEST,
};

/** Raw option data for options common to all DHCP requests */
static uint8_t dhcp_request_options_data[] = {
	DHCP_MAX_MESSAGE_SIZE, DHCP_WORD ( ETH_MAX_MTU ),
	DHCP_VENDOR_CLASS_ID,
	DHCP_STRING (  'P', 'X', 'E', 'C', 'l', 'i', 'e', 'n', 't', ':',
		       'A', 'r', 'c', 'h', ':', '0', '0', '0', '0', '0', ':',
		       'U', 'N', 'D', 'I', ':', '0', '0', '2', '0', '0', '1' ),
	DHCP_CLIENT_ARCHITECTURE, DHCP_WORD ( 0 ),
	DHCP_CLIENT_NDI, DHCP_OPTION ( 1 /* UNDI */ , 2, 1 /* v2.1 */ ),
	DHCP_PARAMETER_REQUEST_LIST,
	DHCP_OPTION ( DHCP_SUBNET_MASK, DHCP_ROUTERS, DHCP_DNS_SERVERS,
		      DHCP_LOG_SERVERS, DHCP_HOST_NAME, DHCP_DOMAIN_NAME,
		      DHCP_ROOT_PATH, DHCP_VENDOR_ENCAP, DHCP_VENDOR_CLASS_ID,
		      DHCP_TFTP_SERVER_NAME, DHCP_BOOTFILE_NAME,
		      DHCP_EB_ENCAP, DHCP_ISCSI_INITIATOR_IQN ),
	DHCP_END
};

/** Options common to all DHCP requests */
static struct dhcp_options dhcp_request_options = {
	.data = dhcp_request_options_data,
	.max_len = sizeof ( dhcp_request_options_data ),
	.len = sizeof ( dhcp_request_options_data ),
};

/** DHCP feature codes */
static uint8_t dhcp_features[0] __table_start ( uint8_t, dhcp_features );
static uint8_t dhcp_features_end[0] __table_end ( uint8_t, dhcp_features );

/**
 * Name a DHCP packet type
 *
 * @v msgtype		DHCP message type
 * @ret string		DHCP mesasge type name
 */
static inline const char * dhcp_msgtype_name ( unsigned int msgtype ) {
	switch ( msgtype ) {
	case 0:			return "BOOTP"; /* Non-DHCP packet */
	case DHCPDISCOVER:	return "DHCPDISCOVER";
	case DHCPOFFER:		return "DHCPOFFER";
	case DHCPREQUEST:	return "DHCPREQUEST";
	case DHCPDECLINE:	return "DHCPDECLINE";
	case DHCPACK:		return "DHCPACK";
	case DHCPNAK:		return "DHCPNAK";
	case DHCPRELEASE:	return "DHCPRELEASE";
	case DHCPINFORM:	return "DHCPINFORM";
	default:		return "DHCP<invalid>";
	}
}

/**
 * Calculate DHCP transaction ID for a network device
 *
 * @v netdev		Network device
 * @ret xid		DHCP XID
 *
 * Extract the least significant bits of the hardware address for use
 * as the transaction ID.
 */
static uint32_t dhcp_xid ( struct net_device *netdev ) {
	uint32_t xid;

	memcpy ( &xid, ( netdev->ll_addr + netdev->ll_protocol->ll_addr_len
			 - sizeof ( xid ) ), sizeof ( xid ) );
	return xid;
}

/**
 * Create a DHCP packet
 *
 * @v dhcppkt		DHCP packet structure to fill in
 * @v netdev		Network device
 * @v msgtype		DHCP message type
 * @v options		Initial options to include (or NULL)
 * @v data		Buffer for DHCP packet
 * @v max_len		Size of DHCP packet buffer
 * @ret rc		Return status code
 *
 * Creates a DHCP packet in the specified buffer, and fills out a @c
 * dhcp_packet structure that can be passed to
 * set_dhcp_packet_option() or copy_dhcp_packet_options().
 */
int create_dhcp_packet ( struct dhcp_packet *dhcppkt,
			 struct net_device *netdev, uint8_t msgtype,
			 struct dhcp_options *options, 
			 void *data, size_t max_len ) {
	struct dhcphdr *dhcphdr = data;
	size_t options_len;
	unsigned int hlen;
	int rc;

	/* Sanity check */
	options_len = ( options ? options->len : 0 );
	if ( max_len < ( sizeof ( *dhcphdr ) + options_len ) )
		return -ENOSPC;

	/* Initialise DHCP packet content */
	memset ( dhcphdr, 0, max_len );
	dhcphdr->xid = dhcp_xid ( netdev );
	dhcphdr->magic = htonl ( DHCP_MAGIC_COOKIE );
	dhcphdr->htype = ntohs ( netdev->ll_protocol->ll_proto );
	dhcphdr->op = dhcp_op[msgtype];
	/* If hardware length exceeds the chaddr field length, don't
	 * use the chaddr field.  This is as per RFC4390.
	 */
	hlen = netdev->ll_protocol->ll_addr_len;
	if ( hlen > sizeof ( dhcphdr->chaddr ) ) {
		hlen = 0;
		dhcphdr->flags = htons ( BOOTP_FL_BROADCAST );
	}
	dhcphdr->hlen = hlen;
	memcpy ( dhcphdr->chaddr, netdev->ll_addr, hlen );
	memcpy ( dhcphdr->options, options->data, options_len );

	/* Initialise DHCP packet structure */
	memset ( dhcppkt, 0, sizeof ( *dhcppkt ) );
	dhcppkt_init ( dhcppkt, data, max_len );
	
	/* Set DHCP_MESSAGE_TYPE option */
	if ( ( rc = dhcppkt_store ( dhcppkt, DHCP_MESSAGE_TYPE,
				    &msgtype, sizeof ( msgtype ) ) ) != 0 )
		return rc;

	return 0;
}

/** DHCP network device descriptor */
struct dhcp_netdev_desc {
	/** Bus type ID */
	uint8_t type;
	/** Vendor ID */
	uint16_t vendor;
	/** Device ID */
	uint16_t device;
} __attribute__ (( packed ));

/** DHCP client identifier */
struct dhcp_client_id {
	/** Link-layer protocol */
	uint8_t ll_proto;
	/** Link-layer address */
	uint8_t ll_addr[MAX_LL_ADDR_LEN];
} __attribute__ (( packed ));

/** DHCP client UUID */
struct dhcp_client_uuid {
	/** Identifier type */
	uint8_t type;
	/** UUID */
	union uuid uuid;
} __attribute__ (( packed ));

#define DHCP_CLIENT_UUID_TYPE 0

/**
 * Create DHCP request packet
 *
 * @v dhcppkt		DHCP packet structure to fill in
 * @v netdev		Network device
 * @v dhcpoffer		DHCPOFFER packet received from server
 * @v data		Buffer for DHCP packet
 * @v max_len		Size of DHCP packet buffer
 * @ret rc		Return status code
 */
int create_dhcp_request ( struct dhcp_packet *dhcppkt,
			  struct net_device *netdev,
			  struct dhcp_packet *dhcpoffer,
			  void *data, size_t max_len ) {
	struct device_description *desc = &netdev->dev->desc;
	struct dhcp_netdev_desc dhcp_desc;
	struct dhcp_client_id client_id;
	struct dhcp_client_uuid client_uuid;
	unsigned int msgtype;
	size_t dhcp_features_len;
	size_t ll_addr_len;
	int rc;

	/* Create DHCP packet */
	msgtype = ( dhcpoffer ? DHCPREQUEST : DHCPDISCOVER );
	if ( ( rc = create_dhcp_packet ( dhcppkt, netdev, msgtype,
					 &dhcp_request_options, data,
					 max_len ) ) != 0 ) {
		DBG ( "DHCP could not create DHCP packet: %s\n",
		      strerror ( rc ) );
		return rc;
	}

	/* Copy any required options from previous server repsonse */
	if ( dhcpoffer ) {
		struct in_addr server_id;
		struct in_addr requested_ip;

		if ( dhcppkt_fetch ( dhcpoffer, DHCP_SERVER_IDENTIFIER,
				     &server_id, sizeof ( server_id ) )
		     != sizeof ( server_id ) ) {
			DBG ( "DHCP offer missing server identifier\n" );
			return -EINVAL;
		}
		if ( dhcppkt_fetch ( dhcpoffer, DHCP_EB_YIADDR,
				     &requested_ip, sizeof ( requested_ip ) )
		     != sizeof ( requested_ip ) ) {
			DBG ( "DHCP offer missing IP address\n" );
			return -EINVAL;
		}
		if ( ( rc = dhcppkt_store ( dhcppkt, DHCP_SERVER_IDENTIFIER,
					    &server_id,
					    sizeof ( server_id ) ) ) != 0 ) {
			DBG ( "DHCP could not set server identifier: %s\n ",
			      strerror ( rc ) );
			return rc;
		}
		if ( ( rc = dhcppkt_store ( dhcppkt, DHCP_REQUESTED_ADDRESS,
					    &requested_ip,
					    sizeof ( requested_ip ) ) ) != 0 ){
			DBG ( "DHCP could not set requested address: %s\n",
			      strerror ( rc ) );
			return rc;
		}
	}

	/* Add options to identify the feature list */
	dhcp_features_len = ( dhcp_features_end - dhcp_features );
	if ( ( rc = dhcppkt_store ( dhcppkt, DHCP_EB_ENCAP, dhcp_features,
				    dhcp_features_len ) ) != 0 ) {
		DBG ( "DHCP could not set features list option: %s\n",
		      strerror ( rc ) );
		return rc;
	}

	/* Add options to identify the network device */
	dhcp_desc.type = desc->bus_type;
	dhcp_desc.vendor = htons ( desc->vendor );
	dhcp_desc.device = htons ( desc->device );
	if ( ( rc = dhcppkt_store ( dhcppkt, DHCP_EB_BUS_ID, &dhcp_desc,
				    sizeof ( dhcp_desc ) ) ) != 0 ) {
		DBG ( "DHCP could not set bus ID option: %s\n",
		      strerror ( rc ) );
		return rc;
	}

	/* Add DHCP client identifier.  Required for Infiniband, and
	 * doesn't hurt other link layers.
	 */
	client_id.ll_proto = ntohs ( netdev->ll_protocol->ll_proto );
	ll_addr_len = netdev->ll_protocol->ll_addr_len;
	assert ( ll_addr_len <= sizeof ( client_id.ll_addr ) );
	memcpy ( client_id.ll_addr, netdev->ll_addr, ll_addr_len );
	if ( ( rc = dhcppkt_store ( dhcppkt, DHCP_CLIENT_ID, &client_id,
				    ( ll_addr_len + 1 ) ) ) != 0 ) {
		DBG ( "DHCP could not set client ID: %s\n",
		      strerror ( rc ) );
		return rc;
	}

	/* Add client UUID, if we have one.  Required for PXE. */
	client_uuid.type = DHCP_CLIENT_UUID_TYPE;
	if ( ( rc = get_uuid ( &client_uuid.uuid ) ) == 0 ) {
		if ( ( rc = dhcppkt_store ( dhcppkt, DHCP_CLIENT_UUID,
					    &client_uuid,
					    sizeof ( client_uuid ) ) ) != 0 ) {
			DBG ( "DHCP could not set client UUID: %s\n",
			      strerror ( rc ) );
			return rc;
		}
	}

	return 0;
}

/****************************************************************************
 *
 * DHCP settings
 *
 */

/** A DHCP settings block */
struct dhcp_settings {
	/** Reference counter */
	struct refcnt refcnt;
	/** DHCP packet */
	struct dhcp_packet dhcppkt;
	/** Setting interface */
	struct settings settings;
};

/**
 * Decrement reference count on DHCP settings block
 *
 * @v dhcpset		DHCP settings block
 */
static inline void dhcpset_put ( struct dhcp_settings *dhcpset ) {
	ref_put ( &dhcpset->refcnt );
}

/**
 * Store value of DHCP setting
 *
 * @v settings		Settings block
 * @v setting		Setting to store
 * @v data		Setting data, or NULL to clear setting
 * @v len		Length of setting data
 * @ret rc		Return status code
 */
static int dhcpset_store ( struct settings *settings, struct setting *setting,
			   const void *data, size_t len ) {
	struct dhcp_settings *dhcpset =
		container_of ( settings, struct dhcp_settings, settings );

	return dhcppkt_store ( &dhcpset->dhcppkt, setting->tag, data, len );
}

/**
 * Fetch value of setting
 *
 * @v settings		Settings block, or NULL to search all blocks
 * @v setting		Setting to fetch
 * @v data		Buffer to fill with setting data
 * @v len		Length of buffer
 * @ret len		Length of setting data, or negative error
 */
static int dhcpset_fetch ( struct settings *settings, struct setting *setting,
			   void *data, size_t len ) {
	struct dhcp_settings *dhcpset =
		container_of ( settings, struct dhcp_settings, settings );

	return dhcppkt_fetch ( &dhcpset->dhcppkt, setting->tag, data, len );
}

/** DHCP settings operations */
static struct settings_operations dhcpset_settings_operations = {
	.store = dhcpset_store,
	.fetch = dhcpset_fetch,
};

/**
 * Create DHCP setting block
 *
 * @v dhcphdr		DHCP packet
 * @v len		Length of DHCP packet
 * @ret dhcpset		DHCP settings block
 */
static struct dhcp_settings * dhcpset_create ( const struct dhcphdr *dhcphdr,
					       size_t len ) {
	struct dhcp_settings *dhcpset;
	void *data;

	dhcpset = zalloc ( sizeof ( *dhcpset ) + len );
	if ( dhcpset ) {
		data = ( ( ( void * ) dhcpset ) + sizeof ( *dhcpset ) );
		memcpy ( data, dhcphdr, len );
		dhcppkt_init ( &dhcpset->dhcppkt, data, len );
		settings_init ( &dhcpset->settings,
				&dhcpset_settings_operations, &dhcpset->refcnt,
				DHCP_SETTINGS_NAME );
	}
	return dhcpset;
}

/****************************************************************************
 *
 * DHCP to UDP interface
 *
 */

/** A DHCP session */
struct dhcp_session {
	/** Reference counter */
	struct refcnt refcnt;
	/** Job control interface */
	struct job_interface job;
	/** Data transfer interface */
	struct xfer_interface xfer;

	/** Network device being configured */
	struct net_device *netdev;

	/** State of the session
	 *
	 * This is a value for the @c DHCP_MESSAGE_TYPE option
	 * (e.g. @c DHCPDISCOVER).
	 */
	int state;
	/** Response obtained from DHCP server */
	struct dhcp_settings *response;
	/** Response obtained from ProxyDHCP server */
	struct dhcp_settings *proxy_response;
	/** Retransmission timer */
	struct retry_timer timer;
	/** Session start time (in ticks) */
	unsigned long start;
};

/**
 * Free DHCP session
 *
 * @v refcnt		Reference counter
 */
static void dhcp_free ( struct refcnt *refcnt ) {
	struct dhcp_session *dhcp =
		container_of ( refcnt, struct dhcp_session, refcnt );

	netdev_put ( dhcp->netdev );
	dhcpset_put ( dhcp->response );
	dhcpset_put ( dhcp->proxy_response );
	free ( dhcp );
}

/**
 * Mark DHCP session as complete
 *
 * @v dhcp		DHCP session
 * @v rc		Return status code
 */
static void dhcp_finished ( struct dhcp_session *dhcp, int rc ) {

	/* Block futher incoming messages */
	job_nullify ( &dhcp->job );
	xfer_nullify ( &dhcp->xfer );

	/* Stop retry timer */
	stop_timer ( &dhcp->timer );

	/* Free resources and close interfaces */
	xfer_close ( &dhcp->xfer, rc );
	job_done ( &dhcp->job, rc );
}

/**
 * Register options received via DHCP
 *
 * @v dhcp		DHCP session
 * @ret rc		Return status code
 */
static int dhcp_register_settings ( struct dhcp_session *dhcp ) {
	struct settings *old_settings;
	struct settings *settings;
	struct settings *parent;
	int rc;

	/* Register ProxyDHCP settings, if present */
	if ( dhcp->proxy_response ) {
		settings = &dhcp->proxy_response->settings;
		settings->name = PROXYDHCP_SETTINGS_NAME;
		old_settings = find_settings ( settings->name );
		if ( old_settings )
			unregister_settings ( old_settings );
		if ( ( rc = register_settings ( settings, NULL ) ) != 0 )
			return rc;
	}

	/* Register DHCP settings */
	parent = netdev_settings ( dhcp->netdev );
	settings = &dhcp->response->settings;
	old_settings = find_child_settings ( parent, settings->name );
	if ( old_settings )
		unregister_settings ( old_settings );
	if ( ( rc = register_settings ( settings, parent ) ) != 0 )
		return rc;

	return 0;
}

/****************************************************************************
 *
 * Data transfer interface
 *
 */

/**
 * Transmit DHCP request
 *
 * @v dhcp		DHCP session
 * @ret rc		Return status code
 */
static int dhcp_send_request ( struct dhcp_session *dhcp ) {
	struct xfer_metadata meta = {
		.netdev = dhcp->netdev,
	};
	struct io_buffer *iobuf;
	struct dhcp_packet *dhcpoffer = NULL;
	struct dhcp_packet dhcppkt;
	int rc;
	
	DBGC ( dhcp, "DHCP %p transmitting %s\n",
	       dhcp, dhcp_msgtype_name ( dhcp->state ) );

	assert ( ( dhcp->state == DHCPDISCOVER ) ||
		 ( dhcp->state == DHCPREQUEST ) );

	/* Start retry timer.  Do this first so that failures to
	 * transmit will be retried.
	 */
	start_timer ( &dhcp->timer );

	/* Allocate buffer for packet */
	iobuf = xfer_alloc_iob ( &dhcp->xfer, DHCP_MIN_LEN );
	if ( ! iobuf )
		return -ENOMEM;

	/* Create DHCP packet in temporary buffer */
	if ( dhcp->state == DHCPREQUEST ) {
		assert ( dhcp->response );
		dhcpoffer = &dhcp->response->dhcppkt;
	}
	if ( ( rc = create_dhcp_request ( &dhcppkt, dhcp->netdev,
					  dhcpoffer, iobuf->data,
					  iob_tailroom ( iobuf ) ) ) != 0 ) {
		DBGC ( dhcp, "DHCP %p could not construct DHCP request: %s\n",
		       dhcp, strerror ( rc ) );
		goto done;
	}

	/* Transmit the packet */
	iob_put ( iobuf, dhcppkt.len );
	rc = xfer_deliver_iob_meta ( &dhcp->xfer, iobuf, &meta );
	iobuf = NULL;
	if ( rc != 0 ) {
		DBGC ( dhcp, "DHCP %p could not transmit UDP packet: %s\n",
		       dhcp, strerror ( rc ) );
		goto done;
	}

 done:
	free_iob ( iobuf );
	return rc;
}

/**
 * Handle DHCP retry timer expiry
 *
 * @v timer		DHCP retry timer
 * @v fail		Failure indicator
 */
static void dhcp_timer_expired ( struct retry_timer *timer, int fail ) {
	struct dhcp_session *dhcp =
		container_of ( timer, struct dhcp_session, timer );

	if ( fail ) {
		dhcp_finished ( dhcp, -ETIMEDOUT );
	} else {
		dhcp_send_request ( dhcp );
	}
}

/**
 * Receive new data
 *
 * @v xfer 		Data transfer interface
 * @v iobuf		I/O buffer
 * @v data		Received data
 * @v len		Length of received data
 * @ret rc		Return status code
 */
static int dhcp_deliver_raw ( struct xfer_interface *xfer,
			      const void *data, size_t len ) {
	struct dhcp_session *dhcp =
		container_of ( xfer, struct dhcp_session, xfer );
	struct dhcp_settings *response;
	struct dhcp_settings **store_response;
	struct dhcphdr *dhcphdr;
	uint8_t msgtype = 0;
	uint8_t priority = 0;
	uint8_t existing_priority = 0;
	unsigned long elapsed;
	int is_proxy;
	uint8_t ignore_proxy = 0;
	int rc;

	/* Convert packet into a DHCP settings block */
	response = dhcpset_create ( data, len );
	if ( ! response ) {
		DBGC ( dhcp, "DHCP %p could not store DHCP packet\n", dhcp );
		return -ENOMEM;
	}
	dhcphdr = response->dhcppkt.dhcphdr;

	/* Check for matching transaction ID */
	if ( dhcphdr->xid != dhcp_xid ( dhcp->netdev ) ) {
		DBGC ( dhcp, "DHCP %p wrong transaction ID (wanted %08lx, "
			"got %08lx)\n", dhcp, ntohl ( dhcphdr->xid ),
			ntohl ( dhcp_xid ( dhcp->netdev ) ) );
		goto out_discard;
	};	

	/* Determine and verify message type */
	is_proxy = ( dhcphdr->yiaddr.s_addr == 0 );
	dhcppkt_fetch ( &response->dhcppkt, DHCP_MESSAGE_TYPE, &msgtype,
			sizeof ( msgtype ) );
	DBGC ( dhcp, "DHCP %p received %s%s\n", dhcp,
	       ( is_proxy ? "Proxy" : "" ), dhcp_msgtype_name ( msgtype ) );
	if ( ( ( dhcp->state != DHCPDISCOVER ) || ( msgtype != DHCPOFFER ) ) &&
	     ( ( dhcp->state != DHCPREQUEST ) || ( msgtype != DHCPACK ) ) ) {
		DBGC ( dhcp, "DHCP %p discarding %s while in %s state\n",
		       dhcp, dhcp_msgtype_name ( msgtype ),
		       dhcp_msgtype_name ( dhcp->state ) );
		goto out_discard;
	}

	/* Update stored standard/ProxyDHCP options, if the new
	 * options have equal or higher priority than the
	 * currently-stored options.
	 */
	store_response = ( is_proxy ? &dhcp->proxy_response : &dhcp->response);
	if ( *store_response ) {
		dhcppkt_fetch ( &(*store_response)->dhcppkt, DHCP_EB_PRIORITY,
				&existing_priority,
				sizeof ( existing_priority ) );
	}
	dhcppkt_fetch ( &response->dhcppkt, DHCP_EB_PRIORITY, &priority,
			sizeof ( priority ) );
	if ( priority >= existing_priority ) {
		dhcpset_put ( *store_response );
		*store_response = response;
	} else {
		dhcpset_put ( response );
	}

	/* If we don't yet have a standard DHCP response (i.e. one
	 * with an IP address), then just leave the timer running.
	 */
	if ( ! dhcp->response )
		goto out;

	/* Handle DHCP response */
	dhcppkt_fetch ( &dhcp->response->dhcppkt, DHCP_EB_NO_PROXYDHCP,
			&ignore_proxy, sizeof ( ignore_proxy ) );
	switch ( dhcp->state ) {
	case DHCPDISCOVER:
		/* If we have allowed sufficient time for ProxyDHCP
		 * reponses, then transition to making the DHCPREQUEST.
		 */
		elapsed = ( currticks() - dhcp->start );
		if ( ignore_proxy || ( elapsed > PROXYDHCP_WAIT_TIME ) ) {
			stop_timer ( &dhcp->timer );
			dhcp->state = DHCPREQUEST;
			dhcp_send_request ( dhcp );
		}
		break;
	case DHCPREQUEST:
		/* DHCP finished; register options and exit */
		if ( ignore_proxy && dhcp->proxy_response ) {
			dhcpset_put ( dhcp->proxy_response );
			dhcp->proxy_response = NULL;
		}
		if ( ( rc = dhcp_register_settings ( dhcp ) ) != 0 ) {
			dhcp_finished ( dhcp, rc );
			break;
		}
		dhcp_finished ( dhcp, 0 );
		break;
	default:
		assert ( 0 );
	}

 out:
	return 0;

 out_discard:
	dhcpset_put ( response );
	return 0;
}

/** DHCP data transfer interface operations */
static struct xfer_interface_operations dhcp_xfer_operations = {
	.close		= ignore_xfer_close,
	.vredirect	= xfer_vopen,
	.window		= unlimited_xfer_window,
	.alloc_iob	= default_xfer_alloc_iob,
	.deliver_iob	= xfer_deliver_as_raw,
	.deliver_raw	= dhcp_deliver_raw,
};

/****************************************************************************
 *
 * Job control interface
 *
 */

/**
 * Handle kill() event received via job control interface
 *
 * @v job		DHCP job control interface
 */
static void dhcp_job_kill ( struct job_interface *job ) {
	struct dhcp_session *dhcp =
		container_of ( job, struct dhcp_session, job );

	/* Terminate DHCP session */
	dhcp_finished ( dhcp, -ECANCELED );
}

/** DHCP job control interface operations */
static struct job_interface_operations dhcp_job_operations = {
	.done		= ignore_job_done,
	.kill		= dhcp_job_kill,
	.progress	= ignore_job_progress,
};

/****************************************************************************
 *
 * Instantiator
 *
 */

/**
 * Start DHCP on a network device
 *
 * @v job		Job control interface
 * @v netdev		Network device
 * @v register_options	DHCP option block registration routine
 * @ret rc		Return status code
 *
 * Starts DHCP on the specified network device.  If successful, the @c
 * register_options() routine will be called with the acquired
 * options.
 */
int start_dhcp ( struct job_interface *job, struct net_device *netdev ) {
	static struct sockaddr_in server = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = INADDR_BROADCAST,
		.sin_port = htons ( BOOTPS_PORT ),
	};
	static struct sockaddr_in client = {
		.sin_family = AF_INET,
		.sin_port = htons ( BOOTPC_PORT ),
	};
	struct dhcp_session *dhcp;
	int rc;

	/* Allocate and initialise structure */
	dhcp = zalloc ( sizeof ( *dhcp ) );
	if ( ! dhcp )
		return -ENOMEM;
	dhcp->refcnt.free = dhcp_free;
	job_init ( &dhcp->job, &dhcp_job_operations, &dhcp->refcnt );
	xfer_init ( &dhcp->xfer, &dhcp_xfer_operations, &dhcp->refcnt );
	dhcp->netdev = netdev_get ( netdev );
	dhcp->timer.expired = dhcp_timer_expired;
	dhcp->state = DHCPDISCOVER;
	dhcp->start = currticks();

	/* Instantiate child objects and attach to our interfaces */
	if ( ( rc = xfer_open_socket ( &dhcp->xfer, SOCK_DGRAM,
				       ( struct sockaddr * ) &server,
				       ( struct sockaddr * ) &client ) ) != 0 )
		goto err;

	/* Start timer to initiate initial DHCPREQUEST */
	start_timer_nodelay ( &dhcp->timer );

	/* Attach parent interface, mortalise self, and return */
	job_plug_plug ( &dhcp->job, job );
	ref_put ( &dhcp->refcnt );
	return 0;

 err:
	dhcp_finished ( dhcp, rc );
	ref_put ( &dhcp->refcnt );
	return rc;
}
