/*
 * FreeSWITCH API
 *
 * Copyright (C) 2017 OpenSIPS Solutions
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * opensips is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 *
 * History:
 * --------
 *  2017-01-23 initial version (liviu)
 */

#include "../../parser/parse_uri.h"
#include "../../resolve.h"
#include "../../forward.h"
#include "../../ut.h"
#include "../../lib/url.h"

#include "fs_api.h"

extern int event_heartbeat_interval;

struct list_head *fs_boxes;
rw_lock_t *box_lock;

static fs_mod_ref *mk_fs_mod_ref(str *tag);
static void free_fs_mod_ref(fs_mod_ref *mref);
static fs_evs *get_fs_evs(const str *fs_url);

/*
 * Parses and validates FreeSWITCH URLs:
 *        "[fs://][[username]:password@]host[:port][;EVENT1[,EVENT2[,.. ]]]"
 */
static int parse_fs_url(const str *in, str *user_out, str *pass_out, str *host_out,
                        unsigned int *port_out)
{
	str st = *in;
	str port;
	char *p, *h;

	if (st.len > FS_SOCK_PREFIX_LEN) {
		if (memcmp(st.s, FS_SOCK_PREFIX, FS_SOCK_PREFIX_LEN) == 0) {
			st.len -= FS_SOCK_PREFIX_LEN;
			st.s += FS_SOCK_PREFIX_LEN;
		}
	}

	p = memchr(st.s, ':', st.len);
	if (!p || !(h = memchr(p, '@', st.len - (p - st.s)))) {
		LM_ERR("failed to locate \":password\" part!\n");
		return -1;
	}

	user_out->len = p - st.s;
	user_out->s = st.s;

	p++;
	pass_out->len = h - p;
	pass_out->s = p;

	h++;
	p = memchr(h, ':', (st.len - (h - st.s)));
	if (p) {
		p++;
		port.s = p;
		port.len = st.len - (p - st.s);

		if (str2int(&port, port_out) != 0) {
			LM_ERR("failed to parse port '%.*s' %d in host '%.*s'\n",
			       port.len, port.s, port.len, st.len, st.s);
			return -1;
		}

		host_out->s = h;
		host_out->len = (p - 1) - h;
	} else {
		host_out->s = h;
		host_out->len = st.len - (h - st.s);
		*port_out = FS_DEFAULT_EVS_PORT;
	}

	return 0;
}

static fs_mod_ref *mk_fs_mod_ref(str *tag)
{
	fs_mod_ref *mref = NULL;

	mref = shm_malloc(sizeof *mref + tag->len);
	if (!mref) {
		LM_ERR("out of mem\n");
		return NULL;
	}
	memset(mref, 0, sizeof *mref);

	mref->tag.s = (char *)(mref + 1);
	mref->tag.len = tag->len;
	memcpy(mref->tag.s, tag->s, tag->len);

	return mref;
}

static void free_fs_mod_ref(fs_mod_ref *mref)
{
	mref->tag.s = NULL;
	shm_free(mref);
}

static fs_mod_ref *get_fs_mod_ref(fs_evs *evs, str *tag)
{
	struct list_head *ele;
	fs_mod_ref *mref;

	list_for_each(ele, &evs->modules) {
		mref = list_entry(ele, fs_mod_ref, list);

		if (str_strcmp(&mref->tag, tag) == 0) {
			return mref;
		}
	}

	return NULL;
}

static fs_evs *get_fs_evs(const str *fs_url)
{
	struct list_head *ele;
	str user, pass, host;
	unsigned int port;
	fs_evs *evs;

	if (parse_fs_url(fs_url, &user, &pass, &host, &port) != 0) {
		LM_ERR("bad FS URL: '%.*s'! Need: fs://[user]:pass@host[:port]\n",
		       fs_url->len, fs_url->s);
		return NULL;
	}

	list_for_each(ele, fs_boxes) {
		evs = list_entry(ele, fs_evs, list);

		if (str_strcmp(&host, &evs->host) == 0 &&
		    str_strcmp(&user, &evs->user) == 0 &&
		    str_strcmp(&pass, &evs->pass) == 0 &&
		    port == evs->port) {
			return evs;
		}
	}

	return NULL;
}


static fs_evs *mk_fs_evs(const str *fs_url)
{
	fs_evs *evs;
	str user, pass, host;
	unsigned int port;
	struct url *url;

	url = parse_url(fs_url, URL_REQ_SCHEME|URL_REQ_PASS, 0);
	if (!url) {
		LM_ERR("bad FS URL: '%.*s'! "
		       "Need: fs://[user]:pass@host[:port][;EVENT1[EVENT2,..]]\n",
		       fs_url->len, fs_url->s);
		return NULL;
	}

	if (parse_fs_url(fs_url, &user, &pass, &host, &port) != 0) {
		LM_ERR("bad FS URL: '%.*s'! Need: fs://[user]:pass@host[:port]\n",
		       fs_url->len, fs_url->s);
		return NULL;
	}

	evs = shm_malloc(sizeof *evs + host.len + 1 + user.len + 1 + pass.len + 1);
	if (!evs) {
		LM_ERR("out of mem\n");
		return NULL;
	}
	memset(evs, 0, sizeof *evs);
	INIT_LIST_HEAD(&evs->modules);

	evs->stats_lk = lock_init_rw();
	if (!evs->stats_lk) {
		LM_ERR("out of mem\n");
		shm_free(evs);
		return NULL;
	}

	LM_DBG("new FS box: host=%.*s, port=%d\n", host.len, host.s, port);

	evs->user.s = (char *)(evs + 1);
	evs->user.len = user.len;
	memcpy(evs->user.s, user.s, user.len);
	evs->user.s[evs->user.len] = '\0';

	evs->pass.s = evs->user.s + evs->user.len + 1;
	evs->pass.len = pass.len;
	memcpy(evs->pass.s, pass.s, pass.len);
	evs->pass.s[evs->pass.len] = '\0';

	evs->host.s = evs->pass.s + evs->pass.len + 1;
	evs->host.len = host.len;
	memcpy(evs->host.s, host.s, host.len);
	evs->host.s[evs->host.len] = '\0';

	evs->port = port;
	return evs;
}

fs_evs *get_evs(const str *evs_str)
{
	fs_evs *evs;

	if (!evs_str->s || evs_str->len == 0) {
		LM_ERR("empty string given!\n");
		return NULL;
	}

	lock_start_write(box_lock);

	evs = get_fs_evs(evs_str);
	LM_DBG("getevs (%.*s): %p\n", evs_str->len, evs_str->s, evs);
	if (!evs) {
		evs = mk_fs_evs(evs_str);
		if (!evs) {
			LM_ERR("failed to create FS box!\n");
			lock_stop_write(box_lock);
			return NULL;
		}

		list_add(&evs->list, fs_boxes);
	}

	evs->ref++;

	lock_stop_write(box_lock);
	return evs;
}

int evs_sub(fs_evs *evs, const str *tag, const struct str_list *events,
            fs_event_cb_f ev_cb)
{
	return 0;
}

void evs_unsub(fs_evs *evs, const str *tag, const struct str_list *events)
{
}

void put_evs(fs_evs *evs)
{
	/**
	 * This prevents a series of deadlocks on shutdown, since the FS connection
	 * manager process is often terminated (SIGTERM) on a typical OpenSIPS
	 * restart along with any locks it has acquired.
	 *
	 * If the "main" process gets here, then he's the only one left anyway
	 */
	if (!is_main)
		lock_start_write(box_lock);

	evs->ref--;

	/**
	 * We cannot immediately free the event socket, as the fd might be polled
	 * by the FreeSWITCH worker process. The ref == 0 check is done there
	 */

	if (!is_main)
		lock_stop_write(box_lock);
}

fs_evs *get_stats_evs(str *evs_str, str *tag)
{
	fs_evs *evs;
	fs_mod_ref *mref;

	if (!evs_str->s || evs_str->len == 0 || !tag) {
		LM_ERR("bad params: '%.*s', %.*s\n", evs_str->len, evs_str->s,
		       (tag ? tag->len:0), (tag ? tag->s: ""));
		return NULL;
	}

	lock_start_write(box_lock);

	evs = get_fs_evs(evs_str);
	LM_DBG("getevs (%.*s): %p\n", evs_str->len, evs_str->s, evs);
	if (!evs) {
		evs = mk_fs_evs(evs_str);
		if (!evs) {
			LM_ERR("failed to create FS box!\n");
			goto out_err;
		}
		evs->type = FS_GW_STATS;

		list_add(&evs->list, fs_boxes);
	}

	mref = mk_fs_mod_ref(tag);
	if (!mref) {
		LM_ERR("mk tag failed\n");
		goto out_err;
	}

	list_add(&mref->list, &evs->modules);

	evs->ref++;

	lock_stop_write(box_lock);
	return evs;

out_err:
	lock_stop_write(box_lock);
	return NULL;
}

int put_stats_evs(fs_evs *evs, str *tag)
{
	fs_mod_ref *mref;

	/**
	 * This prevents a series of deadlocks on shutdown, since the FS connection
	 * manager process is often terminated (SIGTERM) on a typical OpenSIPS
	 * restart along with any locks it has acquired.
	 *
	 * If the "main" process gets here, then he's the only one left anyway
	 */
	if (!is_main)
		lock_start_write(box_lock);

	mref = get_fs_mod_ref(evs, tag);
	if (!mref) {
		LM_ERR("mod ref %.*s does not exist in evs %s:%d\n", tag->len, tag->s,
		       evs->host.s, evs->port);
		goto out_err;
	}

	list_del(&mref->list);
	free_fs_mod_ref(mref);

	evs->ref--;

	/**
	 * We cannot immediately free the event socket, as the fd might be polled
	 * by the FreeSWITCH worker process. The ref == 0 check is done there
	 */

	if (!is_main)
		lock_stop_write(box_lock);

	return 0;

out_err:

	if (!is_main)
		lock_stop_write(box_lock);

	return -1;
}

int fs_bind(struct fs_binds *fapi)
{
	LM_INFO("loading FS API ...\n");

	memset(fapi, 0, sizeof *fapi);

	fapi->stats_update_interval = event_heartbeat_interval;
	fapi->get_evs               = get_evs;
	fapi->put_evs               = put_evs;
	fapi->get_stats_evs         = get_stats_evs;
	fapi->put_stats_evs         = put_stats_evs;

	return 0;
}
