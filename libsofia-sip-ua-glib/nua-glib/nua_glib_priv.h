/*
 * This file is part of the Sofia-SIP package
 *
 * Copyright (C) 2005-2006 Nokia Corporation.
 *
 * Contact: Pekka Pessi <pekka.pessi@nokia.com>
 *
 * * This library is free software; you can redistribute it and/or
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

#ifndef __NUA_GLIB_PRIVATE_H__
#define __NUA_GLIB_PRIVATE_H__

/**@file nua_glib_priv.h Private implementation header for Sofia Glib
 *
 * @author Kai Vehmanen <Kai.Vehmanen@nokia.com>
 * @author Rob Taylor <rob.taylor@collabora.co.uk>
 * @author Pekka Pessi <Pekka.Pessi@nokia.com>
 */

#include <glib.h>

#define SU_ROOT_MAGIC_T NuaGlib
#define NUA_MAGIC_T     NuaGlib
#define NUA_IMAGIC_T    NuaGlibOp
#define NUA_HMAGIC_T    NuaGlibOp
#define SOA_MAGIC_T     NuaGlib

#include <sofia-sip/sip.h>
#include <sofia-sip/sip_header.h>
#include <sofia-sip/sip_status.h>
#include <sofia-sip/nua.h>
#include <sofia-sip/nua_tag.h>
#include <sofia-sip/stun_tag.h>
#include <sofia-sip/soa.h>
#include <sofia-sip/su_tag_io.h>
#include <sofia-sip/su_tagarg.h>
#include <sofia-sip/sl_utils.h>

#include <sofia-sip/su_source.h>
#include <sofia-sip/su_debug.h>

#if HAVE_FUNC
#define enter (void)SU_DEBUG_9(("%s: entering\n", __func__))
#elif HAVE_FUNCTION
#define enter (void)SU_DEBUG_9(("%s: entering\n", __FUNCTION__))
#else
#define enter (void)0
#endif

struct _NuaGlibOp {
  NuaGlibOp *op_next;
  NuaGlib   *op_parent;		/**< Backpointer */

  /**< Remote end identity
   *
   * Contents of To: when initiating, From: when receiving.
   */
  char const   *op_ident;	

  /** NUA handle */ 
  nua_handle_t *op_handle;

  /** How this handle was used initially */
  sip_method_t  op_method;	/**< REGISTER, INVITE, MESSAGE, or SUBSCRIBE */
  char const   *op_method_name;

  /** Call state. 
   *
   * - opc_sent when initial INVITE has been sent
   * - opc_recv when initial INVITE has been received
   * - opc_complate when 200 Ok has been sent/received
   * - opc_active when media is used
   * - opc_sent2 when re-INVITE has been sent
   * - opc_recv2 when re-INVITE has been received
   */
  enum { 
    opc_none, 
    opc_sent = 1, 
    opc_recv = 2, 
    opc_complete = 3, 
    opc_active = 4,
    opc_sent2 = 5,
    opc_recv2 = 6
  } op_callstate;

  int           op_prev_state;     /**< Previous call state */

  unsigned      op_tried_auth: 1;  /**< we've tried to authenticate this op  */
  unsigned      op_auth_failed: 1;  /**< we tried to authenticate this op
                                         and it failed  */
  unsigned      op_persistent : 1; /**< Is this handle persistent? */
  unsigned      op_referred : 1;
  unsigned :0;

  gpointer data;
};


struct _NuaGlibPrivate {

  /* private: maybe this should be really private?*/
  su_home_t   home[1];   /**< Our memory home */
  char const *name;      /**< Our name */
  su_root_t  *root;      /**< Pointer to application root */

  unsigned    init : 1;  /**< True if class is inited */
  
  gchar      *contact;   /**< contact url used by this UA */
  gchar      *authinfo;	 /**< authorization info used by this UA*/
  char       *proxy;     /**< proxy to use,no proxy used if NULL*/
  char       *address;   /**< our SIP address-of-contact*/
  char       *registrar; /**< registrar to use*/
  char       *stun;      /**< STUN server address */
  char       *bind_addr; /**< SIP stack's address, family */

  nua_t        *nua;       /**< Pointer to NUA object */
  NuaGlibOp   *operations; /**< Remote destinations */
};


static void sof_callback(nua_event_t event,
		  int status, char const *phrase,
		  nua_t *nua, NuaGlib *self,
		  nua_handle_t *nh, NuaGlibOp *op, sip_t const *sip,
		  tagi_t tags[]);

static void sof_r_register(int status, char const *phrase,
		      nua_t *nua, NuaGlib *self,
		      nua_handle_t *nh, NuaGlibOp *op, sip_t const *sip,
		      tagi_t tags[]);

static void sof_r_unregister(int status, char const *phrase,
		      nua_t *nua, NuaGlib *self,
		      nua_handle_t *nh, NuaGlibOp *op, sip_t const *sip,
		      tagi_t tags[]);

static void sof_r_publish(int status, char const *phrase,
		   nua_t *nua, NuaGlib *self,
		   nua_handle_t *nh, NuaGlibOp *op, sip_t const *sip,
		   tagi_t tags[]);

static void sof_r_invite(int status, char const *phrase,
		  nua_t *nua, NuaGlib *self,
		  nua_handle_t *nh, NuaGlibOp *op, sip_t const *sip,
		  tagi_t tags[]);
static void sof_i_fork(int status, char const *phrase,
		nua_t *nua, NuaGlib *self,
		nua_handle_t *nh, NuaGlibOp *op, sip_t const *sip,
		tagi_t tags[]);

static void sof_i_invite(nua_t *nua, NuaGlib *self,
		  nua_handle_t *nh, NuaGlibOp *op, sip_t const *sip,
		  tagi_t tags[]);

static void sof_i_state(int status, char const *phrase, 
		 nua_t *nua, NuaGlib *self,
		 nua_handle_t *nh, NuaGlibOp *op, sip_t const *sip,
		 tagi_t tags[]);

static void sof_i_active(nua_t *nua, NuaGlib *self,
		    nua_handle_t *nh, NuaGlibOp *op, sip_t const *sip,
		    tagi_t tags[]);

static void sof_i_terminated(int status, char const *phrase, 
		      nua_t *nua, NuaGlib *self,
		      nua_handle_t *nh, NuaGlibOp *op, sip_t const *sip,
		      tagi_t tags[]);

static void sof_i_prack(nua_t *nua, NuaGlib *self,
		 nua_handle_t *nh, NuaGlibOp *op, sip_t const *sip,
		 tagi_t tags[]);

static void sof_r_bye(int status, char const *phrase, 
	       nua_t *nua, NuaGlib *self,
	       nua_handle_t *nh, NuaGlibOp *op, sip_t const *sip,
	       tagi_t tags[]);

static void sof_i_bye(nua_t *nua, NuaGlib *self,
		 nua_handle_t *nh, NuaGlibOp *op, sip_t const *sip,
		 tagi_t tags[]);

static void sof_i_cancel(nua_t *nua, NuaGlib *self,
		    nua_handle_t *nh, NuaGlibOp *op, sip_t const *sip,
		    tagi_t tags[]);

static void sof_r_message(int status, char const *phrase,
		   nua_t *nua, NuaGlib *self,
		   nua_handle_t *nh, NuaGlibOp *op, sip_t const *sip,
		   tagi_t tags[]);

static void sof_i_message(nua_t *nua, NuaGlib *self,
		   nua_handle_t *nh, NuaGlibOp *op, sip_t const *sip,
		   tagi_t tags[]);

static void sof_r_info(int status, char const *phrase,
		nua_t *nua, NuaGlib *self,
		nua_handle_t *nh, NuaGlibOp *op, sip_t const *sip,
		tagi_t tags[]);

static void sof_i_info(nua_t *nua, NuaGlib *self,
		nua_handle_t *nh, NuaGlibOp *op, sip_t const *sip,
		tagi_t tags[]);

static void sof_r_refer(int status, char const *phrase,
		   nua_t *nua, NuaGlib *self,
		   nua_handle_t *nh, NuaGlibOp *op, sip_t const *sip,
		   tagi_t tags[]);

static void sof_i_refer(nua_t *nua, NuaGlib *self,
		   nua_handle_t *nh, NuaGlibOp *op, sip_t const *sip,
		   tagi_t tags[]);

static void sof_r_subscribe(int status, char const *phrase,
		     nua_t *nua, NuaGlib *self,
		     nua_handle_t *nh, NuaGlibOp *op, sip_t const *sip,
		     tagi_t tags[]);

static void sof_r_unsubscribe(int status, char const *phrase,
		       nua_t *nua, NuaGlib *self,
		       nua_handle_t *nh, NuaGlibOp *op, sip_t const *sip,
		       tagi_t tags[]);

static void sof_r_notify(int status, char const *phrase,
		   nua_t *nua, NuaGlib *self,
		   nua_handle_t *nh, NuaGlibOp *op, sip_t const *sip,
		   tagi_t tags[]);

static void sof_i_notify(nua_t *nua, NuaGlib *self,
		    nua_handle_t *nh, NuaGlibOp *op, sip_t const *sip,
		    tagi_t tags[]);

static void sof_r_options(int status, char const *phrase,
		   nua_t *nua, NuaGlib *self,
		   nua_handle_t *nh, NuaGlibOp *op, sip_t const *sip,
		   tagi_t tags[]);

static void sof_r_shutdown(int status, char const *phrase, 
		    nua_t *nua, NuaGlib *self,
		    nua_handle_t *nh, NuaGlibOp *op, sip_t const *sip,
		    tagi_t tags[]);

static void sof_r_get_params(int status, char const *phrase,
		      nua_t *nua, NuaGlib *self,
		      nua_handle_t *nh, NuaGlibOp *op, sip_t const *sip,
		      tagi_t tags[]);

static void sof_i_error(nua_t *nua, NuaGlib *self, nua_handle_t *nh, NuaGlibOp *op, 
		 int status, char const *phrase,
		 tagi_t tags[]);

#endif /* __NUA_GLIB_PRIVATE_H__ */