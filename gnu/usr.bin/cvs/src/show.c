/*
 * Copyright (c) 2016 joshua stein <jcs@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <assert.h>
#include "cvs.h"
#include "getline.h"

static const char *const show_usage[] = {
	"Usage: %s %s [commitid | changeset | \"genesis\"]\n",
	"(Specify the --help global option for a list of other help options)\n",
	NULL
};

void show_commitid_header(RCSNode *, char *);

int
show(int argc, char **argv)
{
	char *tcommitid = NULL, *repo;
	CommitId *commitid;

	if (argc == 2) {
		if (strcmp(argv[1], "genesis") == 0)
			tcommitid = xstrdup("0");
		else
			tcommitid = xstrdup(argv[1]);
	}
	else if (argc < 1 || argc > 2)
		usage (show_usage);

#ifdef CLIENT_SUPPORT
	if (current_parsed_root->isremote) {
		start_server();

		ign_setup();

		send_arg(tcommitid);
		send_to_server("show\012", 0);

		return get_responses_and_close ();
	}
#endif

	repo = commitid_repo_base();
	if ((commitid = commitid_find(repo, tcommitid)) == NULL)
		error(1, 0, "commitid not found: %s", tcommitid == NULL ?
		    "(latest)" : tcommitid);

	if (commitid->previous == NULL && !commitid->genesis)
		error(1, 0, "commitid has no previous but is not genesis: %s",
		    commitid->commitid);

	if (commitid->genesis) {
		cvs_output("Genesis: ", 0);
		cvs_output(commitid->commitid, 0);
		cvs_output("\n", 1);

		return 0;
	}

	cvs_output("Commitid: ", 0);
	cvs_output(commitid->commitid, 0);
	cvs_output("\n", 1);

	return show_commitid(commitid);
}

int
show_commitid(CommitId *commitid)
{
	Node *head, *fn;
	int didlog = 0;

	cvs_output("Previous: ", 0);
	cvs_output(commitid->previous, 0);
	cvs_output("\n", 1);

	/*
	 * walk changeset file list, find the commitid revision in each file
	 * and generate a diff on it.
	 */
	head = commitid->files->list;
	for (fn = head->next; fn != head; fn = fn->next) {
		RCSNode *rcs;
		char *diffargs[] = { "rdiff", "-apuZ", "-r", "-r", "" };
		CommitIdFile *cif = (CommitIdFile *)fn->data;
		ssize_t len;
		char *rcspath, *rcsfile, *slash;

		if (commitid->repo == NULL)
			error(1, 0, "show_commmitid: null repo");
		if (cif->filename == NULL)
			error(1, 0, "show_commitid: file with no filename");

		len = strlen(current_parsed_root->directory) + 1 +
		    strlen(commitid->repo) + 1 + strlen(cif->filename) + 1;
		rcspath = xmalloc(len);
		rcsfile = xmalloc(strlen(cif->filename) + 1);

		/*
		 * rcspath is something like "bin/csh/err.c" but we need to
		 * pass "err.c" and "/cvs/src/bin/csh" to RCS_parse so it can
		 * try an Attic path of "/cvs/src/bin/csh/Attic/err.c"
		 */

		snprintf(rcspath, len, "%s/%s/%s",
		    current_parsed_root->directory, commitid->repo,
		    cif->filename);
		slash = strrchr(rcspath, '/');
		if (!slash)
			error(1, 0, "can't find slash in %s", rcspath);

		strlcpy(rcsfile, slash + 1, strlen(slash));
		*slash = '\0';

		rcs = RCS_parse(rcsfile, rcspath);
		if (rcs == NULL)
			error(1, 0, "can't find RCS file %s in %s", rcsfile,
			    rcspath);

		if (!didlog) {
			/*
			 * if we have another revision of this same file in
			 * this same changeset, use that one's log, since it's
			 * probably the 1.1.1.1 commit (vs. our 1.1) which has
			 * the actual commit message instead of just "Initial
			 * revision".
			 */
			if (fn->next && fn->next != head) {
				CommitIdFile *ncif =
				    (CommitIdFile *)fn->next->data;
				if (ncif != NULL && strcmp(cif->filename,
				    ncif->filename) == 0) {
					show_commitid_header(rcs,
					    ncif->revision);
					didlog = 1;
				}
			}
		}

		if (!didlog) {
			show_commitid_header(rcs, cif->revision);
			didlog = 1;
		}

		diffargs[2] = xmalloc(20);
		snprintf(diffargs[2], 20, "-r%s", cif->prev_revision);

		diffargs[3] = xmalloc(20);
		snprintf(diffargs[3], 20, "-r%s", cif->revision);

		diffargs[4] = xmalloc(strlen(commitid->repo) + 1 +
		    strlen(cif->filename) + 1);
		sprintf(diffargs[4], "%s/%s", commitid->repo, cif->filename);

		patch(sizeof(diffargs) / sizeof(diffargs[0]), diffargs);

#ifdef DEBUG
		fprintf(stderr, "%s: %s %s %s %s %s\n",
			__func__, diffargs[0], diffargs[1], diffargs[2],
			diffargs[3], diffargs[4]);
#endif

		free(diffargs[4]);
		free(diffargs[3]);
		free(diffargs[2]);
	}

	return 0;
}

void
show_commitid_header(RCSNode *rcs, char *revision)
{
	Node *n, *p;
	RCSVers *ver;
	int year, mon, mday, hour, min, sec;
	char buf[1024];
	char *line;

	RCS_fully_parse(rcs);

	n = findnode(rcs->versions, revision);
	if (n == NULL)
		error (1, 0, "%s: no revision %s", rcs->path, revision);

	ver = (RCSVers *)n->data;

	cvs_output("Author:   ", 0);
	cvs_output(ver->author, 0);
	cvs_output("\n", 1);

	if (sscanf(ver->date, SDATEFORM, &year, &mon, &mday, &hour, &min,
	    &sec) != 6)
		error(1, 0, "malformed date: %s", ver->date);

	if (year < 1900)
		year += 1900;

	p = findnode(ver->other, "log");
	if (p == NULL || !p->data)
		error(1, 0, "no log found on first commit");

	sprintf(buf, "%04d/%02d/%02d %02d:%02d:%02d", year, mon, mday, hour,
	    min, sec);

	cvs_output("Date:     ", 0);
	cvs_output(buf, 0);
	cvs_output("\n\n", 2);

	while ((line = strsep(&p->data, "\n")) != NULL) {
		if (*line == '\0')
			break;

		cvs_output("    ", 4);
		cvs_output(line, 0);
		cvs_output("\n", 1);
	}

	cvs_output("\n", 1);
}
