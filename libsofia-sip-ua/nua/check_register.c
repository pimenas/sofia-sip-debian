/*
 * This file is part of the Sofia-SIP package
 *
 * Copyright (C) 2008 Nokia Corporation.
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

/**@CFILE check_register.c
 *
 * @brief Check-driven tester for Sofia SIP User Agent library
 *
 * @author Pekka Pessi <Pekka.Pessi@nokia.com>
 *
 * @copyright (C) 2008 Nokia Corporation.
 */

#include "config.h"

#undef NDEBUG

#include "check_nua.h"

#include <sofia-sip/sip_status.h>
#include <sofia-sip/sip_header.h>
#include <sofia-sip/soa.h>
#include <sofia-sip/su_tagarg.h>
#include <sofia-sip/su_tag_io.h>
#include <sofia-sip/su_log.h>

#include <stdlib.h>
#include <string.h>
#include <assert.h>

SOFIAPUBVAR su_log_t tport_log[];

static nua_t *nua;

static void register_setup(void)
{
  nua = s2_nua_setup("register", TAG_END());
}

static void register_thread_setup(void)
{
  s2_nua_thread = 1;
  register_setup();
}

static void register_threadless_setup(void)
{
  s2_nua_thread = 0;
  register_setup();
}

static void pingpong_setup(void)
{
  nua = s2_nua_setup("register with pingpong",
		     TPTAG_PINGPONG(20000),
		     TPTAG_KEEPALIVE(10000),
		     TAG_END());
  tport_set_params(s2sip->tcp.tport, TPTAG_PONG2PING(1), TAG_END());
}

static void pingpong_thread_setup(void)
{
  s2_nua_thread = 1;
  pingpong_setup();
}

static void pingpong_threadless_setup(void)
{
  s2_nua_thread = 0;
  pingpong_setup();
}

static void register_teardown(void)
{
  s2_teardown_started("register");
  nua_shutdown(nua);
  fail_unless_event(nua_r_shutdown, 200);
  s2_nua_teardown();
}

static void add_register_fixtures(TCase *tc, int threading, int pingpong)
{
  void (*setup)(void);

  if (pingpong)
    setup = threading ? pingpong_thread_setup : pingpong_threadless_setup;
  else
    setup = threading ? register_thread_setup : register_threadless_setup;

  tcase_add_checked_fixture(tc, setup, register_teardown);
}

/* ---------------------------------------------------------------------- */

START_TEST(register_1_0_1)
{
  nua_handle_t *nh = nua_handle(nua, NULL, TAG_END());
  struct message *m;

  S2_CASE("1.0.1", "Failed Register", "REGISTER returned 403 response");

  nua_register(nh, TAG_END());

  fail_unless((m = s2_sip_wait_for_request(SIP_METHOD_REGISTER)) != NULL, NULL);

  s2_sip_respond_to(m, NULL,
		SIP_403_FORBIDDEN,
		TAG_END());
  s2_sip_free_message(m);

  nua_handle_destroy(nh);

} END_TEST


START_TEST(register_1_1_1)
{
  S2_CASE("1.1.1", "Basic Register", "REGISTER returning 200 OK");

  s2_register_setup();

  s2_register_teardown();

} END_TEST


START_TEST(register_1_1_2)
{
  nua_handle_t *nh;
  struct message *m;

  S2_CASE("1.1.2", "Register with dual authentication",
	  "Register, authenticate");

  nh = nua_handle(nua, NULL, TAG_END());

  nua_register(nh, TAG_END());

  m = s2_sip_wait_for_request(SIP_METHOD_REGISTER); fail_if(!m);
  s2_sip_respond_to(m, NULL,
		SIP_407_PROXY_AUTH_REQUIRED,
		SIPTAG_PROXY_AUTHENTICATE_STR(s2_auth_digest_str),
		TAG_END());
  s2_sip_free_message(m);
  fail_unless_event(nua_r_register, 407);

  nua_authenticate(nh, NUTAG_AUTH(s2_auth_credentials), TAG_END());

  m = s2_sip_wait_for_request(SIP_METHOD_REGISTER); fail_if(!m);
  s2_sip_respond_to(m, NULL,
		SIP_401_UNAUTHORIZED,
		SIPTAG_WWW_AUTHENTICATE_STR(s2_auth2_digest_str),
		SIPTAG_PROXY_AUTHENTICATE_STR(s2_auth_digest_str),
		TAG_END());
  s2_sip_free_message(m);
  fail_unless_event(nua_r_register, 401);

  nua_authenticate(nh, NUTAG_AUTH(s2_auth2_credentials), TAG_END());

  m = s2_sip_wait_for_request(SIP_METHOD_REGISTER);
  fail_if(!m);
  fail_if(!m->sip->sip_authorization);
  fail_if(!m->sip->sip_proxy_authorization);
  s2_save_register(m);

  s2_sip_respond_to(m, NULL,
		SIP_200_OK,
		SIPTAG_CONTACT(s2->registration->contact),
		TAG_END());
  s2_sip_free_message(m);

  assert(s2->registration->contact != NULL);
  fail_unless_event(nua_r_register, 200);

  s2->registration->nh = nh;

  s2_register_teardown();

} END_TEST

/* ---------------------------------------------------------------------- */

static char const receive_natted[] = "received=4.255.255.9";
static char const receive_natted2[] = "received=4.255.255.10";

/* Return Via that looks very natted */
static sip_via_t *natted_via(struct message *m, const char *receive_natted)
{
  su_home_t *h;
  sip_via_t *via;

  h = msg_home(m->msg);
  via = sip_via_dup(h, m->sip->sip_via);
  msg_header_replace_param(h, via->v_common, receive_natted);

  if (via->v_protocol == sip_transport_udp)
    msg_header_replace_param(h, via->v_common, "rport=9");

  if (via->v_protocol == sip_transport_tcp && via->v_rport) {
    tp_name_t const *tpn = tport_name(m->tport);
    char *rport = su_sprintf(h, "rport=%s", tpn->tpn_port);
    msg_header_replace_param(h, via->v_common, rport);
  }

  return via;
}

/* ---------------------------------------------------------------------- */

START_TEST(register_1_2_1) {
  nua_handle_t *nh;
  struct message *m;

  S2_CASE("1.2.1", "Register behind NAT",
	  "Register through NAT, detect NAT, re-REGISTER");

  nh = nua_handle(nua, NULL, TAG_END());

  nua_register(nh, TAG_END());

  m = s2_sip_wait_for_request(SIP_METHOD_REGISTER);
  fail_if(!m);
  fail_if(!m->sip->sip_contact || m->sip->sip_contact->m_next);
  s2_save_register(m);

  s2_sip_respond_to(m, NULL,
		SIP_200_OK,
		SIPTAG_CONTACT(s2->registration->contact),
		SIPTAG_VIA(natted_via(m, receive_natted)),
		TAG_END());
  s2_sip_free_message(m);

  assert(s2->registration->contact != NULL);
  fail_unless_event(nua_r_register, 100);

  m = s2_sip_wait_for_request(SIP_METHOD_REGISTER);
  fail_if(!m);
  fail_if(!m->sip->sip_contact || !m->sip->sip_contact->m_next);
  s2_save_register(m);

  s2_sip_respond_to(m, NULL,
		SIP_200_OK,
		SIPTAG_CONTACT(s2->registration->contact),
		SIPTAG_VIA(natted_via(m, receive_natted)),
		TAG_END());
  s2_sip_free_message(m);

  fail_unless(s2->registration->contact != NULL);
  fail_if(s2->registration->contact->m_next != NULL);
  fail_unless_event(nua_r_register, 200);

  s2->registration->nh = nh;

  s2_register_teardown();

} END_TEST


static nua_handle_t *make_auth_natted_register(
  nua_handle_t *nh,
  tag_type_t tag, tag_value_t value, ...)
{
  struct message *m;

  ta_list ta;
  ta_start(ta, tag, value);
  nua_register(nh, ta_tags(ta));
  ta_end(ta);

  m = s2_sip_wait_for_request(SIP_METHOD_REGISTER); fail_if(!m);
  s2_sip_respond_to(m, NULL,
		SIP_401_UNAUTHORIZED,
		SIPTAG_WWW_AUTHENTICATE_STR(s2_auth_digest_str),
		SIPTAG_VIA(natted_via(m, receive_natted)),
		TAG_END());
  s2_sip_free_message(m);

  fail_unless_event(nua_r_register, 401);

  /* NAT detected event */
  fail_unless_event(nua_i_outbound, 101);

  nua_authenticate(nh, NUTAG_AUTH(s2_auth_credentials), TAG_END());

  m = s2_sip_wait_for_request(SIP_METHOD_REGISTER);
  fail_if(!m);
  fail_if(!m->sip->sip_authorization);
  /* should not unregister the previous contact
   * as it has not been successfully registered */
  fail_if(!m->sip->sip_contact);
  fail_if(m->sip->sip_contact->m_next);
  s2_save_register(m);

  s2_sip_respond_to(m, NULL,
		SIP_200_OK,
		SIPTAG_CONTACT(s2->registration->contact),
		SIPTAG_VIA(natted_via(m, receive_natted)),
		TAG_END());
  s2_sip_free_message(m);

  assert(s2->registration->contact != NULL);
  fail_unless_event(nua_r_register, 200);

  return nh;
}

START_TEST(register_1_2_2_1)
{
  nua_handle_t *nh = nua_handle(nua, NULL, TAG_END());

  S2_CASE("1.2.2.1", "Register behind NAT",
	  "Authenticate, outbound activated");

  mark_point();
  make_auth_natted_register(nh, TAG_END());
  s2->registration->nh = nh;
  s2_register_teardown();
}
END_TEST

START_TEST(register_1_2_2_2)
{
  nua_handle_t *nh = nua_handle(nua, NULL, TAG_END());
  struct message *m;

  S2_CASE("1.2.2.2", "Register behind NAT",
	  "Authenticate, outbound activated, "
	  "authenticate OPTIONS probe, "
	  "NAT binding change");

  mark_point();
  make_auth_natted_register(nh, TAG_END());
  s2->registration->nh = nh;

  mark_point();

  m = s2_sip_wait_for_request(SIP_METHOD_OPTIONS);
  fail_if(!m);
  s2_sip_respond_to(m, NULL,
		SIP_407_PROXY_AUTH_REQUIRED,
		SIPTAG_VIA(natted_via(m, receive_natted)),
		SIPTAG_PROXY_AUTHENTICATE_STR(s2_auth_digest_str),
		TAG_END());
  s2_sip_free_message(m);
  mark_point();

  m = s2_sip_wait_for_request(SIP_METHOD_OPTIONS);
  fail_if(!m); fail_if(!m->sip->sip_proxy_authorization);
  s2_sip_respond_to(m, NULL,
		SIP_200_OK,
		SIPTAG_VIA(natted_via(m, receive_natted)),
		TAG_END());
  s2_sip_free_message(m);

  su_root_step(s2base->root, 20); su_root_step(s2base->root, 20);
  s2_nua_fast_forward(120, s2base->root);	  /* Default keepalive interval */
  mark_point();

  m = s2_sip_wait_for_request(SIP_METHOD_OPTIONS);
  s2_sip_respond_to(m, NULL,
		SIP_200_OK,
		SIPTAG_VIA(natted_via(m, receive_natted)),
		TAG_END());
  s2_sip_free_message(m);

  su_root_step(s2base->root, 20); su_root_step(s2base->root, 20);
  s2_nua_fast_forward(120, s2base->root);	  /* Default keepalive interval */
  mark_point();

  m = s2_sip_wait_for_request(SIP_METHOD_OPTIONS);
  s2_sip_respond_to(m, NULL,
		SIP_200_OK,
		SIPTAG_VIA(natted_via(m, receive_natted2)),
		TAG_END());
  s2_sip_free_message(m);

  fail_unless_event(nua_i_outbound, 0);

  m = s2_sip_wait_for_request(SIP_METHOD_REGISTER);
  fail_if(!m); fail_if(!m->sip->sip_authorization);
  fail_if(!m->sip->sip_contact || !m->sip->sip_contact->m_next);
  s2_save_register(m);

  s2_sip_respond_to(m, NULL,
		SIP_200_OK,
		SIPTAG_CONTACT(s2->registration->contact),
		SIPTAG_VIA(natted_via(m, receive_natted2)),
		TAG_END());
  s2_sip_free_message(m);

  fail_unless_event(nua_r_register, 200);

  fail_unless(s2->registration->contact != NULL);
  fail_if(s2->registration->contact->m_next != NULL);

  s2_register_teardown();

} END_TEST


START_TEST(register_1_2_2_3)
{
  nua_handle_t *nh = nua_handle(nua, NULL, TAG_END());
  struct message *m;

  S2_CASE("1.2.2.3", "Register behind NAT",
	  "Authenticate, outbound activated, "
	  "detect NAT binding change when re-REGISTERing");

  mark_point();

  make_auth_natted_register(nh,
			    NUTAG_OUTBOUND("no-options-keepalive, no-validate"),
			    TAG_END());
  s2->registration->nh = nh;

  s2_nua_fast_forward(3600, s2base->root);
  mark_point();

  m = s2_sip_wait_for_request(SIP_METHOD_REGISTER);
  fail_if(!m); fail_if(!m->sip->sip_authorization);
  s2_save_register(m);

  s2_sip_respond_to(m, NULL,
		SIP_200_OK,
		SIPTAG_CONTACT(s2->registration->contact),
		SIPTAG_VIA(natted_via(m, receive_natted2)),
		TAG_END());
  s2_sip_free_message(m);

  fail_unless_event(nua_r_register, 100);

  m = s2_sip_wait_for_request(SIP_METHOD_REGISTER);
  fail_if(!m); fail_if(!m->sip->sip_authorization);
  fail_if(!m->sip->sip_contact || !m->sip->sip_contact->m_next);
  s2_save_register(m);

  s2_sip_respond_to(m, NULL,
		SIP_200_OK,
		SIPTAG_CONTACT(s2->registration->contact),
		SIPTAG_VIA(natted_via(m, receive_natted2)),
		TAG_END());
  s2_sip_free_message(m);

  fail_unless(s2->registration->contact != NULL);
  fail_if(s2->registration->contact->m_next != NULL);

  fail_unless_event(nua_r_register, 200);

  s2_register_teardown();

} END_TEST


START_TEST(register_1_2_3) {
  nua_handle_t *nh;
  struct message *m;

  S2_CASE("1.2.3", "Register behind NAT",
	  "Outbound activated by error response");

  nh = nua_handle(nua, NULL, TAG_END());
  nua_register(nh, TAG_END());

  mark_point();

  m = s2_sip_wait_for_request(SIP_METHOD_REGISTER);
  fail_if(!m);
  fail_if(!m->sip->sip_contact || m->sip->sip_contact->m_next);

  s2_sip_respond_to(m, NULL,
		400, "Bad Contact",
		SIPTAG_VIA(natted_via(m, receive_natted)),
		TAG_END());
  s2_sip_free_message(m);

  fail_unless_event(nua_r_register, 100);

  m = s2_sip_wait_for_request(SIP_METHOD_REGISTER);
  fail_if(!m);
  s2_save_register(m);

  s2_sip_respond_to(m, NULL,
		SIP_200_OK,
		SIPTAG_CONTACT(s2->registration->contact),
		SIPTAG_VIA(natted_via(m, receive_natted)),
		TAG_END());
  s2_sip_free_message(m);

  fail_unless(s2->registration->contact != NULL);
  fail_if(s2->registration->contact->m_next != NULL);
  fail_unless_event(nua_r_register, 200);

  s2->registration->nh = nh;

  s2_register_teardown();

} END_TEST

#include <sys/time.h>

START_TEST(register_1_2_4)
{
  nua_handle_t *nh = nua_handle(nua, NULL, TAG_END());
  struct message *m;
  struct event *e;
  unsigned t1, n;

  S2_CASE("1.2.4", "Register behind NAT",
	  "Authenticate, outbound activated, "
	  "drop OPTIONS, check that OPTIONS is not retried");

  mark_point();
  make_auth_natted_register(nh, TAG_END());

  s2->registration->nh = nh;
  mark_point();

  t1 = 500;

  for (t1 = 500, n = 0; n < 20; n++) {
    e = NULL, m = NULL;

    s2_next_thing(&e, &m);

    if (e)
      break;

    fail_if(!m);
    fail_if(!m->sip->sip_request);
    fail_if(m->sip->sip_request->rq_method != sip_method_options);
    s2_sip_free_message(m);

    mark_point();

    s2_nua_fast_forward((t1 + 500) / 1000, s2base->root);
    t1 *= 2;
    if (t1 > 4000)
      t1 = 4000;
  }

  fail_unless(e != NULL);
  fail_unless(e->data->e_event == nua_i_outbound);
  fail_unless(e->data->e_status == 408);
  s2_free_event(e);

  s2_sip_flush_messages();

  s2_nua_fast_forward(3600, s2base->root);

  m = s2_sip_wait_for_request(SIP_METHOD_REGISTER);
  fail_if(!m); fail_if(!m->sip->sip_authorization);
  fail_if(!m->sip->sip_contact);

  s2_default_registration_duration = 120;
  s2_save_register(m);

  s2_sip_respond_to(m, NULL,
		    SIP_200_OK,
		    SIPTAG_CONTACT(s2->registration->contact),
		    TAG_END());
  s2_sip_free_message(m);

  fail_unless_event(nua_r_register, 200);

  fail_unless(s2->registration->contact != NULL);
  fail_if(s2->registration->contact->m_next != NULL);

  s2_sip_flush_messages();

  while (s2sip->received == NULL) {
    s2_nua_fast_forward(10, s2base->root);
  }

  m = s2_sip_remove_message(s2sip->received);
  fail_if(!m);
  fail_unless(m->sip->sip_request->rq_method == sip_method_register);
  fail_if(!m->sip->sip_authorization);
  fail_if(!m->sip->sip_contact);
  s2_save_register(m);

  s2_sip_respond_to(m, NULL,
		    SIP_200_OK,
		    SIPTAG_CONTACT(s2->registration->contact),
		    TAG_END());
  s2_sip_free_message(m);

  fail_unless_event(nua_r_register, 200);

  s2_register_teardown();

} END_TEST


/* ---------------------------------------------------------------------- */

START_TEST(register_1_3_1)
{
  nua_handle_t *nh;
  struct message *m;

  S2_CASE("1.3.1", "Register over TCP via NAT",
	  "REGISTER via TCP, detect NTA, re-REGISTER");

  nh = nua_handle(nua, NULL, TAG_END());

  nua_register(nh, NUTAG_PROXY(s2sip->tcp.contact->m_url), TAG_END());

  m = s2_sip_wait_for_request(SIP_METHOD_REGISTER);
  fail_if(!m); fail_if(!m->sip->sip_contact || m->sip->sip_contact->m_next);
  fail_if(!tport_is_tcp(m->tport));
  s2_save_register(m);

  s2_sip_respond_to(m, NULL,
		SIP_200_OK,
		SIPTAG_CONTACT(s2->registration->contact),
		SIPTAG_VIA(natted_via(m, receive_natted)),
		TAG_END());
  s2_sip_free_message(m);

  assert(s2->registration->contact != NULL);
  fail_unless_event(nua_r_register, 100);

  m = s2_sip_wait_for_request(SIP_METHOD_REGISTER);
  fail_if(!m);
  fail_if(!m->sip->sip_contact || !m->sip->sip_contact->m_next);
  s2_save_register(m);

  s2_sip_respond_to(m, NULL,
		SIP_200_OK,
		SIPTAG_CONTACT(s2->registration->contact),
		SIPTAG_VIA(natted_via(m, receive_natted)),
		TAG_END());
  s2_sip_free_message(m);

  fail_unless(s2->registration->contact != NULL);
  fail_if(s2->registration->contact->m_next != NULL);
  fail_unless(
    url_has_param(s2->registration->contact->m_url, "transport=tcp"));
  fail_unless_event(nua_r_register, 200);

  s2->registration->nh = nh;

  s2_register_teardown();

} END_TEST


START_TEST(register_1_3_2_1)
{
  nua_handle_t *nh = nua_handle(nua, NULL, TAG_END());

  S2_CASE("1.3.2.1", "Register behind NAT",
	  "Authenticate, outbound activated");

  mark_point();
  s2->registration->nh = nh;
  make_auth_natted_register(nh, NUTAG_PROXY(s2sip->tcp.contact->m_url), TAG_END());
  fail_if(!tport_is_tcp(s2->registration->tport));
  s2_register_teardown();
}
END_TEST


START_TEST(register_1_3_2_2)
{
  nua_handle_t *nh = nua_handle(nua, NULL, TAG_END());
  struct message *m;

  S2_CASE("1.3.2.2", "Register behind NAT with TCP",
	  "Detect NAT over TCP using rport. "
	  "Authenticate, detect NAT, "
	  "close TCP at server, wait for re-REGISTERs.");

  nua_set_params(nua, NTATAG_TCP_RPORT(1), TAG_END());
  fail_unless_event(nua_r_set_params, 200);

  mark_point();
  s2->registration->nh = nh;
  make_auth_natted_register(
    nh, NUTAG_PROXY(s2sip->tcp.contact->m_url),
    NUTAG_OUTBOUND("no-options-keepalive, no-validate"),
    TAG_END());
  fail_if(!tport_is_tcp(s2->registration->tport));
  tport_shutdown(s2->registration->tport, 2);

  m = s2_sip_wait_for_request(SIP_METHOD_REGISTER);
  fail_if(!m); fail_if(!m->sip->sip_authorization);
  s2_save_register(m);

  s2_sip_respond_to(m, NULL,
		SIP_200_OK,
		SIPTAG_CONTACT(s2->registration->contact),
		SIPTAG_VIA(natted_via(m, receive_natted)),
		TAG_END());
  s2_sip_free_message(m);

  /* The "NAT binding" changed when new TCP connection is established */
  /* => NUA re-REGISTERs with newly detected contact */
  fail_unless_event(nua_r_register, 100);

  m = s2_sip_wait_for_request(SIP_METHOD_REGISTER);
  fail_if(!m); fail_if(!m->sip->sip_authorization);
  fail_if(!m->sip->sip_contact || !m->sip->sip_contact->m_next);
  s2_save_register(m);

  s2_sip_respond_to(m, NULL,
		SIP_200_OK,
		SIPTAG_CONTACT(s2->registration->contact),
		SIPTAG_VIA(natted_via(m, receive_natted)),
		TAG_END());
  s2_sip_free_message(m);

  fail_unless_event(nua_r_register, 200);

  fail_unless(s2->registration->contact != NULL);
  fail_if(s2->registration->contact->m_next != NULL);

  s2_register_teardown();
}
END_TEST

#if nomore
START_TEST(register_1_3_3_1)
{
  nua_handle_t *nh = nua_handle(nua, NULL, TAG_END());
  struct message *m;
  tport_t *tcp;

  S2_CASE("1.3.3.1", "Register behind NAT with UDP and TCP",
	  "Register with UDP, UDP time-outing, then w/ TCP using rport. ");

  nua_set_params(nua, NTATAG_TCP_RPORT(1), TAG_END());
  fail_unless_event(nua_r_set_params, 200);

  mark_point();
  s2->registration->nh = nh;

  nua_register(nh,
	       NUTAG_OUTBOUND("no-options-keepalive, no-validate"),
	       TAG_END());

  /* NTA tries with UDP, we drop them */
  for (;;) {
    m = s2_sip_wait_for_request(SIP_METHOD_REGISTER); fail_if(!m);
    if (!tport_is_udp(m->tport)) /* Drop UDP */
      break;
    s2_sip_free_message(m);
    s2_nua_fast_forward(4, s2base->root);
  }

  tcp = tport_ref(m->tport);

  /* Respond to request over TCP */
  s2_sip_respond_to(m, NULL,
		SIP_401_UNAUTHORIZED,
		SIPTAG_WWW_AUTHENTICATE_STR(s2_auth_digest_str),
		SIPTAG_VIA(natted_via(m, receive_natted)),
		TAG_END());
  s2_sip_free_message(m);
  fail_unless_event(nua_r_register, 401);
  nua_authenticate(nh, NUTAG_AUTH(s2_auth_credentials), TAG_END());

  /* Turn off pong */
  tport_set_params(tcp, TPTAG_PONG2PING(0), TAG_END());

  /* Now request over UDP ... registering TCP contact! */
  m = s2_sip_wait_for_request(SIP_METHOD_REGISTER);
  fail_if(!m); fail_if(!m->sip->sip_authorization);
  s2_save_register(m);
  fail_unless(
    url_has_param(s2->registration->contact->m_url, "transport=tcp"));
  s2_sip_respond_to(m, NULL,
		SIP_200_OK,
		SIPTAG_CONTACT(s2->registration->contact),
		SIPTAG_VIA(natted_via(m, receive_natted)),
		TAG_END());
  s2_sip_free_message(m);

  /* NUA detects oops... re-registers UDP */
  fail_unless_event(nua_r_register, 100);

  m = s2_sip_wait_for_request(SIP_METHOD_REGISTER);
  fail_if(!m); fail_if(!m->sip->sip_authorization);
  fail_if(!m->sip->sip_contact || !m->sip->sip_contact->m_next);
  s2_save_register(m);

  s2_sip_respond_to(m, NULL,
		SIP_200_OK,
		SIPTAG_CONTACT(s2->registration->contact),
		SIPTAG_VIA(natted_via(m, receive_natted)),
		TAG_END());
  s2_sip_free_message(m);

  fail_unless_event(nua_r_register, 200);

  fail_unless(s2->registration->contact != NULL);
  fail_if(s2->registration->contact->m_next != NULL);

  /* Wait until ping-pong failure closes the TCP connection */
  {
    int i;
    for (i = 0; i < 5; i++) {
      su_root_step(s2base->root, 5);
      su_root_step(s2base->root, 5);
      su_root_step(s2base->root, 5);
      s2_nua_fast_forward(5, s2base->root);
    }
  }

  s2_register_teardown();
}
END_TEST
#endif

START_TEST(register_1_3_3_2)
{
  nua_handle_t *nh = nua_handle(nua, NULL, TAG_END());
  struct message *m;
  tport_t *tcp;
  int i;

  S2_CASE("1.3.3.2", "Register behind NAT with TCP",
	  "Register w/ TCP using rport, pingpong. ");

  nua_set_params(nua, NTATAG_TCP_RPORT(1), TAG_END());
  fail_unless_event(nua_r_set_params, 200);

  mark_point();
  s2->registration->nh = nh;

  nua_register(nh,
	       NUTAG_PROXY(s2sip->tcp.contact->m_url),
	       NUTAG_OUTBOUND("no-options-keepalive, no-validate"),
	       TAG_END());

  m = s2_sip_wait_for_request(SIP_METHOD_REGISTER); fail_if(!m);

  fail_unless(tport_is_tcp(m->tport));

  tcp = tport_ref(m->tport);

  /* Respond to request over TCP */
  s2_sip_respond_to(m, NULL,
		SIP_401_UNAUTHORIZED,
		SIPTAG_WWW_AUTHENTICATE_STR(s2_auth_digest_str),
		SIPTAG_VIA(natted_via(m, receive_natted)),
		TAG_END());
  s2_sip_free_message(m);
  fail_unless_event(nua_r_register, 401);
  nua_authenticate(nh, NUTAG_AUTH(s2_auth_credentials), TAG_END());

  m = s2_sip_wait_for_request(SIP_METHOD_REGISTER);
  fail_if(!m); fail_if(!m->sip->sip_authorization);
  fail_if(!m->sip->sip_contact);
  s2_save_register(m);

  s2_sip_respond_to(m, NULL,
		SIP_200_OK,
		SIPTAG_CONTACT(s2->registration->contact),
		SIPTAG_VIA(natted_via(m, receive_natted)),
		TAG_END());
  s2_sip_free_message(m);

  fail_unless_event(nua_r_register, 200);

  fail_unless(s2->registration->contact != NULL);
  fail_if(s2->registration->contact->m_next != NULL);

  /* Turn off pong */
  tport_set_params(tcp, TPTAG_PONG2PING(0), TAG_END());

  /* Wait until ping-pong failure closes the TCP connection */
  for (i = 0; i < 100; i++) {
    s2_nua_fast_forward(5, s2base->root);
    if (tport_is_closed(tcp))
      break;
  }

  m = s2_sip_wait_for_request(SIP_METHOD_REGISTER);
  fail_unless(tport_is_tcp(m->tport));
  fail_unless(tcp != m->tport);
  fail_if(!m); fail_if(!m->sip->sip_authorization);
  fail_if(!m->sip->sip_contact);
  s2_save_register(m);

  s2_sip_respond_to(m, NULL,
		SIP_200_OK,
		SIPTAG_CONTACT(s2->registration->contact),
		SIPTAG_VIA(natted_via(m, receive_natted)),
		TAG_END());
  s2_sip_free_message(m);

  tport_unref(tcp);

  /* Contact changed */
  fail_unless_event(nua_r_register, 100);

  m = s2_sip_wait_for_request(SIP_METHOD_REGISTER);
  fail_if(!m); fail_if(!m->sip->sip_authorization);
  fail_if(!m->sip->sip_contact);
  fail_if(!m->sip->sip_contact->m_next);
  s2_save_register(m);

  s2_sip_respond_to(m, NULL,
		SIP_200_OK,
		SIPTAG_CONTACT(s2->registration->contact),
		SIPTAG_VIA(natted_via(m, receive_natted)),
		TAG_END());
  s2_sip_free_message(m);

  fail_unless_event(nua_r_register, 200);

  s2_register_teardown();
}
END_TEST

/* ---------------------------------------------------------------------- */

TCase *register_tcase(int threading)
{
  TCase *tc = tcase_create("1 - REGISTER");

  add_register_fixtures(tc, threading, 0);

  {
    tcase_add_test(tc, register_1_0_1);
    tcase_add_test(tc, register_1_1_1);
    tcase_add_test(tc, register_1_1_2);
    tcase_add_test(tc, register_1_2_1);
    tcase_add_test(tc, register_1_2_2_1);
    tcase_add_test(tc, register_1_2_2_2);
    tcase_add_test(tc, register_1_2_2_3);
    tcase_add_test(tc, register_1_2_3);
    tcase_add_test(tc, register_1_2_4);
    tcase_add_test(tc, register_1_3_1);
    tcase_add_test(tc, register_1_3_2_1);
    tcase_add_test(tc, register_1_3_2_2);
  }
  tcase_set_timeout(tc, 10);
  return tc;
}

TCase *pingpong_tcase(int threading)
{
  TCase *tc = tcase_create("1 - REGISTER (with PingPong)");

  add_register_fixtures(tc, threading, 1);

  {
    tcase_add_test(tc, register_1_3_3_2);
  }

  tcase_set_timeout(tc, 10);
  return tc;
}

/* ---------------------------------------------------------------------- */

START_TEST(client_1_4_1)
{
  nua_handle_t *nh;
  struct message *message;
  struct event *response;

  S2_CASE("1.4.1", "Simple client transaction",
	  "Send MESSAGE");

  nh = nua_handle(nua, NULL, SIPTAG_TO(s2sip->aor), TAG_END());
  nua_message(nh,
	      SIPTAG_CONTENT_TYPE_STR("text/plain"),
	      SIPTAG_PAYLOAD_STR("hello"),
	      TAG_END());
  message = s2_sip_wait_for_request(SIP_METHOD_MESSAGE);
  s2_sip_respond_to(message, NULL, SIP_202_ACCEPTED, TAG_END());
  s2_sip_free_message(message);
  response = s2_wait_for_event(nua_r_message, 202);
  s2_free_event(response);

  nua_handle_destroy(nh);
}
END_TEST

START_TEST(client_1_4_2)
{
  nua_handle_t *nh;
  struct message *message;
  struct event *response;

  S2_CASE("1.4.2", "Retry-After a delay",
	  "Retry MESSAGE after a delay");

  nh = nua_handle(nua, NULL, SIPTAG_TO(s2sip->aor), TAG_END());
  nua_message(nh,
	      SIPTAG_CONTENT_TYPE_STR("text/plain"),
	      SIPTAG_PAYLOAD_STR("hello"),
	      TAG_END());
  message = s2_sip_wait_for_request(SIP_METHOD_MESSAGE);
  s2_sip_respond_to(message, NULL, SIP_503_SERVICE_UNAVAILABLE,
		    SIPTAG_RETRY_AFTER_STR("3"),
		    TAG_END());
  s2_sip_free_message(message);
  response = s2_wait_for_event(nua_r_message, 100);
  s2_free_event(response);

  s2_fast_forward(32, NULL);

  /* Too long delay */
  message = s2_sip_wait_for_request(SIP_METHOD_MESSAGE);
  s2_sip_respond_to(message, NULL, SIP_503_SERVICE_UNAVAILABLE,
		    SIPTAG_RETRY_AFTER_STR("32"),
		    TAG_END());
  s2_sip_free_message(message);
  response = s2_wait_for_event(nua_r_message, 503);
  s2_free_event(response);

  nua_set_params(nua, NUTAG_MAX_RETRY_AFTER(0), TAG_END());
  fail_unless_event(nua_r_set_params, 200);

  /* Disable feature */
  nua_message(nh,
	      SIPTAG_CONTENT_TYPE_STR("text/plain"),
	      SIPTAG_PAYLOAD_STR("hello"),
	      TAG_END());
  message = s2_sip_wait_for_request(SIP_METHOD_MESSAGE);
  s2_sip_respond_to(message, NULL, SIP_503_SERVICE_UNAVAILABLE,
		    SIPTAG_RETRY_AFTER_STR("3"),
		    TAG_END());
  s2_sip_free_message(message);
  response = s2_wait_for_event(nua_r_message, 503);
  s2_free_event(response);

  nua_handle_destroy(nh);
}
END_TEST


static TCase *client_tcase(int threading)
{
  TCase *tc = tcase_create(threading ?
			   "1.4 - client (MT)" :
			   "1.4 - client");
  void (*setup)(void);

  setup = threading ? register_thread_setup : register_threadless_setup;
  tcase_add_checked_fixture(tc, setup, register_teardown);

  tcase_add_test(tc, client_1_4_1);
  tcase_add_test(tc, client_1_4_2);

  return tc;
}


void check_register_cases(Suite *suite, int threading)
{
  suite_add_tcase(suite, register_tcase(threading));
  suite_add_tcase(suite, pingpong_tcase(threading));
  suite_add_tcase(suite, client_tcase(threading));
}

