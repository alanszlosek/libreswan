/* shutdown pluto, for libreswan
 *
 * Copyright (C) 1997      Angelos D. Keromytis.
 * Copyright (C) 1998-2001,2013 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2003-2008 Michael C Richardson <mcr@xelerance.com>
 * Copyright (C) 2003-2010 Paul Wouters <paul@xelerance.com>
 * Copyright (C) 2007 Ken Bantoft <ken@xelerance.com>
 * Copyright (C) 2008-2009 David McCullough <david_mccullough@securecomputing.com>
 * Copyright (C) 2009 Avesh Agarwal <avagarwa@redhat.com>
 * Copyright (C) 2009-2016 Tuomo Soini <tis@foobar.fi>
 * Copyright (C) 2012-2019 Paul Wouters <pwouters@redhat.com>
 * Copyright (C) 2012-2016 Paul Wouters <paul@libreswan.org>
 * Copyright (C) 2012 Kim B. Heino <b@bbbs.net>
 * Copyright (C) 2012 Philippe Vouters <Philippe.Vouters@laposte.net>
 * Copyright (C) 2012 Wes Hardaker <opensource@hardakers.net>
 * Copyright (C) 2013 David McCullough <ucdevel@gmail.com>
 * Copyright (C) 2016-2020 Andrew Cagney
 * Copyright (C) 2017 Mayank Totale <mtotale@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <https://www.gnu.org/licenses/gpl2.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include <unistd.h>		/* for exit(2) */

#include "constants.h"
#include "lswconf.h"		/* for lsw_conf_free_oco() */
#include "lswnss.h"		/* for lsw_nss_shutdown() */
#include "lswalloc.h"		/* for report_leaks() et.al. */

#include "defs.h"		/* for so_serial_t */
#include "pluto_shutdown.h"
#include "log.h"		/* for close_log() et.al. */

#include "pluto_crypt.h"	/* for stop_crypto_helpers() */
#include "pluto_sd.h"		/* for pluto_sd() */
#include "root_certs.h"		/* for free_root_certs() */
#include "keys.h"		/* for free_preshared_secrets() */
#include "connections.h"	/* for delete_every_connection() */
#include "fetch.h"		/* for stop_crl_fetch_helper() et.al. */
#include "crl_queue.h"		/* for free_crl_queue() */
#include "iface.h"		/* for free_ifaces() */
#include "kernel.h"		/* for kernel_ops.shutdown() */
#include "virtual.h"		/* for free_virtual_ip() */
#include "server.h"		/* for free_server() */
#ifdef USE_DNSSEC
#include "dnssec.h"		/* for unbound_ctx_free() */
#endif
#include "demux.h"		/* for free_demux() */

volatile bool exiting_pluto = false;

/*
 * leave pluto, with status.
 * Once child is launched, parent must not exit this way because
 * the lock would be released.
 *
 *  0 OK
 *  1 general discomfort
 * 10 lock file exists
 */
void exit_pluto(enum pluto_exit_code status)
{
	/* no whack; right? XXX: yes, but why? */
	struct fd *whackfd = null_fd;
	struct logger logger[1] = { GLOBAL_LOGGER(whackfd), };

	/*
	 * Tell the world, well actually all the threads, that pluto
	 * is exiting and they should quit.  Even if pthread_cancel()
	 * weren't buggy, using it correctly would be hard, so use
	 * this instead.
	 */
	exiting_pluto = true;

	/* needed because we may be called in odd state */
	reset_globals();
 #ifdef USE_SYSTEMD_WATCHDOG
	pluto_sd(PLUTO_SD_STOPPING, status);
 #endif

	/*
	 * Wait for the crypto-helper threads to notice EXITING_PLUTO
	 * and exit (if necessary, wake any sleeping helpers from
	 * their slumber).  Without this any helper using NSS after
	 * the code below has shutdown the NSS DB will crash.
	 *
	 * This does not try to delete any tasks left waiting on the
	 * helper queue.  Instead, code further down deleting
	 * connections (which in turn deletes states) should clean
	 * that up?
	 *
	 * This also does not try to delete any completed tasks
	 * waiting on the event loop.  One theory is for the helper
	 * code to be changed so that helper tasks can be "cancelled"
	 * after the've completed?
	 */
	stop_crypto_helpers(logger);

	free_root_certs(whackfd);
	free_preshared_secrets(logger);
	free_remembered_public_keys();
	delete_every_connection();

	/*
	 * free memory allocated by initialization routines.  Please don't
	 * forget to do this.
	 */

#if defined(LIBCURL) || defined(LIBLDAP)
	/*
	 * Wait for the CRL fetch handler to finish its current task.
	 * Without this CRL fetch requests are left hanging and, after
	 * the NSS DB has been closed (below), the helper can crash.
	 */
	stop_crl_fetch_helper();
	/*
	 * free the crl list that the fetch-helper is currently
	 * processing
	 */
	free_crl_fetch();
	/*
	 * free the crl requests that are waiting to be picked and
	 * processed by the fetch-helper.
	 */
	free_crl_queue();
#endif

	lsw_conf_free_oco();	/* free global_oco containing path names */

	free_ifaces();	/* free interface list from memory */
	if (kernel_ops->shutdown != NULL)
		kernel_ops->shutdown();
	lsw_nss_shutdown();
	delete_lock();	/* delete any lock files */
	free_virtual_ip();	/* virtual_private= */
	free_server(); /* no libevent evnts beyond this point */
	free_demux();
	free_pluto_main();	/* our static chars */

#ifdef USE_DNSSEC
	unbound_ctx_free();
#endif

	/* report memory leaks now, after all free_* calls */
	if (leak_detective) {
		report_leaks(logger);
	}
	close_log();	/* close the logfiles */
#ifdef USE_SYSTEMD_WATCHDOG
	pluto_sd(PLUTO_SD_EXIT, status);
#endif
	exit(status);	/* exit, with our error code */
}
