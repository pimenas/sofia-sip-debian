/*
 * This file is part of the Sofia-SIP package
 *
 * Copyright (C) 2005 Nokia Corporation.
 *
 * Contact: Pekka Pessi <pekka.pessi@nokia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

/**@CFILE nua_stack.c
 * @brief Sofia-SIP User Agent Engine implementation
 *
 * @author Pekka Pessi <Pekka.Pessi@nokia.com>
 * @author Kai Vehmanen <Kai.Vehmanen@nokia.com>
 * @author Martti Mela <Martti Mela@nokia.com>
 * @author Remeres Jacobs <Remeres.Jacobs@nokia.com>
 * @author Tat Chan <Tat.Chan@nokia.com>
 *
 * @date Created: Wed Feb 14 18:32:58 2001 ppessi
 */

#include "config.h"

#include <sofia-sip/su_tag_class.h>
#include <sofia-sip/su_tag_inline.h>
#include <sofia-sip/su_tagarg.h>
#include <sofia-sip/su_strlst.h>
#include <sofia-sip/su_uniqueid.h>

#include <sofia-sip/su_tag_io.h>

#define SU_ROOT_MAGIC_T   struct nua_s
#define SU_MSG_ARG_T      struct event_s

#define NUA_SAVED_EVENT_T su_msg_t *

#define NTA_AGENT_MAGIC_T    struct nua_s
#define NTA_LEG_MAGIC_T      struct nua_handle_s
#define NTA_OUTGOING_MAGIC_T struct nua_client_request

#include <sofia-sip/sip.h>
#include <sofia-sip/sip_header.h>
#include <sofia-sip/sip_status.h>
#include <sofia-sip/sip_util.h>

#include <sofia-sip/tport_tag.h>
#include <sofia-sip/nta.h>
#include <sofia-sip/nta_tport.h>
#include <sofia-sip/auth_client.h>

#include <sofia-sip/soa.h>

#include "sofia-sip/nua.h"
#include "sofia-sip/nua_tag.h"
#include "nua_stack.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>

#include <assert.h>

/* ========================================================================
 *
 *                       Protocol stack side
 *
 * ======================================================================== */

nua_handle_t *nh_create(nua_t *nua, tag_type_t t, tag_value_t v, ...);
static void nh_append(nua_t *nua, nua_handle_t *nh);
static void nh_remove(nua_t *nua, nua_handle_t *nh);

static int nh_authorize(nua_handle_t *nh,
			tag_type_t tag, tag_value_t value, ...);

static int nh_challenge(nua_handle_t *nh, sip_t const *sip);

static void nua_stack_timer(nua_t *nua, su_timer_t *t, su_timer_arg_t *a);

inline int nua_client_request_queue(nua_client_request_t *cr);
inline nua_client_request_t *nua_client_request_remove(nua_client_request_t *cr);

/* ---------------------------------------------------------------------- */
/* Constant data */

/**@internal Default internal error. */
char const nua_internal_error[] = "Internal NUA Error";

char const nua_application_sdp[] = "application/sdp";

#define NUA_STACK_TIMER_INTERVAL (1000)

/* ----------------------------------------------------------------------
 * Initialization & deinitialization
 */

int nua_stack_init(su_root_t *root, nua_t *nua)
{
  su_home_t *home;
  nua_handle_t *dnh;

  static int initialized_logs = 0;

  enter;

  if (!initialized_logs) {
    extern su_log_t tport_log[];
    extern su_log_t nta_log[];
    extern su_log_t nea_log[];
    extern su_log_t iptsec_log[];

    su_log_init(tport_log);
    su_log_init(nta_log);
    su_log_init(nea_log);
    su_log_init(iptsec_log);

    initialized_logs = 1;
  }

  nua->nua_root = root;
  nua->nua_timer = su_timer_create(su_root_task(root),
				   NUA_STACK_TIMER_INTERVAL);
  if (!nua->nua_timer)
    return -1;

  home = nua->nua_home;
  nua->nua_handles_tail = &nua->nua_handles;
  sip_from_init(nua->nua_from);

  dnh = su_home_clone(nua->nua_home, sizeof (*dnh) + sizeof(*dnh->nh_prefs));
  if (!dnh)
    return -1;

  dnh->nh_prefs = (void *)(dnh + 1);
  dnh->nh_valid = nua_valid_handle_cookie;
  dnh->nh_nua = nua;
  nua_handle_ref(dnh); dnh->nh_ref_by_stack = 1; 
  nua_handle_ref(dnh); dnh->nh_ref_by_user = 1;
  nh_append(nua, dnh);
  dnh->nh_identity = dnh;
  dnh->nh_ds->ds_local = nua->nua_from;
  dnh->nh_ds->ds_remote = nua->nua_from;

  if (nua_stack_set_defaults(dnh, dnh->nh_prefs) < 0)
    return -1;

  if (nua_stack_set_params(nua, dnh, nua_i_none, nua->nua_args) < 0)
    return -1;

  nua->nua_invite_accept = sip_accept_make(home, SDP_MIME_TYPE);

  nua->nua_nta = nta_agent_create(root, NONE, NULL, NULL,
				  NTATAG_MERGE_482(1),
				  NTATAG_CLIENT_RPORT(1),
				  NTATAG_UA(1),
#if HAVE_SOFIA_SMIME
				  NTATAG_SMIME(nua->sm),
#endif
				  TPTAG_STUN_SERVER(1),
				  TAG_NEXT(nua->nua_args));

  dnh->nh_ds->ds_leg = nta_leg_tcreate(nua->nua_nta,
				       nua_stack_process_request, dnh,
				       NTATAG_NO_DIALOG(1),
				       TAG_END());

  if (nua->nua_nta == NULL ||
      dnh->nh_ds->ds_leg == NULL || 
      nta_agent_set_params(nua->nua_nta, NTATAG_UA(1), TAG_END()) < 0 ||
      nua_stack_init_transport(nua, nua->nua_args) < 0) {
    SU_DEBUG_1(("nua: initializing SIP stack failed\n"));
    return -1;
  }

  if (nua_stack_set_from(nua, 1, nua->nua_args) < 0)
    return -1;

  if (NHP_ISSET(dnh->nh_prefs, detect_network_updates))
    nua_stack_launch_network_change_detector(nua);

  nua_stack_timer(nua, nua->nua_timer, NULL);

  return 0;
}

void nua_stack_deinit(su_root_t *root, nua_t *nua)
{
  enter;

  su_timer_destroy(nua->nua_timer), nua->nua_timer = NULL;
  nta_agent_destroy(nua->nua_nta), nua->nua_nta = NULL;
}

/* ----------------------------------------------------------------------
 * Sending events to client application
 */

static void nua_stack_shutdown(nua_t *);

void
  nua_stack_authenticate(nua_t *, nua_handle_t *, nua_event_t, tagi_t const *),
  nua_stack_respond(nua_t *, nua_handle_t *, int , char const *, tagi_t const *),
  nua_stack_destroy_handle(nua_t *, nua_handle_t *, tagi_t const *);

/* Notifier */
void
  nua_stack_authorize(nua_t *, nua_handle_t *, nua_event_t, tagi_t const *),
  nua_stack_notifier(nua_t *, nua_handle_t *, nua_event_t, tagi_t const *),
  nua_stack_terminate(nua_t *, nua_handle_t *, nua_event_t, tagi_t const *);

int nh_notifier_shutdown(nua_handle_t *nh, nea_event_t *ev,
			 tag_type_t t, tag_value_t v, ...);

int nua_stack_tevent(nua_t *nua, nua_handle_t *nh, msg_t *msg,
		     nua_event_t event, int status, char const *phrase,
		     tag_type_t tag, tag_value_t value, ...)
{
  ta_list ta;
  int retval;
  ta_start(ta, tag, value);
  retval = nua_stack_event(nua, nh, msg, event, status, phrase, ta_args(ta));
  ta_end(ta);
  return retval;
}

/** @internal Send an event to the application. */
int nua_stack_event(nua_t *nua, nua_handle_t *nh, msg_t *msg,
		    nua_event_t event, int status, char const *phrase,
		    tagi_t const *tags)
{
  su_msg_r sumsg = SU_MSG_R_INIT;
  size_t e_len, len, xtra, p_len;

  if (event == nua_r_ack || event == nua_i_none)
    return event;

  if (nh == nua->nua_dhandle)
    nh = NULL;

  if (nua_log->log_level >= 5) {
    char const *name = nua_event_name(event) + 4;
    char const *p = phrase ? phrase : "";

    if (status == 0)
      SU_DEBUG_5(("nua(%p): event %s %s\n", (void *)nh, name, p));
    else
      SU_DEBUG_5(("nua(%p): event %s %u %s\n", (void *)nh, name, status, p));
  }

  if (event == nua_r_destroy) {
    if (msg)
      msg_destroy(msg);
    if (status >= 200) {
      nh_destroy(nua, nh);
    }
    return event;
  }

  if ((event > nua_r_authenticate && event <= nua_r_ack)
      || event < nua_i_error
      || (nh && !nh->nh_valid)
      || (nua->nua_shutdown && event != nua_r_shutdown)) {
    if (msg)
      msg_destroy(msg);
    return event;
  }

  if (tags) {
    e_len = offsetof(event_t, e_tags);
    len = tl_len(tags);
    xtra = tl_xtra(tags, len);
  }
  else {
    e_len = sizeof(event_t), len = 0, xtra = 0;
  }
  p_len = phrase ? strlen(phrase) + 1 : 1;

  if (su_msg_create(sumsg, nua->nua_client, su_task_null,
		    nua_event, e_len + len + xtra + p_len) == 0) {
    event_t *e = su_msg_data(sumsg);
    void *p;

    if (tags) {
      tagi_t *t = e->e_tags, *t_end = (tagi_t *)((char *)t + len);
      void *b = t_end, *end = (char *)b + xtra;

      t = tl_dup(t, tags, &b); p = b;
      assert(t == t_end); assert(b == end); (void)end;
    }
    else
      p = e + 1;

    e->e_event = event;
    e->e_nh = nh ? nua_handle_ref(nh) : nua->nua_dhandle;
    e->e_status = status;
    e->e_phrase = strcpy(p, phrase ? phrase : "");
    if (msg)
      e->e_msg = msg, su_home_threadsafe(msg_home(msg));

    if (su_msg_send(sumsg) != 0 && nh)
      nua_handle_unref(nh);
  }

  return event;
}

/* ----------------------------------------------------------------------
 * Post signal to stack itself
 */
void nua_stack_post_signal(nua_handle_t *nh, nua_event_t event,
			   tag_type_t tag, tag_value_t value, ...)
{
  ta_list ta;
  ta_start(ta, tag, value);
  nua_signal((nh)->nh_nua, nh, NULL, 1, event, 0, NULL, ta_tags(ta));
  ta_end(ta);
}


/* ----------------------------------------------------------------------
 * Receiving events from client
 */
void nua_stack_signal(nua_t *nua, su_msg_r msg, nua_event_data_t *e)
{
  nua_handle_t *nh = e->e_nh;
  tagi_t *tags = e->e_tags;
  nua_event_t event;
  int error = 0;

  assert(tags);

  if (nua_log->log_level >= 7) {
    char const *name = nua_event_name(e->e_event) + 4;
    SU_DEBUG_7(("nua(%p): recv %s\n", (void *)nh, name));
  }

  if (nh) {
    if (!nh->nh_prev)
      nh_append(nua, nh);
    if (!nh->nh_ref_by_stack) {
      /* Mark handle as used by stack */
      nh->nh_ref_by_stack = 1;
      nua_handle_ref(nh);
    }
  }

  if (nua_log->log_level >= 5) {
    char const *name = nua_event_name(e->e_event);
    if (e->e_status == 0)
      SU_DEBUG_5(("nua(%p): signal %s\n", (void *)nh, name + 4));
    else
      SU_DEBUG_5(("nua(%p): signal %s %u %s\n",
		  (void *)nh, name + 4,
		  e->e_status, e->e_phrase ? e->e_phrase : ""));
  }

  su_msg_save(nua->nua_signal, msg);

  event = e->e_event;

  if (nua->nua_shutdown && !e->e_always) {
    /* Shutting down */
    nua_stack_event(nua, nh, NULL, event,
		    901, "Stack is going down",
		    NULL);
  }
  else switch (event) {
  case nua_r_get_params:
    nua_stack_get_params(nua, nh ? nh : nua->nua_dhandle, event, tags);
    break;
  case nua_r_set_params:
    nua_stack_set_params(nua, nh ? nh : nua->nua_dhandle, event, tags);
    break;
  case nua_r_shutdown:
    nua_stack_shutdown(nua);
    break;
  case nua_r_register:
  case nua_r_unregister:
    nua_stack_register(nua, nh, event, tags);
    break;
  case nua_r_invite:
    error = nua_stack_invite(nua, nh, event, tags);
    break;
  case nua_r_cancel:
    error = nua_stack_cancel(nua, nh, event, tags);
    break;
  case nua_r_bye:
    error = nua_stack_bye(nua, nh, event, tags);
    break;
  case nua_r_options:
    error = nua_stack_options(nua, nh, event, tags);
    break;
  case nua_r_refer:
    error = nua_stack_refer(nua, nh, event, tags);
    break;
  case nua_r_publish:
  case nua_r_unpublish:
    error = nua_stack_publish(nua, nh, event, tags);
    break;
  case nua_r_info:
    error = nua_stack_info(nua, nh, event, tags);
    break;
  case nua_r_update:
    error = nua_stack_update(nua, nh, event, tags);
    break;
  case nua_r_message:
    error = nua_stack_message(nua, nh, event, tags);
    break;
  case nua_r_subscribe:
  case nua_r_unsubscribe:
    error = nua_stack_subscribe(nua, nh, event, tags);
    break;
  case nua_r_notify:
    error = nua_stack_notify(nua, nh, event, tags);
    break;
  case nua_r_notifier:
    nua_stack_notifier(nua, nh, event, tags);
    break;
  case nua_r_terminate:
    nua_stack_terminate(nua, nh, event, tags);
    break;
  case nua_r_method:
    error = nua_stack_method(nua, nh, event, tags);
    break;
  case nua_r_authenticate:
    nua_stack_authenticate(nua, nh, event, tags);
    break;
  case nua_r_authorize:
    nua_stack_authorize(nua, nh, event, tags);
    break;
  case nua_r_ack:
    error = nua_stack_ack(nua, nh, event, tags);
    break;
  case nua_r_respond:
    nua_stack_respond(nua, nh, e->e_status, e->e_phrase, tags);
    break;
  case nua_r_destroy: 
    nua_stack_destroy_handle(nua, nh, tags);
    su_msg_destroy(nua->nua_signal);
    return;
  default:
    break;
  }

  if (error < 0) {
    nua_stack_event(nh->nh_nua, nh, NULL, event, NUA_INTERNAL_ERROR, NULL);
  }

  if (su_msg_is_non_null(nua->nua_signal))
    su_msg_destroy(nua->nua_signal);

  if (nh != nua->nua_dhandle)
    nua_handle_unref(nh);
}

/* ====================================================================== */

static int nh_call_pending(nua_handle_t *nh, sip_time_t time);

/**@internal
 * Timer routine.
 *
 * Go through all active handles and execute pending tasks
 */
void nua_stack_timer(nua_t *nua, su_timer_t *t, su_timer_arg_t *a)
{
  nua_handle_t *nh, *nh_next;
  sip_time_t now = sip_now();
  su_root_t *root = su_timer_root(t);

  su_timer_set(t, nua_stack_timer, a);

  if (nua->nua_shutdown) {
    nua_stack_shutdown(nua);
    return;
  }

  for (nh = nua->nua_handles; nh; nh = nh_next) {
    nh_next = nh->nh_next;
    nh_call_pending(nh, now);
    su_root_yield(root);	/* Handle received packets */
  }
}


static
int nh_call_pending(nua_handle_t *nh, sip_time_t now)
{
  nua_dialog_state_t *ds = nh->nh_ds;
  nua_dialog_usage_t *du;
  sip_time_t next = now + NUA_STACK_TIMER_INTERVAL / 1000;

  for (du = ds->ds_usage; du; du = du->du_next) {
    if (now == 0)
      break;
    if (du->du_refresh && du->du_refresh < next)
      break;
  }

  if (du == NULL)
    return 0;

  nua_handle_ref(nh);

  while (du) {
    nua_dialog_usage_t *du_next = du->du_next;

    nua_dialog_usage_refresh(nh, ds, du, now);

    if (du_next == NULL)
      break;

    for (du = nh->nh_ds->ds_usage; du; du = du->du_next)
      if (du == du_next)
	break;

    for (; du; du = du->du_next) {
      if (now == 0)
	break;
      if (du->du_refresh && du->du_refresh < next)
	break;
    }
  }

  nua_handle_unref(nh);

  return 1;
}



/* ====================================================================== */

/**Shutdown a @nua stack.
 *
 * When the @nua stack is shutdown, ongoing calls are released,
 * registrations unregistered, publications un-PUBLISHed and subscriptions
 * terminated. If the stack cannot terminate everything within 30 seconds,
 * it sends the #nua_r_shutdown event with status 500.
 *
 * @param nua         Pointer to @nua stack object
 *
 * @return
 *     nothing
 *
 * @par Related tags:
 *     none
 *
 * @par Events:
 *     #nua_r_shutdown
 *
 * @sa #nua_r_shutdown, nua_destroy(), nua_create(), nua_bye(),
 * nua_unregister(), nua_unpublish(), nua_unsubscribe(), nua_notify(),
 * nua_handle_destroy(), nua_handle_unref()
 */

/** @NUA_EVENT nua_r_shutdown
 *
 * Answer to nua_shutdown().
 *
 * Status codes
 * - 100 shutdown started
 * - 101 shutdown in progress (sent when shutdown has been progressed)
 * - 200 shutdown was successful
 * - 500 shutdown timeout after 30 sec
 *
 * @param status shutdown status code
 * @param nh     NULL
 * @param hmagic NULL
 * @param sip    NULL
 * @param tags   empty
 *
 * @sa nua_shutdown(), nua_destroy()
 *
 * @END_NUA_EVENT
 */

/** @internal Shut down stack. */
void nua_stack_shutdown(nua_t *nua)
{
  nua_handle_t *nh, *nh_next;
  int busy = 0;
  sip_time_t now = sip_now();
  int status;
  char const *phrase;

  enter;

  if (!nua->nua_shutdown)
    nua->nua_shutdown = now;

  for (nh = nua->nua_handles; nh; nh = nh_next) {
    nua_dialog_state_t *ds = nh->nh_ds;
    nua_server_request_t *sr, *sr_next;

    nh_next = nh->nh_next;

    for (sr = ds->ds_sr; sr; sr = sr_next) {
      sr_next = sr->sr_next;

      if (nua_server_request_is_pending(sr)) {
	SR_STATUS1(sr, SIP_410_GONE); /* 410 terminates dialog */
	nua_server_respond(sr, NULL);
	nua_server_report(sr);
      }
    }

    busy += nh_call_pending(nh, 0);

    if (nh->nh_soa) {
      soa_destroy(nh->nh_soa), nh->nh_soa = NULL;
    }

    if (nua_client_request_pending(ds->ds_cr))
      busy++;

    if (nh_notifier_shutdown(nh, NULL, NEATAG_REASON("noresource"), TAG_END()))
      busy++;
  }

  if (!busy)
    SET_STATUS(200, "Shutdown successful");
  else if (now == nua->nua_shutdown)
    SET_STATUS(100, "Shutdown started");
  else if (now - nua->nua_shutdown < 30)
    SET_STATUS(101, "Shutdown in progress");
  else
    SET_STATUS(500, "Shutdown timeout");

  if (status >= 200) {
    for (nh = nua->nua_handles; nh; nh = nh_next) {
      nh_next = nh->nh_next;
      while (nh->nh_ds && nh->nh_ds->ds_usage) {
	nua_dialog_usage_remove(nh, nh->nh_ds, nh->nh_ds->ds_usage);
      }
    }
    su_timer_destroy(nua->nua_timer), nua->nua_timer = NULL;
    nta_agent_destroy(nua->nua_nta), nua->nua_nta = NULL;
  }

  nua_stack_event(nua, NULL, NULL, nua_r_shutdown, status, phrase, NULL);
}

/* ---------------------------------------------------------------------- */

/** @internal Create a handle */
nua_handle_t *nh_create(nua_t *nua, tag_type_t tag, tag_value_t value, ...)
{
  ta_list ta;
  nua_handle_t *nh;

  enter;

  ta_start(ta, tag, value);
  nh = nh_create_handle(nua, NULL, ta_args(ta));
  ta_end(ta);

  if (nh) {
    nh->nh_ref_by_stack = 1;
    nh_append(nua, nh);
  }

  return nh;
}

/** @internal Append a handle to the list of handles */
void nh_append(nua_t *nua, nua_handle_t *nh)
{
  nh->nh_next = NULL;
  nh->nh_prev = nua->nua_handles_tail;
  *nua->nua_handles_tail = nh;
  nua->nua_handles_tail = &nh->nh_next;
}

nua_handle_t *nh_validate(nua_t *nua, nua_handle_t *maybe)
{
  nua_handle_t *nh;

  if (maybe)
    for (nh = nua->nua_handles; nh; nh = nh->nh_next)
      if (nh == maybe)
	return nh;

  return NULL;
}

void nua_stack_destroy_handle(nua_t *nua, nua_handle_t *nh, tagi_t const *tags)
{
  if (nh->nh_notifier)
    nua_stack_terminate(nua, nh, 0, NULL);

  nua_dialog_shutdown(nh, nh->nh_ds);

  if (nh->nh_ref_by_user) {
    nh->nh_ref_by_user = 0;
    nua_handle_unref(nh);
  }

  nh_destroy(nua, nh);
}

#define nh_is_inserted(nh) ((nh)->nh_prev != NULL)

/** @internal Remove a handle from list of handles */
static
void nh_remove(nua_t *nua, nua_handle_t *nh)
{
  assert(nh_is_inserted(nh)); assert(*nh->nh_prev == nh);

  if (nh->nh_next)
    nh->nh_next->nh_prev = nh->nh_prev;
  else
    nua->nua_handles_tail = nh->nh_prev;

  *nh->nh_prev = nh->nh_next;

  nh->nh_prev = NULL;
  nh->nh_next = NULL;
}


void nh_destroy(nua_t *nua, nua_handle_t *nh)
{
  assert(nh); assert(nh != nua->nua_dhandle);

  if (nh->nh_notifier)
    nea_server_destroy(nh->nh_notifier), nh->nh_notifier = NULL;

  nua_dialog_deinit(nh, nh->nh_ds);

  while (nh->nh_ds->ds_cr)
    nua_client_request_destroy(nh->nh_ds->ds_cr);

  while (nh->nh_ds->ds_sr)
    nua_server_request_destroy(nh->nh_ds->ds_sr);

  if (nh->nh_soa)
    soa_destroy(nh->nh_soa), nh->nh_soa = NULL;

  if (nh_is_inserted(nh))
    nh_remove(nua, nh);

  nua_handle_unref(nh);		/* Remove stack reference */
}

/* ======================================================================== */

/**@internal
 * Save handle parameters and initial authentication info.
 *
 * @retval -1 upon an error
 * @retval 0 when successful
 */
int nua_stack_init_handle(nua_t *nua, nua_handle_t *nh,
			  tag_type_t tag, tag_value_t value, ...)
{
  ta_list ta;
  int retval = 0;

  if (nh == NULL)
    return -1;

  assert(nh != nua->nua_dhandle);

  ta_start(ta, tag, value);

  if (nua_stack_set_params(nua, nh, nua_i_error, ta_args(ta)) < 0)
    retval = -1;

  if (!retval && nh->nh_soa)
    if (soa_set_params(nh->nh_soa, ta_tags(ta)) < 0)
      retval = -1;

  ta_end(ta);

  if (retval || nh->nh_init) /* Already initialized? */
    return retval;

  if (nh->nh_tags)
    nh_authorize(nh, TAG_NEXT(nh->nh_tags));

  nh->nh_init = 1;

  return 0;
}

/** @internal Create a handle for processing incoming request */
nua_handle_t *nua_stack_incoming_handle(nua_t *nua,
					nta_incoming_t *irq,
					sip_t const *sip,
					int create_dialog)
{
  nua_handle_t *nh;
  url_t const *url;
  sip_to_t to[1];
  sip_from_t from[1];

  assert(sip && sip->sip_from && sip->sip_to);

  if (sip->sip_contact)
    url = sip->sip_contact->m_url;
  else
    url = sip->sip_from->a_url;

  /* Strip away parameters */
  sip_from_init(from)->a_display = sip->sip_to->a_display;
  *from->a_url = *sip->sip_to->a_url;

  sip_to_init(to)->a_display = sip->sip_from->a_display;
  *to->a_url = *sip->sip_from->a_url;

  nh = nh_create(nua,
		 NUTAG_URL((url_string_t *)url), /* Remote target */
		 SIPTAG_TO(to), /* Local AoR */
		 SIPTAG_FROM(from), /* Remote AoR */
		 TAG_END());

  if (nua_stack_init_handle(nh->nh_nua, nh, TAG_END()) < 0)
    nh_destroy(nua, nh), nh = NULL;

  if (nh && create_dialog) {
    struct nua_dialog_state *ds = nh->nh_ds;

    nua_dialog_store_peer_info(nh, ds, sip);

    ds->ds_leg = nta_leg_tcreate(nua->nua_nta, nua_stack_process_request, nh,
				 SIPTAG_CALL_ID(sip->sip_call_id),
				 SIPTAG_FROM(sip->sip_to),
				 SIPTAG_TO(sip->sip_from),
				 NTATAG_REMOTE_CSEQ(sip->sip_cseq->cs_seq),
				 TAG_END());

    if (!ds->ds_leg || !nta_leg_tag(ds->ds_leg, nta_incoming_tag(irq, NULL)))
      nh_destroy(nua, nh), nh = NULL;
  }

  if (nh)
    nua_dialog_uas_route(nh, nh->nh_ds, sip, 1);

  return nh;
}


/** Set flags and special event on handle.
 *
 * @retval 0 when successful
 * @retval -1 upon an error
 */
int nua_stack_set_handle_special(nua_handle_t *nh,
				 enum nh_kind kind,
				 nua_event_t special)
{
  if (nh == NULL)
    return -1;

  if (special && nh->nh_special && nh->nh_special != special)
    return -1;

  if (!nh_is_special(nh) && !nh->nh_has_invite) {
    switch (kind) {
    case nh_has_invite:    nh->nh_has_invite = 1;    break;
    case nh_has_subscribe: nh->nh_has_subscribe = 1; break;
    case nh_has_notify:    nh->nh_has_notify = 1;    break;
    case nh_has_register:  nh->nh_has_register = 1;  break;
    case nh_has_nothing:
    default:
      break;
    }

    if (special)
      nh->nh_special = special;
  }

  return 0;
}

sip_replaces_t *nua_stack_handle_make_replaces(nua_handle_t *nh, 
					       su_home_t *home,
					       int early_only)
{
  if (nh && nh->nh_ds && nh->nh_ds->ds_leg)
    return nta_leg_make_replaces(nh->nh_ds->ds_leg, home, early_only);
  else
    return NULL;
}

nua_handle_t *nua_stack_handle_by_replaces(nua_t *nua,
					   sip_replaces_t const *r)
{
  if (nua) {
    nta_leg_t *leg = nta_leg_by_replaces(nua->nua_nta, r);
    if (leg)
      return nta_leg_magic(leg, nua_stack_process_request);
  }
  return NULL;
}


/** @internal Add authorization data */
int nh_authorize(nua_handle_t *nh, tag_type_t tag, tag_value_t value, ...)
{
  int retval = 0;
  tagi_t const *ti;
  ta_list ta;

  ta_start(ta, tag, value);

  for (ti = ta_args(ta); ti; ti = tl_next(ti)) {
    if (ti->t_tag == nutag_auth && ti->t_value) {
      char *data = (char *)ti->t_value;
      int rv = auc_credentials(&nh->nh_auth, nh->nh_home, data);

      if (rv > 0) {
	retval = 1;
      }
      else if (rv < 0) {
	retval = -1;
	break;
      }
    }
  }

  ta_end(ta);

  return retval;
}

/**@internal
 * Collect challenges from response.
 *
 * @return Number of updated challenges, 0 if no updates found.
 * @retval -1 upon error.
 */
static
int nh_challenge(nua_handle_t *nh, sip_t const *sip)
{
  int server = 0, proxy = 0;

  if (sip->sip_www_authenticate)
    server = auc_challenge(&nh->nh_auth, nh->nh_home,
			   sip->sip_www_authenticate,
			   sip_authorization_class);

  if (sip->sip_proxy_authenticate)
    proxy = auc_challenge(&nh->nh_auth, nh->nh_home,
			  sip->sip_proxy_authenticate,
			  sip_proxy_authorization_class);

  if (server < 0 || proxy < 0)
    return -1;

  return server + proxy;
}

static inline
int can_redirect(sip_contact_t const *m, sip_method_t method)
{
  if (m && m->m_url->url_host) {
    enum url_type_e type = m->m_url->url_type;
    return
      type == url_sip ||
      type == url_sips ||
      (type == url_tel &&
       (method == sip_method_invite || method == sip_method_message)) ||
      (type == url_im && method == sip_method_message) ||
      (type == url_pres && method == sip_method_subscribe);
  }
  return 0;
}

/* ======================================================================== */
/* Authentication */

/** @NUA_EVENT nua_r_authenticate
 *
 * Response to nua_authenticate(). Under normal operation, this event is
 * never sent but rather the unauthenticated operation is completed. 
 * However, if there is no operation to authentication or if there is an
 * authentication error the #nua_r_authenticate event is sent to the
 * application with the status code as follows:
 * - <i>202 No operation to restart</i>:\n
 *   The authenticator associated with the handle was updated, but there was
 *   no operation to retry with the new credentials.
 * - <i>900 Cannot add credentials</i>:\n
 *   There was internal problem updating authenticator.
 * - <i>904 No matching challenge</i>:\n
 *   There was no challenge matching with the credentials provided by
 *   nua_authenticate(), e.g., their realm did not match with the one 
 *   received with the challenge.
 * 
 * @param status status code from authentication 
 * @param phrase a short textual description of @a status code
 * @param nh     operation handle authenticated
 * @param hmagic application context associated with the handle
 * @param sip    NULL
 * @param tags   empty
 * 
 * @sa nua_terminate(), nua_handle_destroy()
 *
 * @END_NUA_EVENT
 */

void
nua_stack_authenticate(nua_t *nua, nua_handle_t *nh, nua_event_t e,
		       tagi_t const *tags)
{
  int status = nh_authorize(nh, TAG_NEXT(tags));

  if (status > 0) {
    nua_client_request_t *cr = nh->nh_ds->ds_cr;

    if (cr && cr->cr_challenged) {
      nua_client_resend_request(cr, cr->cr_terminating, tags);
    }
    else {
      nua_stack_event(nua, nh, NULL, e, 
		      202, "No operation to restart",
		      NULL);
    }
  }
  else if (status < 0) {
    nua_stack_event(nua, nh, NULL, e, 900, "Cannot add credentials", NULL);
  }
  else {
    nua_stack_event(nua, nh, NULL, e, 904, "No matching challenge", NULL);
  }
}


/* ======================================================================== */
/*
 * Process incoming requests
 */

nua_server_methods_t const *nua_server_methods[] = {
  /* These must be in same order as in sip_method_t */
  &nua_extension_server_methods,
  &nua_invite_server_methods,	/**< INVITE */
  NULL,				/**< ACK */
  NULL,				/**< CANCEL */
  &nua_bye_server_methods,	/**< BYE */
  &nua_options_server_methods,	/**< OPTIONS */
  &nua_register_server_methods,	/**< REGISTER */
  &nua_info_server_methods,	/**< INFO */
  &nua_prack_server_methods,	/**< PRACK */
  &nua_update_server_methods,	/**< UPDATE */
  &nua_message_server_methods,	/**< MESSAGE */
  &nua_subscribe_server_methods,/**< SUBSCRIBE */
  &nua_notify_server_methods,	/**< NOTIFY */
  &nua_refer_server_methods,	/**< REFER */
  &nua_publish_server_methods,	/**< PUBLISH */
  NULL
};


int nua_stack_process_request(nua_handle_t *nh,
			      nta_leg_t *leg,
			      nta_incoming_t *irq,
			      sip_t const *sip)
{
  nua_t *nua = nh->nh_nua;
  sip_method_t method = sip->sip_request->rq_method;
  char const *name = sip->sip_request->rq_method_name;
  nua_server_methods_t const *sm;
  nua_server_request_t *sr, sr0[1];
  int status, initial = 1;

  char const *user_agent = NH_PGET(nh, user_agent);
  sip_supported_t const *supported = NH_PGET(nh, supported);
  sip_allow_t const *allow = NH_PGET(nh, allow);

  enter;

  nta_incoming_tag(irq, NULL);

  /* Hook to outbound */
  if (method == sip_method_options) {
    status = nua_registration_process_request(nua->nua_registrations,
					      irq, sip);
    if (status)
      return status;
  }

  if (nta_check_method(irq, sip, allow,
		       SIPTAG_SUPPORTED(supported),
		       SIPTAG_USER_AGENT_STR(user_agent),
		       TAG_END()))
    return 405;

  switch (sip->sip_request->rq_url->url_type) {
  case url_sip:
  case url_sips:
  case url_im:
  case url_pres:
  case url_tel:
    break;
  default:
    nta_incoming_treply(irq, status = SIP_416_UNSUPPORTED_URI,
			SIPTAG_ALLOW(allow),
			SIPTAG_SUPPORTED(supported),
			SIPTAG_USER_AGENT_STR(user_agent),
			TAG_END());
    return status;
  }

  if (nta_check_required(irq, sip, supported,
			 SIPTAG_ALLOW(allow),
			 SIPTAG_USER_AGENT_STR(user_agent),
			 TAG_END()))
    return 420;

  if (method > sip_method_unknown && method <= sip_method_publish)
    sm = nua_server_methods[method];
  else
    sm = nua_server_methods[0];

  initial = nh == nua->nua_dhandle;

  if (sm == NULL) {
    SU_DEBUG_1(("nua(%p): strange %s from <" URL_PRINT_FORMAT ">\n",
		(void *)nh, sip->sip_request->rq_method_name,
		URL_PRINT_ARGS(sip->sip_from->a_url)));
  }
  else if (initial && sm->sm_flags.in_dialog) {
    /* These must be in-dialog */
    sm = NULL;
  }
  else if (initial && sip->sip_to->a_tag) {
    /* RFC 3261 section 12.2.2:
       
       If the UAS wishes to reject the request because it does not wish to
       recreate the dialog, it MUST respond to the request with a 481
       (Call/Transaction Does Not Exist) status code and pass that to the
       server transaction.
    */
    if (method != sip_method_message || !NH_PGET(nh, win_messenger_enable))
      sm = NULL;
  }

  if (!sm) {
    nta_incoming_treply(irq,
			status = 481, "Call Does Not Exist",
			SIPTAG_ALLOW(allow),
			SIPTAG_SUPPORTED(supported),
			SIPTAG_USER_AGENT_STR(user_agent),
			TAG_END());
    return 481;
  }

  sr = memset(sr0, 0, (sizeof sr0));

  sr->sr_methods = sm;
  sr->sr_method = method = sip->sip_request->rq_method;
  sr->sr_add_contact = sm->sm_flags.add_contact;

  sr->sr_owner = nh;
  sr->sr_initial = initial;

  sr->sr_irq = irq;

  SR_STATUS1(sr, SIP_100_TRYING);

  sr->sr_request.msg = nta_incoming_getrequest(irq);
  sr->sr_request.sip = sip;
  assert(sr->sr_request.msg);

  sr->sr_response.msg = nta_incoming_create_response(irq, 0, NULL);
  sr->sr_response.sip = sip_object(sr->sr_response.msg);

  if (sr->sr_response.msg == NULL) {
    SR_STATUS1(sr, SIP_500_INTERNAL_SERVER_ERROR);
  }
  else if (sm->sm_init && sm->sm_init(sr)) {
    if (sr->sr_status < 200)    /* Init may have set response status */
      SR_STATUS1(sr, SIP_500_INTERNAL_SERVER_ERROR);
  }
  /* Create handle if request does not fail */
  else if (initial && sr->sr_status < 300) {
    if ((nh = nua_stack_incoming_handle(nua, irq, sip, create_dialog)))
      sr->sr_owner = nh;
    else
      SR_STATUS1(sr, SIP_500_INTERNAL_SERVER_ERROR);
  }

  if (sr->sr_status < 300 && sm->sm_preprocess && sm->sm_preprocess(sr)) {
    if (sr->sr_status < 200)    /* Preprocess may have set response status */
      SR_STATUS1(sr, SIP_500_INTERNAL_SERVER_ERROR);
  }

  if (sr->sr_status < 300) {
    if (sm->sm_flags.target_refresh)
      nua_dialog_uas_route(nh, nh->nh_ds, sip, 1); /* Set route and tags */
    nua_dialog_store_peer_info(nh, nh->nh_ds, sip);
  }

  if (sr->sr_status == 100 && method != sip_method_unknown &&
      !sip_is_allowed(NH_PGET(sr->sr_owner, appl_method), method, name)) {
    if (method == sip_method_refer || method == sip_method_subscribe)
      SR_STATUS1(sr, SIP_202_ACCEPTED);
    else
      SR_STATUS1(sr, SIP_200_OK);
  }

  /* INVITE server request is not finalized after 2XX response */
  if (sr->sr_status < (method == sip_method_invite ? 300 : 200)) {
    sr = su_alloc(nh->nh_home, (sizeof *sr));

    if (sr) {
      *sr = *sr0;

      if ((sr->sr_next = nh->nh_ds->ds_sr))
	*(sr->sr_prev = sr->sr_next->sr_prev) = sr,
	  sr->sr_next->sr_prev = &sr->sr_next;
      else
	*(sr->sr_prev = &nh->nh_ds->ds_sr) = sr;
    }
    else {
      sr = sr0;
      SR_STATUS1(sr, SIP_500_INTERNAL_SERVER_ERROR);
    }
  }

  if (sr->sr_status <= 100) {
    SR_STATUS1(sr, SIP_100_TRYING);
    if (method == sip_method_invite || sip->sip_timestamp) {
      nta_incoming_treply(irq, SIP_100_TRYING,
			  SIPTAG_USER_AGENT_STR(user_agent),
			  TAG_END());
    }
  }
  else {
    /* Note that this may change the sr->sr_status */
    nua_server_respond(sr, NULL);
  }

  if (nua_server_report(sr) == 0)
    return 0;

  return 501;
}		 

#undef nua_base_server_init
#undef nua_base_server_preprocess

int nua_base_server_init(nua_server_request_t *sr)
{
  return 0;
}

int nua_base_server_preprocess(nua_server_request_t *sr)
{
  return 0;
}

void nua_server_request_destroy(nua_server_request_t *sr)
{
  if (sr == NULL)
    return;

  if (sr->sr_irq)
    nta_incoming_destroy(sr->sr_irq), sr->sr_irq = NULL;

  if (sr->sr_request.msg)
    msg_destroy(sr->sr_request.msg), sr->sr_request.msg = NULL;

  if (sr->sr_response.msg)
    msg_destroy(sr->sr_response.msg), sr->sr_response.msg = NULL;

  if (sr->sr_prev) {
    /* Allocated from heap */
    if ((*sr->sr_prev = sr->sr_next))
      sr->sr_next->sr_prev = sr->sr_prev;
    su_free(sr->sr_owner->nh_home, sr);
  }
}

/** Respond to a request with given status. 
 *
 * When nua protocol engine receives an incoming SIP request, it can either
 * respond to the request automatically or let it up to application to
 * respond to the request. The automatic answer is sent if the request fails
 * because of method, SIP extension or, in some times, MIME content
 * negotiation fails.
 *
 * When responding to an incoming INVITE request, the nua_respond() can be
 * called without NUTAG_WITH() (or NUTAG_WITH_CURRENT() or
 * NUTAG_WITH_SAVED()). Otherwise, NUTAG_WITH() will contain an indication
 * of the request being responded.
 *
 * In order to simplify the simple applications, most requests are responded
 * automatically. The set of requests always responded by the stack include
 * BYE, CANCEL and NOTIFY. The application can add methods that it likes to
 * handle by itself with NUTAG_APPL_METHOD(). The default set of
 * NUTAG_APPL_METHOD() includes INVITE, PUBLISH, REGISTER and SUBSCRIBE. 
 * Note that unless the method is also included in the set of allowed
 * methods with NUTAG_ALLOW(), the stack will respond to the incoming
 * methods with <i>405 Not Allowed</i>.
 *
 * Note that certain methods are rejected outside a SIP session (created
 * with INVITE transaction). They include BYE, UPDATE, PRACK and INFO. Also
 * the auxiliary methods ACK and CANCEL are rejected by stack if there is no
 * ongoing INVITE transaction corresponding to them.
 *
 * @param nh              Pointer to operation handle
 * @param status          SIP response status (see RFCs of SIP)
 * @param phrase          free text (default response phrase used if NULL)
 * @param tag, value, ... List of tagged parameters
 *
 * @return 
 *    nothing
 *
 * @par Related Tags:
 *    NUTAG_WITH(), NUTAG_WITH_CURRENT(), NUTAG_WITH_SAVED() \n
 *    NUTAG_EARLY_ANSWER() \n
 *    SOATAG_ADDRESS() \n
 *    SOATAG_AF() \n
 *    SOATAG_HOLD() \n
 *    Tags in <sip_tag.h>.
 *
 * @par Events:
 *    #nua_i_state \n
 *    #nua_i_media_error \n
 *    #nua_i_error \n
 *    #nua_i_active \n
 *    #nua_i_terminated \n
 *
 * @sa #nua_i_invite, #nua_i_register, #nua_i_subscribe, #nua_i_publish
 */
void
nua_stack_respond(nua_t *nua, nua_handle_t *nh,
		  int status, char const *phrase, tagi_t const *tags)
{
  nua_server_request_t *sr;
  tagi_t const *t;
  msg_t const *request = NULL;

  t = tl_find_last(tags, nutag_with);

  if (t)
    request = (msg_t const *)t->t_value;

  for (sr = nh->nh_ds->ds_sr; sr; sr = sr->sr_next) {
    if (request && sr->sr_request.msg == request)
      break;
    /* nua_respond() to INVITE can be used without NUTAG_WITH() */
    if (!t && sr->sr_method == sip_method_invite)
      break;
  }

  if (sr == NULL) {
    nua_stack_event(nua, nh, NULL, nua_i_error,
		    500, "Responding to a Non-Existing Request", NULL);
  }
  else if (!nua_server_request_is_pending(sr)) {
    nua_stack_event(nua, nh, NULL, nua_i_error,
		    500, "Already Sent Final Response", NULL);
  }
  else {
    sr->sr_application = status;

    if (tags && nua_stack_set_params(nua, nh, nua_i_none, tags) < 0)
      SR_STATUS1(sr, SIP_500_INTERNAL_SERVER_ERROR);
    else
      sr->sr_status = status, sr->sr_phrase = phrase;

    nua_server_params(sr, tags);
    nua_server_respond(sr, tags);
    nua_server_report(sr);
  }
}

int nua_server_params(nua_server_request_t *sr, tagi_t const *tags)
{
  if (sr->sr_methods->sm_params)
    return sr->sr_methods->sm_params(sr, tags);
  return 0;
}

#undef nua_base_server_params

int nua_base_server_params(nua_server_request_t *sr, tagi_t const *tags) 
{
  return 0;
}

/** Return the response to the client.
 *
 * @retval 0 when successfully sent
 * @retval -1 upon an error
 */
int nua_server_trespond(nua_server_request_t *sr,
			tag_type_t tag, tag_value_t value, ...)
{
  int retval;
  ta_list ta;
  ta_start(ta, tag, value);
  retval = nua_server_respond(sr, ta_args(ta));
  ta_end(ta);
  return retval;
}

/** Return the response to the client.
 *
 * @retval 0 when successfully sent
 * @retval -1 upon an error
 */
int nua_server_respond(nua_server_request_t *sr, tagi_t const *tags)
{
  nua_handle_t *nh = sr->sr_owner;
  sip_method_t method = sr->sr_method;
  struct { msg_t *msg; sip_t *sip; } next = { NULL, NULL };
  tagi_t next_tags[2] = {{ SIPTAG_END() }, { TAG_NEXT(tags) }};
  int retval;

  msg_t *msg = sr->sr_response.msg;
  sip_t *sip = sr->sr_response.sip;
  sip_contact_t *m = sr->sr_request.sip->sip_contact;

  if (sr->sr_response.msg == NULL) {
    assert(sr->sr_status == 500);
    goto internal_error;
  }

  if (sr->sr_status < 200) {
    next.msg = nta_incoming_create_response(sr->sr_irq, 0, NULL);
    next.sip = sip_object(next.msg);
    if (next.sip == NULL)
      SR_STATUS1(sr, SIP_500_INTERNAL_SERVER_ERROR);
  }

  if (nta_incoming_complete_response(sr->sr_irq, msg,
				     sr->sr_status,
				     sr->sr_phrase,
				     TAG_NEXT(tags)) < 0)
    ;
  else if (!sip->sip_supported && NH_PGET(nh, supported) &&
	   sip_add_dup(msg, sip, (sip_header_t *)NH_PGET(nh, supported)) < 0)
    ;
  else if (!sip->sip_user_agent && NH_PGET(nh, user_agent) &&
	   sip_add_make(msg, sip, sip_user_agent_class, 
			NH_PGET(nh, user_agent)) < 0)
    ;
  else if (!sip->sip_organization && NH_PGET(nh, organization) &&
	   sip_add_dup(msg, sip, (void *)NH_PGET(nh, organization)) < 0)
    ;
  else if (!sip->sip_allow && NH_PGET(nh, allow) &&
	   sip_add_dup(msg, sip, (void *)NH_PGET(nh, allow)) < 0)
    ;
  else if (!sip->sip_allow_events && 
	   (method == sip_method_publish || method == sip_method_subscribe) &&
	   NH_PGET(nh, allow_events) &&
	   sip_add_dup(msg, sip, (void *)NH_PGET(nh, allow_events)) < 0)
    ;
  else if (!sip->sip_contact && sr->sr_status < 300 && sr->sr_add_contact &&
	   nua_registration_add_contact_to_response(nh, msg, sip, NULL, m) < 0)
    ;
  else {
    int term;
    
    term = sip_response_terminates_dialog(sr->sr_status, sr->sr_method, NULL);

    sr->sr_terminating = (term < 0) ? -1 : (term > 0 || sr->sr_terminating);

    retval = sr->sr_methods->sm_respond(sr, next_tags);

    if (sr->sr_status < 200) 
      sr->sr_response.msg = next.msg, sr->sr_response.sip = next.sip;
    else if (next.msg)
      msg_destroy(next.msg);

    return retval;
  }

  if (next.msg)
    msg_destroy(next.msg);

  SR_STATUS1(sr, SIP_500_INTERNAL_SERVER_ERROR);

  msg_destroy(msg);

 internal_error:
  sr->sr_response.msg = NULL, sr->sr_response.sip = NULL;
  nta_incoming_treply(sr->sr_irq, sr->sr_status, sr->sr_phrase, TAG_END());

  return 0;
}

/** Return the response to the client.
 *
 * @retval 0 when successfully sent
 * @retval -1 upon an error
 */
int nua_base_server_respond(nua_server_request_t *sr, tagi_t const *tags)
{
  msg_t *response = sr->sr_response.msg;

  sr->sr_response.msg = NULL, sr->sr_response.sip = NULL;

  return nta_incoming_mreply(sr->sr_irq, response);
}

int nua_server_report(nua_server_request_t *sr)
{
  if (sr)
    return sr->sr_methods->sm_report(sr, NULL);
  else
    return 2;
}

int nua_base_server_treport(nua_server_request_t *sr,
			    tag_type_t tag, tag_value_t value,
			    ...)
{
  int retval;
  ta_list ta;
  ta_start(ta, tag, value);
  retval = nua_base_server_report(sr, ta_args(ta));
  ta_end(ta);
  return retval;
}

/**Report request event to the application.
 *
 * @retval 0 request lives
 * @retval 1 request was destroyed
 * @retval 2 request and its usage was destroyed
 * @retval 3 request, all usages and dialog was destroyed
 * @retval 4 request, all usages, dialog, and handle was destroyed
 */
int nua_base_server_report(nua_server_request_t *sr, tagi_t const *tags)
{
  nua_handle_t *nh = sr->sr_owner;
  nua_t *nua = nh->nh_nua;
  nua_dialog_usage_t *usage = sr->sr_usage;
  int initial = sr->sr_initial;
  int status = sr->sr_status;
  char const *phrase = sr->sr_phrase;

  int terminated;
  int handle_can_be_terminated = initial && !sr->sr_event;

  assert(nh);

  if (sr->sr_application) {
    /* There was an error sending response */
    if (sr->sr_application != sr->sr_status)
      nua_stack_event(nua, nh, NULL, nua_i_error, status, phrase, tags);
    sr->sr_application = 0;
  }
  else if (status < 300 && !sr->sr_event) {
    msg_t *msg = msg_ref_create(sr->sr_request.msg);
    nua_event_t e = sr->sr_methods->sm_event;
    sr->sr_event = 1;
    nua_stack_event(nua, nh, msg, e, status, phrase, tags);
  }

  if (status < 200)
    return 0;			/* sr lives on until final response is sent */

  if (sr->sr_method == sip_method_invite && status < 300)
    return 0;			/* INVITE lives on until ACK is received */

  if (initial && 300 <= status)
    terminated = 1;
  else if (sr->sr_terminating && status < 300)
    terminated = 1;
  else
    terminated = sip_response_terminates_dialog(status, sr->sr_method, NULL);

  nua_server_request_destroy(sr);

  if (!terminated)
    return 1;

  if (usage)
    nua_dialog_usage_remove(nh, nh->nh_ds, usage);

  if (!initial) {
    if (terminated > 0)
      return 2;

    /* Remove all usages of the dialog */
    nua_dialog_remove_usages(nh, nh->nh_ds, status, phrase);

    return 3;
  }
  else if (!handle_can_be_terminated) {
    return 3;
  }
  else {
    if (nh != nh->nh_nua->nua_dhandle)
      nh_destroy(nh->nh_nua, nh);

    return 4;
  }
}

/* ---------------------------------------------------------------------- */

/** @class nua_client_request
 *
 * Each handle has a queue of client-side requests; if a request is pending,
 * a new request from API is added to the queue. After the request is
 * complete, it is removed from the queue and destroyed by the default. The
 * exception is the client requests bound to a dialog usage: they are saved
 * and re-used when the dialog usage is refreshed (and sometimes when the
 * usage is terminated).
 * 
 * The client request is subclassed and its behaviour modified using virtual
 * function table in #nua_client_methods_t.
 * 
 * The first three methods (crm_template(), crm_init(), crm_send()) are
 * called when the request is sent first time.
 * 
 * The crm_template() is called if a template request message is needed (for
 * example, in case of unregister, unsubscribe and unpublish, the template
 * message is taken from the request establishing the usage).
 * 
 * The crm_init() is called when the template message and dialog leg has
 * been created and populated by the tags procided by the application. Its
 * parameters msg and sip are pointer to the template request message that
 * is saved in the @ref "cr_msg" nua_client_request::cr_msg field.
 *
 * The crm_send() is called with a copy of the template message that has
 * been populated with all the fields included in the request, including
 * @CSeq and @MaxForwards. The crm_send() function, such as
 * nua_publish_client_request(), usually calls nua_base_client_trequest() that
 * then creates the nta-level transaction.
 *
 * The response to the request is processed by crm_check_restart(), which
 * modifies and restarts the request when needed (e.g., when negotiating
 * expiration time). After the request has been suitably modified, e.g., the
 * expiration time has been increased, the restart function calls
 * nua_client_restart(), which restarts the request and relays the
 * intermediate response to the application with nua_client_restart() and
 * crm_report().
 *
 * The final responses are processed by crm_recv() and and preliminary ones
 * by crm_preliminary(). Both functions call nua_base_client_response() after
 * method-specific processing.
 *
 * The nua_base_client_response() relays the response to the application with
 * nua_client_restart() and crm_report().
 *
 * @par Terminating Dialog Usages and Dialogs
 *
 * The response can be marked as terminating with nua_client_terminating(). 
 * When a terminating request completes the dialog usage is removed and the
 * dialog is destroyed (unless there is an another active usage).
 */
static int nua_client_request_try(nua_client_request_t *cr);
static int nua_client_request_sendmsg(nua_client_request_t *cr,
  				      msg_t *msg, sip_t *sip);

/**Create a client request.
 *
 * @retval 0 if request is pending
 * @retval > 0 if error event has been sent
 * @retval < 0 upon an error
 */
int nua_client_create(nua_handle_t *nh, 
		      int event,
		      nua_client_methods_t const *methods,
		      tagi_t const * const tags)
{
  su_home_t *home = nh->nh_home;
  nua_client_request_t *cr;
  sip_method_t method;
  char const *name;

  method = methods->crm_method, name = methods->crm_method_name;
  if (!name) {
    tagi_t const *t = tl_find_last(tags, nutag_method);
    if (t)
      name = (char const *)t->t_value;
  }

  cr = su_zalloc(home, sizeof *cr + methods->crm_extra);
  if (!cr) {
    return nua_stack_event(nh->nh_nua, nh, 
			   NULL,
			   event,
			   NUA_INTERNAL_ERROR,
			   NULL);
  }

  cr->cr_owner = nh;
  cr->cr_methods = methods;
  cr->cr_event = event;
  cr->cr_method = method;
  cr->cr_method_name = name;
  cr->cr_contactize = methods->crm_flags.target_refresh;
  cr->cr_auto = 1;

  if (su_msg_is_non_null(nh->nh_nua->nua_signal)) {
    nua_event_data_t const *e = su_msg_data(cr->cr_signal);

    if (tags == e->e_tags && event == e->e_event) {
      cr->cr_auto = 0;
      if (tags) {
	su_msg_save(cr->cr_signal, nh->nh_nua->nua_signal);
	cr->cr_tags = tags;
      }
    }
  }

  if (tags && cr->cr_tags == NULL)
    cr->cr_tags = tl_tlist(nh->nh_home, TAG_NEXT(tags));

  if (nua_client_request_queue(cr))
    return 0;

  return nua_client_init_request(cr);
}

int nua_client_tcreate(nua_handle_t *nh, 
		       int event,
		       nua_client_methods_t const *methods,
		       tag_type_t tag, tag_value_t value, ...)
{
  int retval;
  ta_list ta;
  ta_start(ta, tag, value);
  retval = nua_client_create(nh, event, methods, ta_args(ta));
  ta_end(ta);
  return retval;
}

inline int nua_client_request_queue(nua_client_request_t *cr)
{
  int queued = 0;
  nua_client_request_t **queue = &cr->cr_owner->nh_ds->ds_cr;

  assert(cr->cr_prev == NULL && cr->cr_next == NULL);

  cr->cr_status = 0;

  if (cr->cr_method != sip_method_invite &&
      cr->cr_method != sip_method_cancel) {
    while (*queue) {
      if ((*queue)->cr_method == sip_method_invite ||
	  (*queue)->cr_method == sip_method_cancel)
	break;
      queue = &(*queue)->cr_next;
      queued = 1;
    }
  }
  else {
    while (*queue)
      queue = &(*queue)->cr_next;
  }

  if ((cr->cr_next = *queue))
    cr->cr_next->cr_prev = &cr->cr_next;

  cr->cr_prev = queue, *queue = cr;

  return queued;
}

inline nua_client_request_t *nua_client_request_remove(nua_client_request_t *cr)
{
  if (cr->cr_prev)
    if ((*cr->cr_prev = cr->cr_next))
      cr->cr_next->cr_prev = cr->cr_prev;
  cr->cr_prev = NULL, cr->cr_next = NULL;
  return cr;
}

void nua_client_request_destroy(nua_client_request_t *cr)
{
  nua_handle_t *nh;
  
  if (cr == NULL)
    return;

  if (cr->cr_methods->crm_deinit)
    cr->cr_methods->crm_deinit(cr);

  nh = cr->cr_owner;

  su_msg_destroy(cr->cr_signal);

  nua_client_request_remove(cr);
  nua_client_bind(cr, NULL);
  
  if (cr->cr_msg)
    msg_destroy(cr->cr_msg);
  cr->cr_msg = NULL, cr->cr_sip = NULL;

  if (cr->cr_orq)
    nta_outgoing_destroy(cr->cr_orq);

  cr->cr_orq = NULL;

  if (cr->cr_target)
    su_free(nh->nh_home, cr->cr_target);

  su_free(nh->nh_home, cr);
}

/** Bind client request to a dialog usage */ 
int nua_client_bind(nua_client_request_t *cr, nua_dialog_usage_t *du)
{
  assert(cr);
  if (cr == NULL)
    return -1;

  if (du == NULL) {
    if (cr->cr_usage && cr->cr_usage->du_cr == cr)
      cr->cr_usage->du_cr = NULL;
    cr->cr_usage = NULL;
    return 0;
  }

  if (du->du_cr && cr != du->du_cr) {
    assert(!nua_client_is_queued(du->du_cr));
    if (nua_client_is_queued(du->du_cr))
      return -1;
    if (nua_client_is_reporting(du->du_cr)) {
      du->du_cr->cr_usage = NULL;
      du->du_cr = NULL;
    }
    else
      nua_client_request_destroy(du->du_cr);
  }

  du->du_cr = cr, cr->cr_usage = du;

  return 0;
}

/**Initialize client request for sending.
 *
 * This function is called only first time the request is sent.
 *
 * @retval 0 if request is pending
 * @retval >=1 if error event has been sent
 */
int nua_client_init_request(nua_client_request_t *cr)
{
  nua_handle_t *nh = cr->cr_owner;
  nua_t *nua = nh->nh_nua;
  nua_dialog_state_t *ds = nh->nh_ds;
  msg_t *msg = NULL;
  sip_t *sip;
  url_string_t const *url = NULL;
  tagi_t const *t;
  int has_contact = 0;
  int error = 0;
  
  if (!cr->cr_method_name)
    return nua_client_return(cr, NUA_INTERNAL_ERROR, NULL);

  if (cr->cr_msg)
    return nua_client_request_try(cr);

  cr->cr_answer_recv = 0, cr->cr_offer_sent = 0;
  cr->cr_offer_recv = 0, cr->cr_answer_sent = 0;
  cr->cr_terminated = 0, cr->cr_graceful = 0;

  nua_stack_init_handle(nua, nh, TAG_NEXT(cr->cr_tags));

  if (cr->cr_method == sip_method_cancel) {
    if (cr->cr_methods->crm_init) {
      error = cr->cr_methods->crm_init(cr, NULL, NULL, cr->cr_tags);
      if (error)
	return error;
    }

    if (cr->cr_methods->crm_send)
      return cr->cr_methods->crm_send(cr, NULL, NULL, cr->cr_tags);
    else
      return nua_base_client_request(cr, NULL, NULL, cr->cr_tags);
  }

  if (!cr->cr_methods->crm_template ||
      !cr->cr_methods->crm_template(cr, &msg, cr->cr_tags))
    msg = nta_msg_create(nua->nua_nta, 0);

  sip = sip_object(msg);
  if (!sip)
    return nua_client_return(cr, NUA_INTERNAL_ERROR, msg);

  /**@par Populating SIP Request Message with Tagged Arguments
   *
   * The tagged arguments can be used to pass values for any SIP headers
   * to the stack. When the INVITE message (or any other SIP message) is
   * created, the tagged values saved with nua_handle() are used first,
   * next the tagged values given with the operation (nua_invite()) are
   * added.
   *
   * When multiple tags for the same header are specified, the behaviour
   * depends on the header type. If only a single header field can be
   * included in a SIP message, the latest non-NULL value is used, e.g.,
   * @Subject. However, if the SIP header can consist of multiple lines or
   * header fields separated by comma, e.g., @Accept, all the tagged
   * values are concatenated.
   *
   * However, if a tag value is #SIP_NONE (-1 casted as a void pointer),
   * the values from previous tags are ignored.
   */
  if (nh->nh_tags) {
    for (t = nh->nh_tags; t; t = t_next(t)) {
      if (t->t_tag == siptag_contact ||
	  t->t_tag == siptag_contact_str)
	has_contact = 1;
      else if (t->t_tag == nutag_url)
	url = (url_string_t const *)t->t_value;
    }

    t = nh->nh_tags, sip_add_tagis(msg, sip, &t);
  }

  for (t = cr->cr_tags; t; t = t_next(t)) {
    if (t->t_tag == siptag_contact ||
	t->t_tag == siptag_contact_str)
      has_contact = 1;
    else if (t->t_tag == nutag_url)
      url = (url_string_t const *)t->t_value;
    else if (t->t_tag == nutag_auth && t->t_value) {
      /* XXX ignoring errors */
      if (nh->nh_auth)
	auc_credentials(&nh->nh_auth, nh->nh_home, (char *)t->t_value);
    }
  }

  if (cr->cr_method == sip_method_register && url == NULL)
    url = (url_string_t const *)NH_PGET(nh, registrar);
  
  if ((t = cr->cr_tags)) {
    if (sip_add_tagis(msg, sip, &t) < 0)
      goto error;
  }

  /**
   * Now, the target URI for the request needs to be determined.
   *
   * For initial requests, values from tags are used. If NUTAG_URL() is
   * given, it is used as target URI. Otherwise, if SIPTAG_TO() is given,
   * it is used as target URI. If neither is given, the complete request
   * line already specified using SIPTAG_REQUEST() or SIPTAG_REQUEST_STR()
   * is used. At this point, the target URI is stored in the request line,
   * together with method name and protocol version ("SIP/2.0"). The
   * initial dialog information is also created: @CallID, @CSeq headers
   * are generated, if they do not exist, and a tag is added to the @From
   * header.
   */

  if (!ds->ds_leg) {
    if (ds->ds_remote_tag && ds->ds_remote_tag[0] && 
	sip_to_tag(nh->nh_home, sip->sip_to, ds->ds_remote_tag) < 0)
      goto error;

    if (sip->sip_from == NULL && 
	sip_add_dup(msg, sip, (sip_header_t *)nua->nua_from) < 0)
      goto error;

    if (cr->cr_methods->crm_flags.create_dialog) {
      ds->ds_leg = nta_leg_tcreate(nua->nua_nta,
				   nua_stack_process_request, nh,
				   SIPTAG_CALL_ID(sip->sip_call_id),
				   SIPTAG_FROM(sip->sip_from),
				   SIPTAG_TO(sip->sip_to),
				   SIPTAG_CSEQ(sip->sip_cseq),
				   TAG_END());
      if (!ds->ds_leg)
	goto error;

      if (!sip->sip_from->a_tag &&
	  sip_from_tag(msg_home(msg), sip->sip_from,
		       nta_leg_tag(ds->ds_leg, NULL)) < 0)
	goto error;
    }
  }
  else {
    if (ds->ds_route)
      url = NULL;
  }

  if (url && nua_client_set_target(cr, (url_t *)url) < 0)
    goto error;

  cr->cr_has_contact = has_contact;

  if (cr->cr_methods->crm_init) {
    error = cr->cr_methods->crm_init(cr, msg, sip, cr->cr_tags);
    if (error < -1)
      msg = NULL;
    if (error < 0)
      goto error;
    if (error != 0)
      return error;
  }

  cr->cr_msg = msg;
  cr->cr_sip = sip;

  return nua_client_request_try(cr);

 error:
  return nua_client_return(cr, NUA_INTERNAL_ERROR, msg);
}


/** Resend the request message.
 *
 * @retval 0 if request is pending
 * @retval >=1 if error event has been sent
 */
int nua_client_resend_request(nua_client_request_t *cr,
			      int terminating,
			      tagi_t const *tags)
{
  if (cr) {
    if (tags && cr->cr_msg)
      if (sip_add_tagis(cr->cr_msg, NULL, &tags) < 0)
	/* XXX */;

    cr->cr_retry_count = 0;
    cr->cr_terminating = terminating;

    if (!nua_client_is_queued(cr)) {
      if (nua_client_request_queue(cr))
	return 0;
      if (nua_client_is_reporting(cr))
	return 0;
    }

    return nua_client_request_try(cr);
  }
  return 0;
}


/** Create a request message and send it.
 *
 * If an error occurs, send error event to the application.
 *
 * @retval 0 if request is pending
 * @retval >=1 if error event has been sent
 */
static
int nua_client_request_try(nua_client_request_t *cr)
{
  int error = -1;
  msg_t *msg = msg_copy(cr->cr_msg);
  sip_t *sip = sip_object(msg);

  cr->cr_answer_recv = 0, cr->cr_offer_sent = 0;
  cr->cr_offer_recv = 0, cr->cr_answer_sent = 0;
  cr->cr_terminated = 0, cr->cr_graceful = 0;

  if (msg && sip) {
    error = nua_client_request_sendmsg(cr, msg, sip);
    if (!error)
      return 0;

    if (error == -1)
      msg_destroy(msg);
  }

  if (error < 0)
    error = nua_client_response(cr, NUA_INTERNAL_ERROR, NULL);

  assert(error > 0);
  return error;
}

/**Send a request message.
 *
 * @retval 0 if request is pending
 * @retval >=1 if error event has been sent
 * @retval -1 if error occurred but event has not been sent,
               and @a msg has not been destroyed
 * @retval -2 if error occurred, event has not been sent,
 *            but @a msg has been destroyed
 */
static
int nua_client_request_sendmsg(nua_client_request_t *cr, msg_t *msg, sip_t *sip)
{
  nua_handle_t *nh = cr->cr_owner;
  nua_dialog_state_t *ds = nh->nh_ds;
  sip_method_t method = cr->cr_method;
  char const *name = cr->cr_method_name;
  url_string_t const *url = (url_string_t *)cr->cr_target;
  nta_leg_t *leg;

  assert(cr->cr_orq == NULL);

  if (ds->ds_leg)
    leg = ds->ds_leg;
  else
    leg = nh->nh_nua->nua_dhandle->nh_ds->ds_leg; /* Default leg */

  if (nua_dialog_is_established(ds)) {
    while (sip->sip_route)
      sip_route_remove(msg, sip);
  }
  
  /**
   * For in-dialog requests, the request URI is taken from the @Contact
   * header received from the remote party during dialog establishment, 
   * and the NUTAG_URL() is ignored.
   *
   * Also, the @CallID and @CSeq headers and @From and @To tags are
   * generated based on the dialog information and added to the request. 
   * If the dialog has a route, it is added to the request, too.
   */
  if (nta_msg_request_complete(msg, leg, method, name, url) < 0)
    return -1;

  /**@MaxForwards header (with default value set by NTATAG_MAX_FORWARDS()) is
   * also added now, if it does not exist.
   */

  if (!ds->ds_remote)
    ds->ds_remote = sip_to_dup(nh->nh_home, sip->sip_to);
  if (!ds->ds_local)
    ds->ds_local = sip_from_dup(nh->nh_home, sip->sip_from);

  /**
   * Next, values previously set with nua_set_params() or nua_set_hparams()
   * are used: @Allow, @Supported, @Organization, and @UserAgent headers are
   * added to the request if they are not already set. 
   */
  if (!sip->sip_allow && !ds->ds_route)
    sip_add_dup(msg, sip, (sip_header_t*)NH_PGET(nh, allow));
  
  if (!sip->sip_supported && NH_PGET(nh, supported))
    sip_add_dup(msg, sip, (sip_header_t *)NH_PGET(nh, supported));
  
  if (method == sip_method_register && NH_PGET(nh, path_enable) &&
      !sip_has_feature(sip->sip_supported, "path") &&
      !sip_has_feature(sip->sip_require, "path"))
    sip_add_make(msg, sip, sip_supported_class, "path");
  
  if (!sip->sip_organization && NH_PGET(nh, organization))
    sip_add_dup(msg, sip, (sip_header_t *)NH_PGET(nh, organization));

  if (!sip->sip_user_agent && NH_PGET(nh, user_agent))
    sip_add_make(msg, sip, sip_user_agent_class, NH_PGET(nh, user_agent));

  /**
   * Next, the stack generates a @Contact header for the request (unless
   * the application already gave a @Contact header or it does not want to
   * use @Contact and indicates that by including SIPTAG_CONTACT(NULL) or
   * SIPTAG_CONTACT(SIP_NONE) in the tagged parameters.) If the
   * application has registered the URI in @From header, the @Contact
   * header used with registration is used. Otherwise, the @Contact header
   * is generated from the local IP address and port number.
   */

  /**For the initial requests, @ServiceRoute set that was received from the
   * registrar is also added to the request message.
   */
  if (cr->cr_method != sip_method_register) {
    if (nua_registration_add_contact_to_request(nh, msg, sip,
						cr->cr_contactize &&
						!cr->cr_has_contact,
						!ds->ds_route) < 0)
      return -1;
  }

  cr->cr_challenged = 0;

  if (cr->cr_methods->crm_send)
    return cr->cr_methods->crm_send(cr, msg, sip, NULL);

  return nua_base_client_request(cr, msg, sip, NULL);
}

/**Add tags to request message and send it, 
 *
 * @retval 0 success
 * @retval -1 if error occurred, but event has not been sent
 * @retval -2 if error occurred, event has not been sent,
 *            and @a msg has been destroyed
 * @retval >=1 if error event has been sent
 */
int nua_base_client_trequest(nua_client_request_t *cr,
			     msg_t *msg, sip_t *sip,
			     tag_type_t tag, tag_value_t value, ...)
{
  int retval;
  ta_list ta;
  ta_start(ta, tag, value);
  retval = nua_base_client_request(cr, msg, sip, ta_args(ta));
  ta_end(ta);
  return retval;
}

/** Send request.
 *
 * @retval 0 success
 * @retval -1 if error occurred, but event has not been sent
 * @retval -2 if error occurred, event has not been sent,
 *            and @a msg has been destroyed
 * @retval >=1 if error event has been sent
 */
int nua_base_client_request(nua_client_request_t *cr, msg_t *msg, sip_t *sip,
			    tagi_t const *tags)
{
  nua_handle_t *nh = cr->cr_owner;

  if (nh->nh_auth && auc_authorize(&nh->nh_auth, msg, sip) < 0)
    return nua_client_return(cr, 900, "Cannot add credentials", msg);

  cr->cr_seq = sip->sip_cseq->cs_seq; /* Save last sequence number */

  cr->cr_orq = nta_outgoing_mcreate(nh->nh_nua->nua_nta,
				    nua_client_orq_response, cr,
				    NULL, 
				    msg,
				    TAG_NEXT(tags));

  return cr->cr_orq ? 0 : -1;
}

/** Callback for nta client transaction */
int nua_client_orq_response(nua_client_request_t *cr,
			    nta_outgoing_t *orq,
			    sip_t const *sip)
{
  int status;
  char const *phrase;

  if (sip && sip->sip_status) {
    status = sip->sip_status->st_status;
    phrase = sip->sip_status->st_phrase;
  }
  else {
    status = nta_outgoing_status(orq);
    phrase = "";
  }

  nua_client_response(cr, status, phrase, sip);

  return 0;
}

/**Return response to the client request.
 *
 * Return a response generated by the stack. This function is used to return
 * a error response within @a nua_client_methods_t#crm_init or @a
 * nua_client_methods_t#crm_send functions. It takes care of disposing the @a
 * to_be_destroyed that cannot be sent.
 *
 * @retval 0 if response event was preliminary
 * @retval 1 if response event was final
 * @retval 2 if response event destroyed the handle, too.
 */
int nua_client_return(nua_client_request_t *cr,
		      int status,
		      char const *phrase,
		      msg_t *to_be_destroyed)
{
  if (to_be_destroyed)
    msg_destroy(to_be_destroyed);
  nua_client_response(cr, status, phrase, NULL);
  return 1;
}

/** Process response to the client request.
 *
 * The response can be generated by the stack (@a sip is NULL) or
 * returned by the remote server.
 *
 * @retval 0 if response event was preliminary
 * @retval 1 if response event was final
 * @retval 2 if response event destroyed the handle, too.
 */
int nua_client_response(nua_client_request_t *cr,
			int status,
			char const *phrase,
			sip_t const *sip)
{
  nua_handle_t *nh = cr->cr_owner;
  nua_dialog_usage_t *du = cr->cr_usage;

  if (cr->cr_restarting)
    return 0;

  cr->cr_status = status;

  if (status < 200) {
    /* Xyzzy */
  }
  else if (sip && nua_client_check_restart(cr, status, phrase, sip)) {
    return 0;
  }
  else if (status < 300) {
    if (cr->cr_terminating) {
      cr->cr_terminated = 1;
    }
    else {
      if (sip) {
	if (cr->cr_methods->crm_flags.target_refresh)
	  nua_dialog_uac_route(nh, nh->nh_ds, sip, 1);
	nua_dialog_store_peer_info(nh, nh->nh_ds, sip);
      }

      if (du && du->du_cr == cr)
	du->du_ready = 1;
    }
  }
  else {
    sip_method_t method = cr->cr_method;
    int terminated, graceful = 1;

    if (status < 700)
      terminated = sip_response_terminates_dialog(status, method, &graceful);
    else
      /* XXX - terminate usage by all internal error responses */
      terminated = 0, graceful = 1;

    if (terminated < 0)
      cr->cr_terminated = terminated;      
    else if (cr->cr_terminating)
      cr->cr_terminated = 1;
    else if (terminated && graceful)
      cr->cr_graceful = graceful;
    else if (terminated)
      cr->cr_terminated = 1;
  }
  
  if (status < 200) {
    if (cr->cr_methods->crm_preliminary)
      cr->cr_methods->crm_preliminary(cr, status, phrase, sip);
    else
      nua_base_client_response(cr, status, phrase, sip, NULL);
    return 0;
  }  

  if (cr->cr_methods->crm_recv)
    return cr->cr_methods->crm_recv(cr, status, phrase, sip);
  else
    return nua_base_client_response(cr, status, phrase, sip, NULL);
}

/** Check if request should be restarted.
 *
 * @retval 1 if restarted or waring for restart
 * @retval 0 otherwise
 */
int nua_client_check_restart(nua_client_request_t *cr,
			     int status,
			     char const *phrase,
			     sip_t const *sip)
{
  nua_handle_t *nh = cr->cr_owner;

  assert(cr && status >= 200 && phrase && sip);

  if (cr->cr_retry_count >= NH_PGET(nh, retry_count))
    return 0;

  if (cr->cr_methods->crm_check_restart)
    return cr->cr_methods->crm_check_restart(cr, status, phrase, sip);
  else
    return nua_base_client_check_restart(cr, status, phrase, sip);
}

int nua_base_client_check_restart(nua_client_request_t *cr,
				  int status,
				  char const *phrase,
				  sip_t const *sip)
{
  nua_handle_t *nh = cr->cr_owner; 

  /* XXX - handle Retry-After */

  if (status == 302) {
    if (!can_redirect(sip->sip_contact, cr->cr_method))
      return 0;

    if (nua_client_set_target(cr, sip->sip_contact->m_url) >= 0)
      return nua_client_restart(cr, 100, "Redirected");
  }

  if (status == 423) {
    unsigned my_expires = 0;
    
    if (cr->cr_sip->sip_expires)
      my_expires = cr->cr_sip->sip_expires->ex_delta;

    if (sip->sip_min_expires &&
	sip->sip_min_expires->me_delta > my_expires) {
      sip_expires_t ex[1];
      sip_expires_init(ex);
      ex->ex_delta = sip->sip_min_expires->me_delta;

      if (sip_add_dup(cr->cr_msg, NULL, (sip_header_t *)ex) < 0)
	return 0;

      return nua_client_restart(cr, 100, "Re-Negotiating Expiration");
    }
  }

  if (((status == 401 && sip->sip_www_authenticate) ||
       (status == 407 && sip->sip_proxy_authenticate)) &&
      nh_challenge(nh, sip) > 0) {
    nta_outgoing_t *orq;
    if (auc_has_authorization(&nh->nh_auth)) 
      return nua_client_restart(cr, 100, "Request Authorized by Cache");

    orq = cr->cr_orq, cr->cr_orq = NULL;
    cr->cr_challenged = 1;
    cr->cr_retry_count++;
    nua_client_report(cr, status, phrase, NULL, orq, NULL);
    nta_outgoing_destroy(orq);

    return 1;
  }

  return 0;  /* This was a final response that cannot be restarted. */
}

/** Restart request.
 *
 * @retval 1 if restarted
 * @retval 0 otherwise
 */
int nua_client_restart(nua_client_request_t *cr,
		       int status, char const *phrase)
{
  nua_handle_t *nh = cr->cr_owner;
  nta_outgoing_t *orq;
  int error = -1, terminated, graceful;
  msg_t *msg;
  sip_t *sip;

  if (++cr->cr_retry_count > NH_PGET(nh, retry_count))
    return 0;

  orq = cr->cr_orq, cr->cr_orq = NULL;  assert(orq);
  terminated = cr->cr_terminated, cr->cr_terminated = 0;
  graceful = cr->cr_graceful, cr->cr_graceful = 0;

  msg = msg_copy(cr->cr_msg);
  sip = sip_object(msg);

  if (msg && sip) {
    cr->cr_restarting = 1;
    error = nua_client_request_sendmsg(cr, msg, sip);
    cr->cr_restarting = 0;
    if (error !=0 && error != -2)
      msg_destroy(msg);
  }


  if (error) {
    cr->cr_graceful = graceful;
    cr->cr_terminated = terminated;
    assert(cr->cr_orq == NULL);
    cr->cr_orq = orq;
    return 0;
  }

  nua_client_report(cr, status, phrase, NULL, orq, NULL);

  nta_outgoing_destroy(orq);

  return 1;
}

int nua_client_set_target(nua_client_request_t *cr, url_t const *target)
{
  url_t *new_target, *old_target = cr->cr_target;

  if (!target || target == old_target)
    return 0;

  new_target = url_hdup(cr->cr_owner->nh_home, (url_t *)target);
  if (!new_target)
    return -1;
  cr->cr_target = new_target;
  if (old_target)
    su_free(cr->cr_owner->nh_home, old_target);

  return 0;
}

/**@internal
 * Relay response event to the application.
 *
 * @todo 
 * If handle has already been marked as destroyed by nua_handle_destroy(),
 * release the handle with nh_destroy().
 *
 * @retval 0 if event was preliminary
 * @retval 1 if event was final
 * @retval 2 if event destroyed the handle, too.
 */
int nua_base_client_tresponse(nua_client_request_t *cr,
			      int status, char const *phrase,
			      sip_t const *sip,
			      tag_type_t tag, tag_value_t value, ...)
{
  ta_list ta;
  int retval;

  if (cr->cr_event == nua_r_destroy)
    return nua_base_client_response(cr, status, phrase, sip, NULL);

  ta_start(ta, tag, value);
  retval = nua_base_client_response(cr, status, phrase, sip, ta_args(ta));
  ta_end(ta);

  return retval;
}

/**@internal
 * Relay response event to the application.
 *
 * @todo 
 * If handle has already been marked as destroyed by nua_handle_destroy(),
 * release the handle with nh_destroy().
 *
 * @retval 0 if event was preliminary
 * @retval 1 if event was final
 * @retval 2 if event destroyed the handle, too.
 */
int nua_base_client_response(nua_client_request_t *cr,
			     int status, char const *phrase,
			     sip_t const *sip,
			     tagi_t const *tags)
{
  nua_handle_t *nh = cr->cr_owner;
  int next;

  cr->cr_reporting = 1;

  if (status >= 200 && status < 300)
    nh_challenge(nh, sip);  /* Collect nextnonce */

  nua_client_report(cr, status, phrase, sip, cr->cr_orq, tags);

  if (status >= 200)
    nua_client_request_remove(cr);

  if (cr->cr_method == sip_method_invite ? status < 300 : status < 200) {
    cr->cr_reporting = 0;
    return 1;
  }

  if (cr->cr_orq)
    nta_outgoing_destroy(cr->cr_orq), cr->cr_orq = NULL;

  if (cr->cr_usage) {
    nua_dialog_usage_t *du = cr->cr_usage;

    if (cr->cr_terminated < 0)
      /* XXX - dialog has been terminated */;

    if (cr->cr_terminated ||
	(!du->du_ready && status >= 300 && nua_client_is_bound(cr))) {
      /* Usage has been destroyed */
      nua_dialog_usage_remove(nh, nh->nh_ds, du);
    }
    else if (cr->cr_graceful) {
      /* Terminate usage gracefully */
      nua_dialog_usage_shutdown(nh, nh->nh_ds, du); 
    }
  }

  cr->cr_reporting = 0;

  if (nua_client_is_queued(cr))
    return 1;

  next = cr->cr_method != sip_method_invite && cr->cr_method != sip_method_cancel;

  if (!nua_client_is_bound(cr))
    nua_client_request_destroy(cr);

  if (next && nh->nh_ds->ds_cr != NULL && nh->nh_ds->ds_cr != cr) {
    cr = nh->nh_ds->ds_cr;
    if (cr->cr_method != sip_method_invite && cr->cr_method != sip_method_cancel)
      nua_client_init_request(cr);
  }

  return 1;
}

/** Send event, zap transaction but leave cr in list */
int nua_client_report(nua_client_request_t *cr,
		      int status, char const *phrase,
		      sip_t const *sip,
		      nta_outgoing_t *orq,
		      tagi_t const *tags)
{
  nua_handle_t *nh;

  if (cr->cr_event == nua_r_destroy)
    return 1;

  if (cr->cr_methods->crm_report)
    return cr->cr_methods->crm_report(cr, status, phrase, sip, orq, tags);

  nh = cr->cr_owner;
  
  nua_stack_event(nh->nh_nua, nh, 
		  nta_outgoing_getresponse(orq),
		  cr->cr_event,
		  status, phrase,
		  tags);
  return 1;
}

int nua_client_treport(nua_client_request_t *cr,
		       int status, char const *phrase,
		       sip_t const *sip,
		       nta_outgoing_t *orq,
		       tag_type_t tag, tag_value_t value, ...)
{
  int retval;
  ta_list ta;
  ta_start(ta, tag, value);
  retval = nua_client_report(cr, status, phrase, sip, orq, ta_args(ta));
  ta_end(ta);
  return retval;
}

nua_client_request_t *
nua_client_request_pending(nua_client_request_t const *cr)
{
  for (;cr;cr = cr->cr_next)
    if (cr->cr_orq)
      return (nua_client_request_t *)cr;

  return NULL;
}
