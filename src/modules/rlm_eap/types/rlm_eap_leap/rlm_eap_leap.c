/*
 * rlm_eap_leap.c    Handles that are called from eap
 *
 * Version:     $Id$
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * Copyright 2003 Alan DeKok <aland@freeradius.org>
 * Copyright 2006 The FreeRADIUS server project
 */

RCSID("$Id$")

#include <stdio.h>
#include <stdlib.h>

#include "eap_leap.h"

static rlm_rcode_t CC_HINT(nonnull) mod_process(void *instance, eap_session_t *eap_session);

static rlm_rcode_t mod_process(UNUSED void *instance, eap_session_t *eap_session)
{
	int		rcode;
	REQUEST 	*request = eap_session->request;
	leap_session_t	*session;
	leap_packet_t	*packet;
	leap_packet_t	*reply;
	VALUE_PAIR	*password;

	if (!eap_session->opaque) {
		REDEBUG("Cannot authenticate without LEAP history");
		return RLM_MODULE_INVALID;
	}
	session = talloc_get_type_abort(eap_session->opaque, leap_session_t);
	reply = NULL;

	/*
	 *	Extract the LEAP packet.
	 */
	packet = eap_leap_extract(request, eap_session->this_round);
	if (!packet) return RLM_MODULE_INVALID;

	/*
	 *	The password is never sent over the wire.
	 *	Always get the configured password, for each user.
	 */
	password = fr_pair_find_by_num(eap_session->request->control, 0, FR_CLEARTEXT_PASSWORD, TAG_ANY);
	if (!password) password = fr_pair_find_by_num(eap_session->request->control, 0, FR_NT_PASSWORD, TAG_ANY);
	if (!password) {
		REDEBUG("No Cleartext-Password or NT-Password configured for this user");
		talloc_free(packet);
		return RLM_MODULE_REJECT;
	}

	/*
	 *	We've already sent the AP challenge.  This packet
	 *	should contain the NtChallengeResponse
	 */
	switch (session->stage) {
	case 4:			/* Verify NtChallengeResponse */
		RDEBUG2("Stage 4");
		rcode = eap_leap_stage4(request, packet, password, session);
		session->stage = 6;

		/*
		 *	We send EAP-Success or EAP-Fail, and not
		 *	any LEAP packet.  So we return here.
		 */
		if (!rcode) {
			eap_session->this_round->request->code = FR_EAP_CODE_FAILURE;
			talloc_free(packet);
			return 0;
		}

		eap_session->this_round->request->code = FR_EAP_CODE_SUCCESS;

		/*
		 *	Do this only for Success.
		 */
		eap_session->this_round->request->id = eap_session->this_round->response->id + 1;
		eap_session->this_round->set_request_id = true;

		/*
		 *	LEAP requires a challenge in stage 4, not
		 *	an Access-Accept, which is normally returned
		 *	by eap_compose() in eap.c, when the EAP reply code
		 *	is EAP_SUCCESS.
		 */
		eap_session->request->reply->code = FR_CODE_ACCESS_CHALLENGE;
		talloc_free(packet);
		return RLM_MODULE_OK;

	case 6:			/* Issue session key */
		RDEBUG2("Stage 6");
		reply = eap_leap_stage6(request, packet, eap_session->request->username, password, session);
		break;

		/*
		 *	Stages 1, 3, and 5 are requests from the AP.
		 *	Stage 2 is handled by initiate()
		 */
	default:
		RDEBUG("Internal sanity check failed on stage");
		break;
	}

	talloc_free(packet);

	/*
	 *	Process the packet.  We don't care about any previous
	 *	EAP packets, as
	 */
	if (!reply) return RLM_MODULE_FAIL;

	eap_leap_compose(request, eap_session->this_round, reply);
	talloc_free(reply);

	return RLM_MODULE_OK;
}

/*
 * send an initial eap-leap request
 * ie access challenge to the user/peer.

 * Frame eap reply packet.
 * len = header + type + leap_methoddata
 * leap_methoddata = value_size + value
 */
static rlm_rcode_t CC_HINT(nonnull) mod_session_init(UNUSED void *instance, eap_session_t *eap_session)
{
	REQUEST 	*request = eap_session->request;
	leap_session_t	*session;
	leap_packet_t	*reply;

	RDEBUG2("Stage 2");

	/*
	 *	LEAP requires a User-Name attribute
	 */
	if (!eap_session->request->username) {
		REDEBUG("User-Name is required for EAP-LEAP authentication");
		return RLM_MODULE_REJECT;
	}

	reply = eap_leap_initiate(request, eap_session->this_round, eap_session->request->username);
	if (!reply) return RLM_MODULE_FAIL;

	eap_leap_compose(request, eap_session->this_round, reply);

	MEM(eap_session->opaque = session = talloc(eap_session, leap_session_t));

	/*
	 *	Remember which stage we're in, and which challenge
	 *	we sent to the AP.  The later stages will take care
	 *	of filling in the peer response.
	 */
	session->stage = 4;	/* the next stage we're in */
	memcpy(session->peer_challenge, reply->challenge, reply->count);

	RDEBUG2("Successfully initiated");

	talloc_free(reply);

	eap_session->process = mod_process;

	return RLM_MODULE_OK;
}

/*
 *	The module name should be the only globally exported symbol.
 *	That is, everything else should be 'static'.
 */
extern rlm_eap_submodule_t rlm_eap_leap;
rlm_eap_submodule_t rlm_eap_leap = {
	.name		= "eap_leap",
	.magic		= RLM_MODULE_INIT,
	.session_init	= mod_session_init,	/* Initialise a new EAP session */
	.process	= mod_process		/* Process next round of EAP method */
};
