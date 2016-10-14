/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* nm-ndisc.c - Perform IPv6 neighbor discovery
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright (C) 2013 Red Hat, Inc.
 */

#include "nm-default.h"

#include "nm-ndisc.h"

#include <stdlib.h>
#include <arpa/inet.h>
#include <string.h>

#include "nm-setting-ip6-config.h"

#include "nm-ndisc-private.h"
#include "nm-utils.h"
#include "nm-platform.h"
#include "nmp-netns.h"

#define _NMLOG_PREFIX_NAME                "ndisc"

/*****************************************************************************/

struct _NMNDiscPrivate {
	/* this *must* be the first field. */
	NMNDiscDataInternal rdata;

	gint32 solicitations_left;
	guint send_rs_id;
	gint32 last_rs;
	guint ra_timeout_id;  /* first RA timeout */
	guint timeout_id;   /* prefix/dns/etc lifetime timeout */
	char *last_send_rs_error;
	NMUtilsIPv6IfaceId iid;

	/* immutable values: */
	int ifindex;
	char *ifname;
	char *network_id;
	NMSettingIP6ConfigAddrGenMode addr_gen_mode;
	NMUtilsStableType stable_type;
	gint32 max_addresses;
	gint32 router_solicitations;
	gint32 router_solicitation_interval;

	NMPlatform *platform;
	NMPNetns *netns;
};

typedef struct _NMNDiscPrivate NMNDiscPrivate;

NM_GOBJECT_PROPERTIES_DEFINE_BASE (
	PROP_PLATFORM,
	PROP_IFINDEX,
	PROP_IFNAME,
	PROP_STABLE_TYPE,
	PROP_NETWORK_ID,
	PROP_ADDR_GEN_MODE,
	PROP_MAX_ADDRESSES,
	PROP_ROUTER_SOLICITATIONS,
	PROP_ROUTER_SOLICITATION_INTERVAL,
);

enum {
	CONFIG_CHANGED,
	RA_TIMEOUT,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE (NMNDisc, nm_ndisc, G_TYPE_OBJECT)

#define NM_NDISC_GET_PRIVATE(self) _NM_GET_PRIVATE_PTR(self, NMNDisc, NM_IS_NDISC)

/*****************************************************************************/

static void _config_changed_log (NMNDisc *ndisc, NMNDiscConfigMap changed);

/*****************************************************************************/

NMPNetns *
nm_ndisc_netns_get (NMNDisc *self)
{
	g_return_val_if_fail (NM_IS_NDISC (self), NULL);

	return NM_NDISC_GET_PRIVATE (self)->netns;
}

gboolean
nm_ndisc_netns_push (NMNDisc *self, NMPNetns **netns)
{
	NMNDiscPrivate *priv;

	g_return_val_if_fail (NM_IS_NDISC (self), FALSE);

	priv = NM_NDISC_GET_PRIVATE (self);
	if (   priv->netns
	    && !nmp_netns_push (priv->netns)) {
		NM_SET_OUT (netns, NULL);
		return FALSE;
	}

	NM_SET_OUT (netns, priv->netns);
	return TRUE;
}

/*****************************************************************************/

int
nm_ndisc_get_ifindex (NMNDisc *self)
{
	g_return_val_if_fail (NM_IS_NDISC (self), 0);

	return NM_NDISC_GET_PRIVATE (self)->ifindex;
}

const char *
nm_ndisc_get_ifname (NMNDisc *self)
{
	g_return_val_if_fail (NM_IS_NDISC (self), NULL);

	return NM_NDISC_GET_PRIVATE (self)->ifname;
}

/*****************************************************************************/

static const NMNDiscData *
_data_complete (NMNDiscDataInternal *data)
{
#define _SET(data, field) \
	G_STMT_START { \
		if ((data->public.field##_n = data->field->len) > 0) \
			data->public.field = (gpointer) data->field->data; \
		else \
			data->public.field = NULL; \
	} G_STMT_END
	_SET (data, gateways);
	_SET (data, addresses);
	_SET (data, routes);
	_SET (data, dns_servers);
	_SET (data, dns_domains);
#undef _SET
	return &data->public;
}

static void
_emit_config_change (NMNDisc *self, NMNDiscConfigMap changed)
{
	_config_changed_log (self, changed);
	g_signal_emit (self, signals[CONFIG_CHANGED], 0,
	               _data_complete (&NM_NDISC_GET_PRIVATE (self)->rdata),
	               (guint) changed);
}

/*****************************************************************************/

gboolean
nm_ndisc_add_gateway (NMNDisc *ndisc, const NMNDiscGateway *new)
{
	NMNDiscDataInternal *rdata = &NM_NDISC_GET_PRIVATE(ndisc)->rdata;
	int i, insert_idx = -1;

	for (i = 0; i < rdata->gateways->len; i++) {
		NMNDiscGateway *item = &g_array_index (rdata->gateways, NMNDiscGateway, i);

		if (IN6_ARE_ADDR_EQUAL (&item->address, &new->address)) {
			if (new->lifetime == 0) {
				g_array_remove_index (rdata->gateways, i--);
				return TRUE;
			}

			if (item->preference != new->preference) {
				g_array_remove_index (rdata->gateways, i--);
				continue;
			}

			memcpy (item, new, sizeof (*new));
			return FALSE;
		}

		/* Put before less preferable gateways. */
		if (item->preference < new->preference && insert_idx < 0)
			insert_idx = i;
	}

	if (new->lifetime)
		g_array_insert_val (rdata->gateways, MAX (insert_idx, 0), *new);
	return !!new->lifetime;
}

/**
 * complete_address:
 * @ndisc: the #NMNDisc
 * @addr: the #NMNDiscAddress
 *
 * Adds the host part to the address that has network part set.
 * If the address already has a host part, add a different host part
 * if possible (this is useful in case DAD failed).
 *
 * Can fail if a different address can not be generated (DAD failure
 * for an EUI-64 address or DAD counter overflow).
 *
 * Returns: %TRUE if the address could be completed, %FALSE otherwise.
 **/
static gboolean
complete_address (NMNDisc *ndisc, NMNDiscAddress *addr)
{
	NMNDiscPrivate *priv;
	GError *error = NULL;

	g_return_val_if_fail (NM_IS_NDISC (ndisc), FALSE);

	priv = NM_NDISC_GET_PRIVATE (ndisc);
	if (priv->addr_gen_mode == NM_SETTING_IP6_CONFIG_ADDR_GEN_MODE_STABLE_PRIVACY) {
		if (!nm_utils_ipv6_addr_set_stable_privacy (priv->stable_type,
		                                            &addr->address,
		                                            priv->ifname,
		                                            priv->network_id,
		                                            addr->dad_counter++,
		                                            &error)) {
			_LOGW ("complete-address: failed to generate an stable-privacy address: %s",
			       error->message);
			g_clear_error (&error);
			return FALSE;
		}
		_LOGD ("complete-address: using an stable-privacy address");
		return TRUE;
	}

	if (!priv->iid.id) {
		_LOGW ("complete-address: can't generate an EUI-64 address: no interface identifier");
		return FALSE;
	}

	if (addr->address.s6_addr32[2] == 0x0 && addr->address.s6_addr32[3] == 0x0) {
		_LOGD ("complete-address: adding an EUI-64 address");
		nm_utils_ipv6_addr_set_interface_identifier (&addr->address, priv->iid);
		return TRUE;
	}

	_LOGW ("complete-address: can't generate a new EUI-64 address");
	return FALSE;
}

gboolean
nm_ndisc_complete_and_add_address (NMNDisc *ndisc, NMNDiscAddress *new)
{
	NMNDiscPrivate *priv;
	NMNDiscDataInternal *rdata;
	int i;

	if (!complete_address (ndisc, new))
		return FALSE;

	priv = NM_NDISC_GET_PRIVATE (ndisc);
	rdata = &priv->rdata;

	for (i = 0; i < rdata->addresses->len; i++) {
		NMNDiscAddress *item = &g_array_index (rdata->addresses, NMNDiscAddress, i);

		if (IN6_ARE_ADDR_EQUAL (&item->address, &new->address)) {
			gboolean changed;

			if (new->lifetime == 0) {
				g_array_remove_index (rdata->addresses, i--);
				return TRUE;
			}

			changed = item->timestamp + item->lifetime  != new->timestamp + new->lifetime ||
			          item->timestamp + item->preferred != new->timestamp + new->preferred;
			*item = *new;
			return changed;
		}
	}

	/* we create at most max_addresses autoconf addresses. This is different from
	 * what the kernel does, because it considers *all* addresses (including
	 * static and other temporary addresses).
	 **/
	if (priv->max_addresses && rdata->addresses->len >= priv->max_addresses)
		return FALSE;

	if (new->lifetime)
		g_array_insert_val (rdata->addresses, i, *new);
	return !!new->lifetime;
}

gboolean
nm_ndisc_add_route (NMNDisc *ndisc, const NMNDiscRoute *new)
{
	NMNDiscPrivate *priv;
	NMNDiscDataInternal *rdata;
	int i, insert_idx = -1;

	if (new->plen == 0 || new->plen > 128) {
		/* Only expect non-default routes.  The router has no idea what the
		 * local configuration or user preferences are, so sending routes
		 * with a prefix length of 0 must be ignored by NMNDisc.
		 *
		 * Also, upper layers also don't expect that NMNDisc exposes routes
		 * with a plen or zero or larger then 128.
		 */
		g_return_val_if_reached (FALSE);
	}

	priv = NM_NDISC_GET_PRIVATE (ndisc);
	rdata = &priv->rdata;

	for (i = 0; i < rdata->routes->len; i++) {
		NMNDiscRoute *item = &g_array_index (rdata->routes, NMNDiscRoute, i);

		if (IN6_ARE_ADDR_EQUAL (&item->network, &new->network) && item->plen == new->plen) {
			if (new->lifetime == 0) {
				g_array_remove_index (rdata->routes, i--);
				return TRUE;
			}

			if (item->preference != new->preference) {
				g_array_remove_index (rdata->routes, i--);
				continue;
			}

			memcpy (item, new, sizeof (*new));
			return FALSE;
		}

		/* Put before less preferable routes. */
		if (item->preference < new->preference && insert_idx < 0)
			insert_idx = i;
	}

	if (new->lifetime)
		g_array_insert_val (rdata->routes, CLAMP (insert_idx, 0, G_MAXINT), *new);
	return !!new->lifetime;
}

gboolean
nm_ndisc_add_dns_server (NMNDisc *ndisc, const NMNDiscDNSServer *new)
{
	NMNDiscPrivate *priv;
	NMNDiscDataInternal *rdata;
	int i;

	priv = NM_NDISC_GET_PRIVATE (ndisc);
	rdata = &priv->rdata;

	for (i = 0; i < rdata->dns_servers->len; i++) {
		NMNDiscDNSServer *item = &g_array_index (rdata->dns_servers, NMNDiscDNSServer, i);

		if (IN6_ARE_ADDR_EQUAL (&item->address, &new->address)) {
			if (new->lifetime == 0) {
				g_array_remove_index (rdata->dns_servers, i);
				return TRUE;
			}
			if (item->timestamp != new->timestamp || item->lifetime != new->lifetime) {
				*item = *new;
				return TRUE;
			}
			return FALSE;
		}
	}

	if (new->lifetime)
		g_array_insert_val (rdata->dns_servers, i, *new);
	return !!new->lifetime;
}

/* Copies new->domain if 'new' is added to the dns_domains list */
gboolean
nm_ndisc_add_dns_domain (NMNDisc *ndisc, const NMNDiscDNSDomain *new)
{
	NMNDiscPrivate *priv;
	NMNDiscDataInternal *rdata;
	NMNDiscDNSDomain *item;
	int i;

	priv = NM_NDISC_GET_PRIVATE (ndisc);
	rdata = &priv->rdata;

	for (i = 0; i < rdata->dns_domains->len; i++) {
		item = &g_array_index (rdata->dns_domains, NMNDiscDNSDomain, i);

		if (!g_strcmp0 (item->domain, new->domain)) {
			gboolean changed;

			if (new->lifetime == 0) {
				g_array_remove_index (rdata->dns_domains, i);
				return TRUE;
			}

			changed = (item->timestamp != new->timestamp ||
			           item->lifetime != new->lifetime);
			if (changed) {
				item->timestamp = new->timestamp;
				item->lifetime = new->lifetime;
			}
			return changed;
		}
	}

	if (new->lifetime) {
		g_array_insert_val (rdata->dns_domains, i, *new);
		item = &g_array_index (rdata->dns_domains, NMNDiscDNSDomain, i);
		item->domain = g_strdup (new->domain);
	}
	return !!new->lifetime;
}

/*****************************************************************************/

static gboolean
send_rs_timeout (NMNDisc *ndisc)
{
	nm_auto_pop_netns NMPNetns *netns = NULL;
	NMNDiscClass *klass = NM_NDISC_GET_CLASS (ndisc);
	NMNDiscPrivate *priv = NM_NDISC_GET_PRIVATE (ndisc);
	GError *error = NULL;

	priv->send_rs_id = 0;

	if (!nm_ndisc_netns_push (ndisc, &netns))
		return G_SOURCE_REMOVE;

	if (klass->send_rs (ndisc, &error)) {
		_LOGD ("router solicitation sent");
		priv->solicitations_left--;
		g_clear_pointer (&priv->last_send_rs_error, g_free);
	} else {
		gboolean different_message;

		different_message = g_strcmp0 (priv->last_send_rs_error, error->message) != 0;
		_NMLOG (different_message ? LOGL_WARN : LOGL_DEBUG,
		        "failure sending router solicitation: %s", error->message);
		if (different_message) {
			g_clear_pointer (&priv->last_send_rs_error, g_free);
			priv->last_send_rs_error = g_strdup (error->message);
		}
		g_clear_error (&error);
	}

	priv->last_rs = nm_utils_get_monotonic_timestamp_s ();
	if (priv->solicitations_left > 0) {
		_LOGD ("scheduling router solicitation retry in %d seconds.",
		       (int) priv->router_solicitation_interval);
		priv->send_rs_id = g_timeout_add_seconds (priv->router_solicitation_interval,
		                                          (GSourceFunc) send_rs_timeout, ndisc);
	} else {
		_LOGD ("did not receive a router advertisement after %d solicitations.",
		       (int) priv->router_solicitations);
	}

	return G_SOURCE_REMOVE;
}

static void
solicit (NMNDisc *ndisc)
{
	NMNDiscPrivate *priv = NM_NDISC_GET_PRIVATE (ndisc);
	gint64 next, now;

	if (priv->send_rs_id)
		return;

	now = nm_utils_get_monotonic_timestamp_s ();

	priv->solicitations_left = priv->router_solicitations;

	next = (((gint64) priv->last_rs) + priv->router_solicitation_interval) - now;
	next = CLAMP (next, 0, G_MAXINT32);
	_LOGD ("scheduling explicit router solicitation request in %" G_GINT64_FORMAT " seconds.",
	       next);
	priv->send_rs_id = g_timeout_add_seconds ((guint32) next, (GSourceFunc) send_rs_timeout, ndisc);
}

/*****************************************************************************/

/**
 * nm_ndisc_set_iid:
 * @ndisc: the #NMNDisc
 * @iid: the new interface ID
 *
 * Sets the "Modified EUI-64" interface ID to be used when generating
 * IPv6 addresses using received prefixes. Identifiers are either generated
 * from the hardware addresses or manually set by the operator with
 * "ip token" command.
 *
 * Upon token change (or initial setting) all addresses generated using
 * the old identifier are removed. The caller should ensure the addresses
 * will be reset by soliciting router advertisements.
 *
 * In case the stable privacy addressing is used %FALSE is returned and
 * addresses are left untouched.
 *
 * Returns: %TRUE if addresses need to be regenerated, %FALSE otherwise.
 **/
gboolean
nm_ndisc_set_iid (NMNDisc *ndisc, const NMUtilsIPv6IfaceId iid)
{
	NMNDiscPrivate *priv;
	NMNDiscDataInternal *rdata;

	g_return_val_if_fail (NM_IS_NDISC (ndisc), FALSE);

	priv = NM_NDISC_GET_PRIVATE (ndisc);
	rdata = &priv->rdata;

	if (priv->iid.id != iid.id) {
		priv->iid = iid;

		if (priv->addr_gen_mode == NM_SETTING_IP6_CONFIG_ADDR_GEN_MODE_STABLE_PRIVACY)
			return FALSE;

		if (rdata->addresses->len) {
			_LOGD ("IPv6 interface identifier changed, flushing addresses");
			g_array_remove_range (rdata->addresses, 0, rdata->addresses->len);
			_emit_config_change (ndisc, NM_NDISC_CONFIG_ADDRESSES);
			solicit (ndisc);
		}
		return TRUE;
	}

	return FALSE;
}

static gboolean
ndisc_ra_timeout_cb (gpointer user_data)
{
	NMNDisc *ndisc = NM_NDISC (user_data);

	NM_NDISC_GET_PRIVATE (ndisc)->ra_timeout_id = 0;
	g_signal_emit (ndisc, signals[RA_TIMEOUT], 0);
	return G_SOURCE_REMOVE;
}

void
nm_ndisc_start (NMNDisc *ndisc)
{
	nm_auto_pop_netns NMPNetns *netns = NULL;
	NMNDiscPrivate *priv = NM_NDISC_GET_PRIVATE (ndisc);
	NMNDiscClass *klass = NM_NDISC_GET_CLASS (ndisc);
	gint64 ra_wait_secs;

	g_return_if_fail (klass->start);
	g_return_if_fail (!priv->ra_timeout_id);

	_LOGD ("starting neighbor discovery: %d", priv->ifindex);

	if (!nm_ndisc_netns_push (ndisc, &netns))
		return;

	ra_wait_secs = (((gint64) priv->router_solicitations) * priv->router_solicitation_interval) + 1;
	ra_wait_secs = CLAMP (ra_wait_secs, 30, 120);
	priv->ra_timeout_id = g_timeout_add_seconds (ra_wait_secs, ndisc_ra_timeout_cb, ndisc);
	_LOGD ("scheduling RA timeout in %d seconds", (int) ra_wait_secs);

	klass->start (ndisc);

	solicit (ndisc);
}

void
nm_ndisc_dad_failed (NMNDisc *ndisc, struct in6_addr *address)
{
	NMNDiscDataInternal *rdata;
	int i;
	gboolean changed = FALSE;

	rdata = &NM_NDISC_GET_PRIVATE (ndisc)->rdata;

	for (i = 0; i < rdata->addresses->len; i++) {
		NMNDiscAddress *item = &g_array_index (rdata->addresses, NMNDiscAddress, i);

		if (!IN6_ARE_ADDR_EQUAL (&item->address, address))
			continue;

		_LOGD ("DAD failed for discovered address %s", nm_utils_inet6_ntop (address, NULL));
		if (!complete_address (ndisc, item))
			g_array_remove_index (rdata->addresses, i--);
		changed = TRUE;
	}

	if (changed)
		_emit_config_change (ndisc, NM_NDISC_CONFIG_ADDRESSES);
}

#define CONFIG_MAP_MAX_STR 7

static void
config_map_to_string (NMNDiscConfigMap map, char *p)
{
	if (map & NM_NDISC_CONFIG_DHCP_LEVEL)
		*p++ = 'd';
	if (map & NM_NDISC_CONFIG_GATEWAYS)
		*p++ = 'G';
	if (map & NM_NDISC_CONFIG_ADDRESSES)
		*p++ = 'A';
	if (map & NM_NDISC_CONFIG_ROUTES)
		*p++ = 'R';
	if (map & NM_NDISC_CONFIG_DNS_SERVERS)
		*p++ = 'S';
	if (map & NM_NDISC_CONFIG_DNS_DOMAINS)
		*p++ = 'D';
	*p = '\0';
}

static const char *
dhcp_level_to_string (NMNDiscDHCPLevel dhcp_level)
{
	switch (dhcp_level) {
	case NM_NDISC_DHCP_LEVEL_NONE:
		return "none";
	case NM_NDISC_DHCP_LEVEL_OTHERCONF:
		return "otherconf";
	case NM_NDISC_DHCP_LEVEL_MANAGED:
		return "managed";
	default:
		return "INVALID";
	}
}

#define expiry(item) (item->timestamp + item->lifetime)

static void
_config_changed_log (NMNDisc *ndisc, NMNDiscConfigMap changed)
{
	NMNDiscPrivate *priv;
	NMNDiscDataInternal *rdata;
	int i;
	char changedstr[CONFIG_MAP_MAX_STR];
	char addrstr[INET6_ADDRSTRLEN];

	if (!_LOGD_ENABLED ())
		return;

	priv = NM_NDISC_GET_PRIVATE (ndisc);
	rdata = &priv->rdata;

	config_map_to_string (changed, changedstr);
	_LOGD ("neighbor discovery configuration changed [%s]:", changedstr);
	_LOGD ("  dhcp-level %s", dhcp_level_to_string (priv->rdata.public.dhcp_level));
	for (i = 0; i < rdata->gateways->len; i++) {
		NMNDiscGateway *gateway = &g_array_index (rdata->gateways, NMNDiscGateway, i);

		inet_ntop (AF_INET6, &gateway->address, addrstr, sizeof (addrstr));
		_LOGD ("  gateway %s pref %d exp %u", addrstr, gateway->preference, expiry (gateway));
	}
	for (i = 0; i < rdata->addresses->len; i++) {
		NMNDiscAddress *address = &g_array_index (rdata->addresses, NMNDiscAddress, i);

		inet_ntop (AF_INET6, &address->address, addrstr, sizeof (addrstr));
		_LOGD ("  address %s exp %u", addrstr, expiry (address));
	}
	for (i = 0; i < rdata->routes->len; i++) {
		NMNDiscRoute *route = &g_array_index (rdata->routes, NMNDiscRoute, i);

		inet_ntop (AF_INET6, &route->network, addrstr, sizeof (addrstr));
		_LOGD ("  route %s/%d via %s pref %d exp %u", addrstr, (int) route->plen,
		       nm_utils_inet6_ntop (&route->gateway, NULL), route->preference,
		       expiry (route));
	}
	for (i = 0; i < rdata->dns_servers->len; i++) {
		NMNDiscDNSServer *dns_server = &g_array_index (rdata->dns_servers, NMNDiscDNSServer, i);

		inet_ntop (AF_INET6, &dns_server->address, addrstr, sizeof (addrstr));
		_LOGD ("  dns_server %s exp %u", addrstr, expiry (dns_server));
	}
	for (i = 0; i < rdata->dns_domains->len; i++) {
		NMNDiscDNSDomain *dns_domain = &g_array_index (rdata->dns_domains, NMNDiscDNSDomain, i);

		_LOGD ("  dns_domain %s exp %u", dns_domain->domain, expiry (dns_domain));
	}
}

static void
clean_gateways (NMNDisc *ndisc, guint32 now, NMNDiscConfigMap *changed, guint32 *nextevent)
{
	NMNDiscDataInternal *rdata;
	guint i;

	rdata = &NM_NDISC_GET_PRIVATE (ndisc)->rdata;

	for (i = 0; i < rdata->gateways->len; i++) {
		NMNDiscGateway *item = &g_array_index (rdata->gateways, NMNDiscGateway, i);
		guint64 expiry = (guint64) item->timestamp + item->lifetime;

		if (item->lifetime == G_MAXUINT32)
			continue;

		if (now >= expiry) {
			g_array_remove_index (rdata->gateways, i--);
			*changed |= NM_NDISC_CONFIG_GATEWAYS;
		} else if (*nextevent > expiry)
			*nextevent = expiry;
	}
}

static void
clean_addresses (NMNDisc *ndisc, guint32 now, NMNDiscConfigMap *changed, guint32 *nextevent)
{
	NMNDiscDataInternal *rdata;
	guint i;

	rdata = &NM_NDISC_GET_PRIVATE (ndisc)->rdata;

	for (i = 0; i < rdata->addresses->len; i++) {
		NMNDiscAddress *item = &g_array_index (rdata->addresses, NMNDiscAddress, i);
		guint64 expiry = (guint64) item->timestamp + item->lifetime;

		if (item->lifetime == G_MAXUINT32)
			continue;

		if (now >= expiry) {
			g_array_remove_index (rdata->addresses, i--);
			*changed |= NM_NDISC_CONFIG_ADDRESSES;
		} else if (*nextevent > expiry)
			*nextevent = expiry;
	}
}

static void
clean_routes (NMNDisc *ndisc, guint32 now, NMNDiscConfigMap *changed, guint32 *nextevent)
{
	NMNDiscDataInternal *rdata;
	guint i;

	rdata = &NM_NDISC_GET_PRIVATE (ndisc)->rdata;

	for (i = 0; i < rdata->routes->len; i++) {
		NMNDiscRoute *item = &g_array_index (rdata->routes, NMNDiscRoute, i);
		guint64 expiry = (guint64) item->timestamp + item->lifetime;

		if (item->lifetime == G_MAXUINT32)
			continue;

		if (now >= expiry) {
			g_array_remove_index (rdata->routes, i--);
			*changed |= NM_NDISC_CONFIG_ROUTES;
		} else if (*nextevent > expiry)
			*nextevent = expiry;
	}
}

static void
clean_dns_servers (NMNDisc *ndisc, guint32 now, NMNDiscConfigMap *changed, guint32 *nextevent)
{
	NMNDiscDataInternal *rdata;
	guint i;

	rdata = &NM_NDISC_GET_PRIVATE (ndisc)->rdata;

	for (i = 0; i < rdata->dns_servers->len; i++) {
		NMNDiscDNSServer *item = &g_array_index (rdata->dns_servers, NMNDiscDNSServer, i);
		guint64 expiry = (guint64) item->timestamp + item->lifetime;
		guint64 refresh = (guint64) item->timestamp + item->lifetime / 2;

		if (item->lifetime == G_MAXUINT32)
			continue;

		if (now >= expiry) {
			g_array_remove_index (rdata->dns_servers, i--);
			*changed |= NM_NDISC_CONFIG_DNS_SERVERS;
		} else if (now >= refresh)
			solicit (ndisc);
		else if (*nextevent > refresh)
			*nextevent = refresh;
	}
}

static void
clean_dns_domains (NMNDisc *ndisc, guint32 now, NMNDiscConfigMap *changed, guint32 *nextevent)
{
	NMNDiscDataInternal *rdata;
	guint i;

	rdata = &NM_NDISC_GET_PRIVATE (ndisc)->rdata;

	for (i = 0; i < rdata->dns_domains->len; i++) {
		NMNDiscDNSDomain *item = &g_array_index (rdata->dns_domains, NMNDiscDNSDomain, i);
		guint64 expiry = (guint64) item->timestamp + item->lifetime;
		guint64 refresh = (guint64) item->timestamp + item->lifetime / 2;

		if (item->lifetime == G_MAXUINT32)
			continue;

		if (now >= expiry) {
			g_array_remove_index (rdata->dns_domains, i--);
			*changed |= NM_NDISC_CONFIG_DNS_DOMAINS;
		} else if (now >= refresh)
			solicit (ndisc);
		else if (*nextevent > refresh)
			*nextevent = refresh;
	}
}

static gboolean timeout_cb (gpointer user_data);

static void
check_timestamps (NMNDisc *ndisc, guint32 now, NMNDiscConfigMap changed)
{
	NMNDiscPrivate *priv = NM_NDISC_GET_PRIVATE (ndisc);
	/* Use a magic date in the distant future (~68 years) */
	guint32 never = G_MAXINT32;
	guint32 nextevent = never;

	nm_clear_g_source (&priv->timeout_id);

	clean_gateways (ndisc, now, &changed, &nextevent);
	clean_addresses (ndisc, now, &changed, &nextevent);
	clean_routes (ndisc, now, &changed, &nextevent);
	clean_dns_servers (ndisc, now, &changed, &nextevent);
	clean_dns_domains (ndisc, now, &changed, &nextevent);

	if (changed)
		_emit_config_change (ndisc, changed);

	if (nextevent != never) {
		g_return_if_fail (nextevent > now);
		_LOGD ("scheduling next now/lifetime check: %u seconds",
		       nextevent - now);
		priv->timeout_id = g_timeout_add_seconds (nextevent - now, timeout_cb, ndisc);
	}
}

static gboolean
timeout_cb (gpointer user_data)
{
	NMNDisc *self = user_data;

	NM_NDISC_GET_PRIVATE (self)->timeout_id = 0;
	check_timestamps (self, nm_utils_get_monotonic_timestamp_s (), 0);
	return G_SOURCE_REMOVE;
}

void
nm_ndisc_ra_received (NMNDisc *ndisc, guint32 now, NMNDiscConfigMap changed)
{
	NMNDiscPrivate *priv = NM_NDISC_GET_PRIVATE (ndisc);

	nm_clear_g_source (&priv->ra_timeout_id);
	nm_clear_g_source (&priv->send_rs_id);
	g_clear_pointer (&priv->last_send_rs_error, g_free);
	check_timestamps (ndisc, now, changed);
}

/*****************************************************************************/

static void
dns_domain_free (gpointer data)
{
	g_free (((NMNDiscDNSDomain *)(data))->domain);
}

static void
set_property (GObject *object, guint prop_id,
              const GValue *value, GParamSpec *pspec)
{
	NMNDisc *self = NM_NDISC (object);
	NMNDiscPrivate *priv = NM_NDISC_GET_PRIVATE (self);

	switch (prop_id) {
	case PROP_PLATFORM:
		/* construct-only */
		priv->platform = g_value_get_object (value) ? : NM_PLATFORM_GET;
		if (!priv->platform)
			g_return_if_reached ();

		g_object_ref (priv->platform);

		priv->netns = nm_platform_netns_get (priv->platform);
		if (priv->netns)
			g_object_ref (priv->netns);

		g_return_if_fail (!priv->netns || priv->netns == nmp_netns_get_current ());
		break;
	case PROP_IFINDEX:
		/* construct-only */
		priv->ifindex = g_value_get_int (value);
		g_return_if_fail (priv->ifindex > 0);
		break;
	case PROP_IFNAME:
		/* construct-only */
		priv->ifname = g_value_dup_string (value);
		g_return_if_fail (priv->ifname && priv->ifname[0]);
		break;
	case PROP_STABLE_TYPE:
		/* construct-only */
		priv->stable_type = g_value_get_int (value);
		break;
	case PROP_NETWORK_ID:
		/* construct-only */
		priv->network_id = g_value_dup_string (value);
		break;
	case PROP_ADDR_GEN_MODE:
		/* construct-only */
		priv->addr_gen_mode = g_value_get_int (value);
		break;
	case PROP_MAX_ADDRESSES:
		/* construct-only */
		priv->max_addresses = g_value_get_int (value);
		break;
	case PROP_ROUTER_SOLICITATIONS:
		/* construct-only */
		priv->router_solicitations = g_value_get_int (value);
		break;
	case PROP_ROUTER_SOLICITATION_INTERVAL:
		/* construct-only */
		priv->router_solicitation_interval = g_value_get_int (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
nm_ndisc_init (NMNDisc *ndisc)
{
	NMNDiscPrivate *priv;
	NMNDiscDataInternal *rdata;

	priv = G_TYPE_INSTANCE_GET_PRIVATE (ndisc, NM_TYPE_NDISC, NMNDiscPrivate);
	ndisc->_priv = priv;

	rdata = &priv->rdata;

	rdata->gateways = g_array_new (FALSE, FALSE, sizeof (NMNDiscGateway));
	rdata->addresses = g_array_new (FALSE, FALSE, sizeof (NMNDiscAddress));
	rdata->routes = g_array_new (FALSE, FALSE, sizeof (NMNDiscRoute));
	rdata->dns_servers = g_array_new (FALSE, FALSE, sizeof (NMNDiscDNSServer));
	rdata->dns_domains = g_array_new (FALSE, FALSE, sizeof (NMNDiscDNSDomain));
	g_array_set_clear_func (rdata->dns_domains, dns_domain_free);
	priv->rdata.public.hop_limit = 64;

	/* Start at very low number so that last_rs - router_solicitation_interval
	 * is much lower than nm_utils_get_monotonic_timestamp_s() at startup.
	 */
	priv->last_rs = G_MININT32;
}

static void
dispose (GObject *object)
{
	NMNDisc *ndisc = NM_NDISC (object);
	NMNDiscPrivate *priv = NM_NDISC_GET_PRIVATE (ndisc);

	nm_clear_g_source (&priv->ra_timeout_id);
	nm_clear_g_source (&priv->send_rs_id);
	g_clear_pointer (&priv->last_send_rs_error, g_free);

	nm_clear_g_source (&priv->timeout_id);

	G_OBJECT_CLASS (nm_ndisc_parent_class)->dispose (object);
}

static void
finalize (GObject *object)
{
	NMNDisc *ndisc = NM_NDISC (object);
	NMNDiscPrivate *priv = NM_NDISC_GET_PRIVATE (ndisc);
	NMNDiscDataInternal *rdata = &priv->rdata;

	g_free (priv->ifname);
	g_free (priv->network_id);

	g_array_unref (rdata->gateways);
	g_array_unref (rdata->addresses);
	g_array_unref (rdata->routes);
	g_array_unref (rdata->dns_servers);
	g_array_unref (rdata->dns_domains);

	g_clear_object (&priv->netns);
	g_clear_object (&priv->platform);

	G_OBJECT_CLASS (nm_ndisc_parent_class)->finalize (object);
}

static void
nm_ndisc_class_init (NMNDiscClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	g_type_class_add_private (klass, sizeof (NMNDiscPrivate));

	object_class->set_property = set_property;
	object_class->dispose = dispose;
	object_class->finalize = finalize;

	obj_properties[PROP_PLATFORM] =
	    g_param_spec_object (NM_NDISC_PLATFORM, "", "",
	                         NM_TYPE_PLATFORM,
	                         G_PARAM_WRITABLE |
	                         G_PARAM_CONSTRUCT_ONLY |
	                         G_PARAM_STATIC_STRINGS);
	obj_properties[PROP_IFINDEX] =
	    g_param_spec_int (NM_NDISC_IFINDEX, "", "",
	                      0, G_MAXINT, 0,
	                      G_PARAM_WRITABLE |
	                      G_PARAM_CONSTRUCT_ONLY |
	                      G_PARAM_STATIC_STRINGS);
	obj_properties[PROP_IFNAME] =
	    g_param_spec_string (NM_NDISC_IFNAME, "", "",
	                         NULL,
	                         G_PARAM_WRITABLE |
	                         G_PARAM_CONSTRUCT_ONLY |
	                         G_PARAM_STATIC_STRINGS);
	obj_properties[PROP_STABLE_TYPE] =
	    g_param_spec_int (NM_NDISC_STABLE_TYPE, "", "",
	                      NM_UTILS_STABLE_TYPE_UUID, NM_UTILS_STABLE_TYPE_STABLE_ID, NM_UTILS_STABLE_TYPE_UUID,
	                      G_PARAM_WRITABLE |
	                      G_PARAM_CONSTRUCT_ONLY |
	                      G_PARAM_STATIC_STRINGS);
	obj_properties[PROP_NETWORK_ID] =
	    g_param_spec_string (NM_NDISC_NETWORK_ID, "", "",
	                         NULL,
	                         G_PARAM_WRITABLE |
	                         G_PARAM_CONSTRUCT_ONLY |
	                         G_PARAM_STATIC_STRINGS);
	obj_properties[PROP_ADDR_GEN_MODE] =
	    g_param_spec_int (NM_NDISC_ADDR_GEN_MODE, "", "",
	                      NM_SETTING_IP6_CONFIG_ADDR_GEN_MODE_EUI64, NM_SETTING_IP6_CONFIG_ADDR_GEN_MODE_STABLE_PRIVACY, NM_SETTING_IP6_CONFIG_ADDR_GEN_MODE_EUI64,
	                      G_PARAM_WRITABLE |
	                      G_PARAM_CONSTRUCT_ONLY |
	                      G_PARAM_STATIC_STRINGS);
	obj_properties[PROP_MAX_ADDRESSES] =
	    g_param_spec_int (NM_NDISC_MAX_ADDRESSES, "", "",
	                      0, G_MAXINT32, NM_NDISC_MAX_ADDRESSES_DEFAULT,
	                      G_PARAM_WRITABLE |
	                      G_PARAM_CONSTRUCT_ONLY |
	                      G_PARAM_STATIC_STRINGS);
	obj_properties[PROP_ROUTER_SOLICITATIONS] =
	    g_param_spec_int (NM_NDISC_ROUTER_SOLICITATIONS, "", "",
	                      1, G_MAXINT32, NM_NDISC_ROUTER_SOLICITATIONS_DEFAULT,
	                      G_PARAM_WRITABLE |
	                      G_PARAM_CONSTRUCT_ONLY |
	                      G_PARAM_STATIC_STRINGS);
	obj_properties[PROP_ROUTER_SOLICITATION_INTERVAL] =
	    g_param_spec_int (NM_NDISC_ROUTER_SOLICITATION_INTERVAL, "", "",
	                      1, G_MAXINT32, NM_NDISC_ROUTER_SOLICITATION_INTERVAL_DEFAULT,
	                      G_PARAM_WRITABLE |
	                      G_PARAM_CONSTRUCT_ONLY |
	                      G_PARAM_STATIC_STRINGS);
	g_object_class_install_properties (object_class, _PROPERTY_ENUMS_LAST, obj_properties);

	signals[CONFIG_CHANGED] =
	    g_signal_new (NM_NDISC_CONFIG_CHANGED,
	                  G_OBJECT_CLASS_TYPE (klass),
	                  G_SIGNAL_RUN_FIRST,
	                  0,
	                  NULL, NULL, NULL,
	                  G_TYPE_NONE, 2, G_TYPE_POINTER, G_TYPE_UINT);
	signals[RA_TIMEOUT] =
	    g_signal_new (NM_NDISC_RA_TIMEOUT,
	                  G_OBJECT_CLASS_TYPE (klass),
	                  G_SIGNAL_RUN_FIRST,
	                  0,
	                  NULL, NULL, NULL,
	                  G_TYPE_NONE, 0);
}
