/*
 * Copyright 2003,2004,2005,2006 Red Hat, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, and the entire permission notice in its entirety,
 *    including the disclaimer of warranties.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior
 *    written permission.
 *
 * ALTERNATIVELY, this product may be distributed under the terms of the
 * GNU Lesser General Public License, in which case the provisions of the
 * LGPL are required INSTEAD OF the above restrictions.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN
 * NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "../config.h"

#ifdef HAVE_SECURITY_PAM_APPL_H
#include <security/pam_appl.h>
#endif

#ifdef HAVE_SECURITY_PAM_MODULES_H
#define PAM_SM_AUTH
#define PAM_SM_SESSION
#include <security/pam_modules.h>
#endif

#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <krb5.h>
#ifdef USE_KRB4
#include KRB4_DES_H
#include KRB4_KRB_H
#ifdef KRB4_KRB_ERR_H
#include KRB4_KRB_ERR_H
#endif
#endif

#include "conv.h"
#include "init.h"
#include "initopts.h"
#include "items.h"
#include "log.h"
#include "options.h"
#include "prompter.h"
#include "sly.h"
#include "stash.h"
#include "tokens.h"
#include "userinfo.h"
#include "v5.h"
#include "v4.h"
#include "xstr.h"

#ident "$Id$"

int
pam_sm_authenticate(pam_handle_t *pamh, int flags,
		    int argc, PAM_KRB5_MAYBE_CONST char **argv)
{
	PAM_KRB5_MAYBE_CONST char *user;
	krb5_context ctx;
	struct _pam_krb5_options *options;
	struct _pam_krb5_user_info *userinfo;
	struct _pam_krb5_stash *stash;
	krb5_get_init_creds_opt gic_options;
	int i, retval;
	char *password;

	/* Initialize Kerberos. */
	if (_pam_krb5_init_ctx(&ctx, argc, argv) != 0) {
		warn("error initializing Kerberos");
		return PAM_SERVICE_ERR;
	}

	/* Get the user's name. */
	i = pam_get_user(pamh, &user, NULL);
	if (i != PAM_SUCCESS) {
		warn("could not identify user name");
		krb5_free_context(ctx);
		return i;
	}

	/* Read our options. */
	options = _pam_krb5_options_init(pamh, argc, argv, ctx);
	if (options == NULL) {
		warn("error parsing options (shouldn't happen)");
		krb5_free_context(ctx);
		return PAM_SERVICE_ERR;
	}
	if (options->debug) {
		debug("called to authenticate '%s', realm '%s'", user,
		      options->realm);
	}
	krb5_get_init_creds_opt_init(&gic_options);
	_pam_krb5_set_init_opts(ctx, &gic_options, options);

	/* Get information about the user and the user's principal name. */
	userinfo = _pam_krb5_user_info_init(ctx, user, options->realm,
					    options->user_check,
					    options->n_mappings,
					    options->mappings);
	if (userinfo == NULL) {
		if (options->ignore_unknown_principals) {
			retval = PAM_IGNORE;
		} else {
			warn("error getting information about '%s'", user);
			retval = PAM_USER_UNKNOWN;
		}
		if (options->use_second_pass) {
			password = NULL;
			i = _pam_krb5_prompt_for(pamh, "Password: ", &password);
			if ((i == PAM_SUCCESS) &&
			    (flags & PAM_DISALLOW_NULL_AUTHTOK) &&
			    (strlen(password) == 0)) {
				warn("disallowing NULL authtok for '%s'", user);
				retval = PAM_AUTH_ERR;
				i = PAM_AUTH_ERR;
			}
			if (i == PAM_SUCCESS) {
				/* Save the password for the next module. */
				if (!_pam_krb5_has_item(pamh, PAM_AUTHTOK)) {
					if (options->debug) {
						debug("saving newly-entered "
						      "password for use by "
						      "other modules");
					}
					pam_set_item(pamh, PAM_AUTHTOK, password);
				}
			}
		}
		_pam_krb5_options_free(pamh, ctx, options);
		krb5_free_context(ctx);
		return retval;
	}
	if (options->debug) {
		debug("authenticating '%s'", userinfo->unparsed_name);
	}

	/* Check the minimum UID argument. */
	if ((options->minimum_uid != (uid_t) -1) &&
	    (userinfo->uid < options->minimum_uid)) {
		if (options->debug) {
			debug("ignoring '%s' -- uid below minimum = %lu", user,
			      (unsigned long) options->minimum_uid);
		}
		_pam_krb5_user_info_free(ctx, userinfo);
		_pam_krb5_options_free(pamh, ctx, options);
		krb5_free_context(ctx);
		return PAM_IGNORE;
	}

	/* Get the stash for this user. */
	stash = _pam_krb5_stash_get(pamh, userinfo, options);
	if (stash == NULL) {
		warn("error retrieving stash for '%s' (shouldn't happen)",
		     user);
		_pam_krb5_user_info_free(ctx, userinfo);
		_pam_krb5_options_free(pamh, ctx, options);
		krb5_free_context(ctx);
		return PAM_SERVICE_ERR;
	}

	/* If we've been called before, then the stash is more or less stale,
	 * so reset things for applications which call pam_authenticate() more
	 * than once with the same library context. */
	stash->v5attempted = 0;

	retval = PAM_AUTH_ERR;

	/* If we're configured to use an existing ccache, try that. */
	if ((retval != PAM_SUCCESS) && (options->existing_ticket)) {
		if (options->debug) {
			debug("trying existing credentials for '%s'",user);
		}
		retval = v5_get_creds(ctx, pamh,
				      &stash->v5creds, userinfo,
				      options,
				      KRB5_TGS_NAME,
				      password, &gic_options,
				      &stash->v5result);
		stash->v5attempted = 1;
		if (options->debug) {
			debug("got result %d (%s)", stash->v5result,
			      v5_error_message(stash->v5result));
		}
	}

	/* Try with the stored password, if we've been told to do so. */
	if ((retval != PAM_SUCCESS) && (options->use_first_pass)) {
		password = NULL;
		i = _pam_krb5_get_item_text(pamh, PAM_AUTHTOK, &password);
		if ((i == PAM_SUCCESS) &&
		    (flags & PAM_DISALLOW_NULL_AUTHTOK) &&
		    (password != NULL) &&
		    (strlen(password) == 0)) {
			warn("disallowing NULL authtok for '%s'", user);
			retval = PAM_AUTH_ERR;
			i = PAM_AUTH_ERR;
		}
		if ((i == PAM_SUCCESS) && (password != NULL)) {
			if (options->debug) {
				debug("trying previously-entered password for "
				      "'%s'", user);
			}
			retval = v5_get_creds(ctx, pamh,
					      &stash->v5creds, userinfo,
					      options,
					      KRB5_TGS_NAME,
					      password, &gic_options,
					      &stash->v5result);
			stash->v5attempted = 1;
			if (options->debug) {
				debug("got result %d (%s)", stash->v5result,
				      v5_error_message(stash->v5result));
			}
		}
		if ((retval == PAM_SUCCESS) &&
		    ((options->v4 == 1) || (options->v4_for_afs == 1))) {
			v4_get_creds(ctx, pamh, stash, userinfo, options,
				     password, &i);
			if ((i != 0) && (options->debug)) {
				debug("error obtaining v4 creds: %d (%s)",
				      i, v5_error_message(i));
			}
			if (stash->v4present &&
			    (options->ignore_afs == 0) &&
			    (options->tokens == 1)) {
				v5_save(ctx, stash, userinfo, options, NULL);
				v4_save(ctx, stash, userinfo, options,
					getuid(), getgid(), NULL);
				tokens_obtain(ctx, stash, options, userinfo, 1);
				v4_destroy(ctx, stash, options);
				v5_destroy(ctx, stash, options);
			}
		}
	}

	/* If that didn't work, ask for a new password and try again. */
	if ((retval != PAM_SUCCESS) && (options->use_second_pass)) {
		password = NULL;
		i = _pam_krb5_prompt_for(pamh, "Password: ", &password);
		if ((i == PAM_SUCCESS) &&
		    (flags & PAM_DISALLOW_NULL_AUTHTOK) &&
		    (password != NULL) &&
		    (strlen(password) == 0)) {
			warn("disallowing NULL authtok for '%s'", user);
			retval = PAM_AUTH_ERR;
			i = PAM_AUTH_ERR;
		}
		if ((i == PAM_SUCCESS) && (password != NULL)) {
			/* Save the password for the next module. */
			if (!_pam_krb5_has_item(pamh, PAM_AUTHTOK)) {
				if (options->debug) {
					debug("saving newly-entered "
					      "password for use by "
					      "other modules");
				}
				pam_set_item(pamh, PAM_AUTHTOK, password);
			}
			/* Get creds. */
			if (options->debug) {
				debug("trying newly-entered password for "
				      "'%s'", user);
			}
			retval = v5_get_creds(ctx, pamh,
					      &stash->v5creds, userinfo,
					      options,
					      KRB5_TGS_NAME,
					      password, &gic_options,
					      &stash->v5result);
			stash->v5attempted = 1;
			if (options->debug) {
				debug("got result %d (%s)", stash->v5result,
				      v5_error_message(stash->v5result));
			}
			/* Save the password for the next module. */
			if (!_pam_krb5_has_item(pamh, PAM_AUTHTOK)) {
				if (options->debug) {
					debug("saving newly-entered "
					      "password for use by "
					      "other modules");
				}
				pam_set_item(pamh, PAM_AUTHTOK, password);
			}
		} else {
			warn("error reading password for '%s'", user);
		}
		if ((retval == PAM_SUCCESS) &&
		    ((options->v4 == 1) || (options->v4_for_afs == 1))) {
			v4_get_creds(ctx, pamh, stash, userinfo, options,
				     password, &i);
			if ((i != 0) && (options->debug)) {
				debug("error obtaining v4 creds: %d (%s)",
				      i, v5_error_message(i));
			}
			if (stash->v4present &&
			    (options->ignore_afs == 0) &&
			    (options->tokens == 1)) {
				v5_save(ctx, stash, userinfo, options, NULL);
				v4_save(ctx, stash, userinfo, options,
					getuid(), getgid(), NULL);
				tokens_obtain(ctx, stash, options, userinfo, 1);
				v4_destroy(ctx, stash, options);
				v5_destroy(ctx, stash, options);
			}
		}
		if (password != NULL) {
			xstrfree(password);
		}
	}

	/* If we didn't use any password, try here. */
	if ((retval != PAM_SUCCESS) &&
	    (!options->use_first_pass) && (!options->use_second_pass)) {
		password = NULL;
		retval = v5_get_creds(ctx, pamh,
				      &stash->v5creds, userinfo,
				      options,
				      KRB5_TGS_NAME,
				      NULL, &gic_options,
				      &stash->v5result);
		stash->v5attempted = 1;
		if (options->debug) {
			debug("got result %d (%s)", stash->v5result,
			      v5_error_message(stash->v5result));
		}
		if ((retval == PAM_SUCCESS) &&
		    ((options->v4 == 1) || (options->v4_for_afs == 1))) {
			v4_get_creds(ctx, pamh, stash, userinfo, options,
				     password, &i);
			if ((i != 0) && (options->debug)) {
				debug("error obtaining v4 creds: %d (%s)",
				      i, v5_error_message(i));
			}
			if (stash->v4present &&
			    (options->ignore_afs == 0) &&
			    (options->tokens == 1)) {
				v5_save(ctx, stash, userinfo, options, NULL);
				v4_save(ctx, stash, userinfo, options,
					getuid(), getgid(), NULL);
				tokens_obtain(ctx, stash, options, userinfo, 1);
				v4_destroy(ctx, stash, options);
				v5_destroy(ctx, stash, options);
			}
		}
	}

	/* If we got this far, check the target user's .k5login file. */
	if ((retval == PAM_SUCCESS) && options->user_check) {
		if (krb5_kuserok(ctx, userinfo->principal_name, user) == 0) {
			notice("account checks fail for '%s': user disallowed "
			       "by .k5login file for '%s'",
			       userinfo->unparsed_name, user);
			retval = PAM_PERM_DENIED;
		}
	}

	/* Log the authentication status, optionally saving the credentials in
	 * a piece of shared memory. */
	if (retval == PAM_SUCCESS) {
		if (options->use_shmem) {
			_pam_krb5_stash_shm_write(pamh, stash, options,
						  userinfo);
		}
		notice("authentication succeeds for '%s' (%s)", user,
		       userinfo->unparsed_name);
	} else {
		if ((retval == PAM_USER_UNKNOWN) &&
		    options->ignore_unknown_principals) {
			retval = PAM_IGNORE;
		} else {
			notice("authentication fails for '%s' (%s): %s (%s)",
			       user,
			       userinfo->unparsed_name,
			       pam_strerror(pamh, retval),
			       v5_error_message(stash->v5result));
		}
	}

	/* Clean up. */
	if (options->debug) {
		debug("pam_authenticate returning %d (%s)", retval,
		      pam_strerror(pamh, retval));
	}
	_pam_krb5_options_free(pamh, ctx, options);
	_pam_krb5_user_info_free(ctx, userinfo);
	krb5_free_context(ctx);

	return retval;
}

int
pam_sm_setcred(pam_handle_t *pamh, int flags,
	       int argc, PAM_KRB5_MAYBE_CONST char **argv)
{
	if (flags & PAM_ESTABLISH_CRED) {
		return pam_sm_open_session(pamh, flags, argc, argv);
	}
	if (flags & (PAM_REINITIALIZE_CRED | PAM_REFRESH_CRED)) {
		if (_pam_krb5_sly_looks_unsafe() == 0) {
			return _pam_krb5_sly_maybe_refresh(pamh, flags,
							   argc, argv);
		} else {
			return PAM_IGNORE;
		}
	}
	if (flags & PAM_DELETE_CRED) {
		return pam_sm_close_session(pamh, flags, argc, argv);
	}
	warn("pam_setcred() called with no flags");
	return PAM_SERVICE_ERR;
}
