/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

#ifdef HAVE_DIAGNOSTIC
static void __hazard_dump(WT_SESSION_IMPL *);
#endif

/*
 * __wt_hazard_set --
 *	Set a hazard reference.
 */
int
__wt_hazard_set(WT_SESSION_IMPL *session, WT_REF *ref
#ifdef HAVE_DIAGNOSTIC
    , const char *file, int line
#endif
    )
{
	WT_CONNECTION_IMPL *conn;
	WT_HAZARD *hp;

	conn = S2C(session);

	/*
	 * Do the dance:
	 *
	 * The memory location which makes a page "real" is the WT_REF's state
	 * of WT_REF_MEM, which can be set to WT_REF_LOCKED at any time by the
	 * page eviction server.
	 *
	 * Add the WT_REF reference to the session's hazard list and flush the
	 * write, then see if the state field is still WT_REF_MEM.  If it's
	 * still WT_REF_MEM, we can use the page because the page eviction
	 * server will see our hazard reference before it discards the buffer
	 * (the eviction server sets the state to WT_REF_LOCKED, then flushes
	 * memory and checks the hazard references).
	 */
	for (hp = session->hazard;
	    hp < session->hazard + conn->hazard_size; ++hp) {
		if (hp->page != NULL)
			continue;

		/*
		 * A write barrier is needed to ensure the current values in
		 * ref are stored before the pointer becomes visible in the
		 * hazard list.
		 */
		WT_PUBLISH(hp->page, ref->page);
#ifdef HAVE_DIAGNOSTIC
		hp->file = file;
		hp->line = line;
#endif

		/*
		 * Check to see if it's still valid (where valid means a state
		 * of WT_REF_MEM).
		 */
		if (ref->state == WT_REF_MEM) {
			WT_VERBOSE(session, HAZARD,
			    "session %p hazard %p: set", session, ref->page);
			return (1);
		}

		/*
		 * The page isn't available, it's being considered for eviction
		 * (or being evicted for all we know).  If the eviction server
		 * sees our hazard reference before evicting the page, it will
		 * return the page to use, no harm done.  In the worst case, we
		 * could be asleep for a long time; that won't hurt anything,
		 * we just might prevent random pages from being evicted.
		 */
		hp->page = NULL;
		return (0);
	}

	__wt_errx(session,
	    "there are no more hazard reference slots in the session");

#ifdef HAVE_DIAGNOSTIC
	__hazard_dump(session);
#endif

	return (0);
}

/*
 * __wt_hazard_clear --
 *	Clear a hazard reference.
 */
void
__wt_hazard_clear(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_CONNECTION_IMPL *conn;
	WT_HAZARD *hp;

	conn = S2C(session);

	/*
	 * The default value for a WT_HAZARD slot is NULL, but clearing a
	 * NULL reference isn't a good idea.
	 */
	WT_ASSERT(session, page != NULL);

	WT_VERBOSE(session, HAZARD,
	    "session %p hazard %p: clr", session, page);

	/* Clear the caller's hazard pointer. */
	for (hp = session->hazard;
	    hp < session->hazard + conn->hazard_size; ++hp)
		if (hp->page == page) {
			/*
			 * Make sure any changes to the page are visible before
			 * we give up our hazard reference.
			 */
			WT_PUBLISH(hp->page, NULL);
			return;
		}
	WT_FAILURE(session, "hazard reference not found");
}

/*
 * __wt_hazard_empty --
 *	Verify that no hazard references are set.
 */
void
__wt_hazard_empty(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_HAZARD *hp;

	conn = S2C(session);

	/*
	 * Check for a set hazard reference and complain if we find one.  Clear
	 * any we find because it's not a correctness problem (any hazard ref
	 * we find can't be real because the session is being closed when we're
	 * called).   We do this work because it's not expensive, and we don't
	 * want to let a hazard reference lie around, keeping a page from being
	 * flushed.  The flush isn't necessary for correctness, but gives the
	 * cache eviction thread immediate access to any page our reference
	 * blocks.
	 */
#ifdef HAVE_DIAGNOSTIC
	__hazard_dump(session);
#endif

	for (hp = session->hazard;
	    hp < session->hazard + conn->hazard_size; ++hp)
		if (hp->page != NULL) {
			hp->page = NULL;

			__wt_errx(session,
			    "unexpected hazard reference at session.close");
		}
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __hazard_dump --
 *	Display the list of hazard references.
 */
static void
__hazard_dump(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_HAZARD *hp;
	int fail;

	conn = S2C(session);

	fail = 0;
	for (hp = session->hazard;
	    hp < session->hazard + conn->hazard_size; ++hp)
		if (hp->page != NULL) {
			__wt_errx(session,
			    "hazard reference: (%p: %s, line %d)",
			    hp->page, hp->file, hp->line);
			fail = 1;
		}

	if (fail)
		WT_FAILURE(session, "unexpected hazard reference");
}

/*
 * __wt_hazard_validate --
 *	Confirm that a page isn't on the hazard list.
 */
void
__wt_hazard_validate(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_CONNECTION_IMPL *conn;
	WT_HAZARD *hp;
	WT_SESSION_IMPL **tp;

	conn = S2C(session);

	for (tp = conn->sessions; (session = *tp) != NULL; ++tp)
		for (hp = session->hazard;
		    hp < session->hazard + S2C(session)->hazard_size; ++hp)
			if (hp->page == page) {
				WT_FAILURE(session,
				    "discarded page has hazard reference: "
				    "(%p: %s, line %d)",
				    hp->page, hp->file, hp->line);
			}
}
#endif
