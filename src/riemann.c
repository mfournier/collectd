/*
 * collectd - src/riemann.c
 *
 * Copyright (C) 2012  Pierre-Yves Ritschard <pyr@spootnik.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "collectd.h"
#include "plugin.h"
#include "common.h"
#include "configfile.h"
#include "riemann.pb-c.h"

#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <inttypes.h>
#include <pthread.h>

#define RIEMANN_DELAY		1
#define RIEMANN_PORT		5555
#define RIEMANN_MAX_TAGS	37
#define RIEMANN_EXTRA_TAGS	32

struct riemann_host {
	struct riemann_host	*next;
#define F_CONNECT		 0x01
	u_int8_t		 flags;
	pthread_mutex_t		 lock;
	int			 delay;
	char			 name[DATA_MAX_NAME_LEN];
	int			 port;
	int			 s;
};

struct riemann_event {
	Event		 ev;
	char		 service[DATA_MAX_NAME_LEN];
	const char	*tags[RIEMANN_MAX_TAGS];
};

char	*riemann_tags[RIEMANN_EXTRA_TAGS];
int	 riemann_tagcount;

int	riemann_write(const data_set_t *, const value_list_t *, user_data_t *);
int	riemann_connect(struct riemann_host *);
void	riemann_free(void *);
int	riemann_config_host(oconfig_item_t *);
int	riemann_config(oconfig_item_t *);
void	module_register(void);

/*
 * Functions
 */
int
riemann_write(const data_set_t *ds,
	      const value_list_t *vl,
	      user_data_t *ud)
{
	int			 i, j;
	int			 status;
	struct riemann_host	*host = ud->data;
	Msg			 msg = MSG__INIT;
	Event			*ev;
	struct riemann_event	*event_tab, *event;
	u_char			*buf;
	size_t			 len;

	if ((status = riemann_connect(host)) != 0)
		return status;

	msg.n_events = vl->values_len;

	/*
	 * Get rid of allocations up front
	 */
	if ((msg.events = calloc(msg.n_events, sizeof(*msg.events))) == NULL ||
	    (event_tab = calloc(msg.n_events, sizeof(*event_tab))) == NULL) {
		free(msg.events);
		free(event_tab);
		return ENOMEM;
	}

	/*
	 * Now produce valid protobuf structures
	 */
	for (i = 0; i < vl->values_len; i++) {
		event = &event_tab[i];
		event__init(&event->ev);

		ev = &event->ev;
		event__init(ev);
		ev->host = host->name;
		ev->has_time = 1;
		ev->time = CDTIME_T_TO_TIME_T(vl->time);
		ev->has_ttl = 1;
		ev->ttl = CDTIME_T_TO_TIME_T(vl->interval) + host->delay;
		ev->n_tags = 3;
		ev->tags = (char **)event->tags;
		event->tags[0] = DS_TYPE_TO_STRING(ds->ds[i].type);
		event->tags[1] = vl->plugin;
		event->tags[2] = ds->ds[i].name;
		if (vl->plugin_instance && strlen(vl->plugin_instance)) {
			event->tags[ev->n_tags++] = vl->plugin_instance;
		}
		if (vl->type && strlen(vl->type)) {
			event->tags[ev->n_tags++] = vl->type;
		}
		if (vl->type_instance && strlen(vl->type_instance)) {
			event->tags[ev->n_tags++] = vl->type_instance;
		}

		/* add user defined extra tags */
		for (j = 0; j < riemann_tagcount; j++)
			event->tags[ev->n_tags++] = riemann_tags[j];

		switch (ds->ds[i].type) {
		case DS_TYPE_COUNTER:
			ev->has_metric_sint64 = 1;
			ev->metric_sint64 = vl->values[i].counter;
			break;
		case DS_TYPE_GAUGE:
			ev->has_metric_d = 1;
			ev->metric_d = vl->values[i].gauge;
			break;
		case DS_TYPE_DERIVE:
			ev->has_metric_sint64 = 1;
			ev->metric_sint64 = vl->values[i].derive;
			break;
		case DS_TYPE_ABSOLUTE:
			ev->has_metric_sint64 = 1;
			ev->metric_sint64 = vl->values[i].absolute;
			break;
		default:
			WARNING("riemann_write: unknown metric type: %d",
				ds->ds[i].type);
			break;
		}
		ssnprintf(event->service, sizeof(event->service),
			  "%s-%s-%s-%s-%s", vl->plugin, vl->plugin_instance,
			  vl->type, vl->type_instance, ds->ds[i].name);
		ev->service = event->service;
		DEBUG("riemann_write: %s ready to send", ev->service);
		msg.events[i] = ev;
	}
	
	/*
	 * we have now packed a bunch of events, let's pack them
	 */
	len = msg__get_packed_size(&msg);
	DEBUG("riemann_write: packed size computed: %ld", len);
	if ((buf = calloc(1, len)) == NULL) {
		WARNING("riemann_write: failing to alloc buf!");
		sfree(msg.events);
		return ENOMEM;
	}

	/*
	 * prepend full size to beginning of buffer
	 */
	msg__pack(&msg, buf);
	sfree(msg.events);

	/*
	 * we're now ready to send
	 */
	if (write(host->s, buf, len) != len) {
		WARNING("riemann_write: could not send out full packet");
		return -1;
	}
	free(buf);
	return 0;
}

int
riemann_connect(struct riemann_host *host)
{
	int			 e;
	struct addrinfo		*ai, *res, hints;
	struct sockaddr_in	*sin4;
	struct sockaddr_in6	*sin6;

	if (host->flags & F_CONNECT)
		return 0;
		
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	
	if ((e = getaddrinfo(host->name, NULL, &hints, &res)) != 0) {
		WARNING("could not resolve host \"%s\": %s",
			host->name, gai_strerror(e));
		return -1;
	}

	for (ai = res; ai != NULL; ai = ai->ai_next) {
		pthread_mutex_lock(&host->lock);
		/*
		 * check if another thread did not already succesfully connect
		 */
		if (host->flags & F_CONNECT) {
			freeaddrinfo(res);
			return 0;
		}
		
		if ((host->s = socket(ai->ai_family, SOCK_DGRAM, 0)) == -1) {
			pthread_mutex_unlock(&host->lock);
			WARNING("riemann_connect: could not open socket");
			freeaddrinfo(res);
			return -1;
		}

		switch (ai->ai_family) {
		case AF_INET:
			sin4 = (struct sockaddr_in *)ai->ai_addr;
			sin4->sin_port = ntohs(host->port);
			break;
		case AF_INET6:
			sin6 = (struct sockaddr_in6 *)ai->ai_addr;
			sin6->sin6_port = ntohs(host->port);
			break;
		default:
			WARNING("riemann_connect: unsupported address family");
			close(host->s);
			pthread_mutex_unlock(&host->lock);
			freeaddrinfo(res);
			return -1;
		}

		if (connect(host->s, ai->ai_addr, ai->ai_addrlen) != 0) {
			close(host->s);
			host->flags |= ~F_CONNECT;
			pthread_mutex_unlock(&host->lock);
			freeaddrinfo(res);
			return -1;
		}
		host->flags |= F_CONNECT;
		DEBUG("got a succesful connection for: %s", host->name);
		pthread_mutex_unlock(&host->lock);
		break;
	}
	
	freeaddrinfo(res);
	if (ai == NULL) {
		WARNING("riemann_connect: no suitable hosts found");
		return -1;
	}

	return 0;
}

void
riemann_free(void *p)
{
	struct riemann_host	*host = p;

	if (host->flags & F_CONNECT)
		close(host->s);
	sfree(host);
}

int
riemann_config_host(oconfig_item_t *ci)
{
	struct riemann_host	*host = NULL;
	int			 status = 0;
	int			 i;
	oconfig_item_t		*child;
	char			 cb_name[DATA_MAX_NAME_LEN];
	user_data_t		 ud;

	if (ci->values_num != 1 ||
	    ci->values[0].type != OCONFIG_TYPE_STRING) {
		WARNING("riemann hosts need one string argument");
		return -1;
	}

	if ((host = calloc(1, sizeof (*host))) == NULL) {
		WARNING("riemann host allocation failed");
		return ENOMEM;
	}

	if (cf_util_get_string_buffer(ci, host->name,
				      sizeof(host->name)) != 0) {
		WARNING("riemann host name too long");
		sfree(host);
		return -1;
	}

	host->port = RIEMANN_PORT;
	host->delay = RIEMANN_DELAY;
	for (i = 0; i < ci->children_num; i++) {
		/*
		 * The code here could be simplified but makes room
		 * for easy adding of new options later on.
		 */
		child = &ci->children[i];
		status = 0;

		if (strcasecmp(child->key, "port") == 0) {
			if ((status = cf_util_get_port_number(child)) < 0) {
				WARNING("invalid port number");
				break;
			}
			host->port = status;
			status = 0;
		} else if (strcasecmp(child->key, "delay") == 0) {
			if ((status = cf_util_get_int(ci, &host->delay)) != 0)
				break;
		} else {
			WARNING("riemann plugin: ignoring unknown config "
				"option: \"%s\"", child->key);
		}
	}
	if (status != 0) {
		sfree(host);
		return status;
	}

	pthread_mutex_init(&host->lock, NULL);
	ssnprintf(cb_name, sizeof(cb_name), "riemann/%s:%d", host->name, host->port);
	DEBUG("riemann cb_name: %s", cb_name);
	ud.data = host;
	ud.free_func = riemann_free;
	
	if ((status = plugin_register_write(cb_name, riemann_write, &ud)) != 0)
		riemann_free(host);
	
	return status;
}

int
riemann_config(oconfig_item_t *ci)
{
	int		 i;
	char		*newtag;
	oconfig_item_t	*child;

	for (i = 0; i < ci->children_num; i++)  {
		child = &ci->children[i];

		if (strcasecmp(child->key, "host") == 0) {
			riemann_config_host(child);
		} else if (strcasecmp(child->key, "tag") == 0) {
			if (riemann_tagcount >= RIEMANN_EXTRA_TAGS) {
				WARNING("riemann plugin: too many tags");
				return -1;
			}
			newtag = NULL;
			cf_util_get_string(child, &newtag);
			if (newtag == NULL)
				return -1;
			riemann_tags[riemann_tagcount++] = newtag;
			DEBUG("riemann_config: got tag: %s", newtag);
 
		} else {
			WARNING ("riemann plugin: Ignoring unknown "
				 "configuration option \"%s\" at top level.",
				 child->key);
		}
	}
	return (0);
}

void
module_register(void)
{
	DEBUG("riemann: module_register");
	
	plugin_register_complex_config ("riemann", riemann_config);
}
