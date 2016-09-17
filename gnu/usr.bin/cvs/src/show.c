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

int
show(int argc, char **argv)
{
	char *tcommitid, *repo;
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
		error(1, 0, "commitid not found: %s", tcommitid);

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

	return show_commitid(commitid, 0);
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
	 * and generate a diff -uNP on it.  the log messages should be the same
	 * on every file, so just print the first one.
	 */
	head = commitid->files->list;
	for (fn = head->next; fn != head; fn = fn->next) {
		RCSNode *rcs;
		char *diffargs[] = { "rdiff", "-upZ", "-r", "-r", "" };
		ssize_t len = strlen(current_parsed_root->directory) +
		    1 + strlen(commitid->repo) + 1;
		char *rcspath = xmalloc(len);

		snprintf(rcspath, len, "%s/%s", current_parsed_root->directory,
		    commitid->repo);

		rcs = RCS_parse(fn->key, rcspath);
		if (rcs == NULL)
			error(1, 0, "can't find RCS file %s in %s", fn->key,
			    rcspath);

		if (!didlog) {
			Node *revhead, *rev, *n, *p;
			RCSVers *ver;
			int year, mon, mday, hour, min, sec;
			char buf[1024];
			char *line;

			RCS_fully_parse(rcs);

			n = findnode(rcs->versions,
			    ((CommitIdFile *)fn->data)->revision);
			if (n == NULL)
				error (1, 0, "%s: no revision %s", rcs->path,
				    ((CommitIdFile *)fn->data)->revision);

			ver = (RCSVers *)n->data;

			cvs_output("Author:   ", 0);
			cvs_output(ver->author, 0);
			cvs_output("\n", 1);

			if (sscanf(ver->date, SDATEFORM, &year, &mon, &mday,
			    &hour, &min, &sec) != 6)
				error(1, 0, "malformed date: %s", ver->date);

			if (year < 1900)
				year += 1900;

			p = findnode(ver->other, "log");
			if (p == NULL || !p->data)
				error(1, 0, "no log found on first commit");

			sprintf(buf, "%04d/%02d/%02d %02d:%02d:%02d",
			    year, mon, mday, hour, min, sec);

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

			didlog = 1;
		}

		diffargs[2] = xmalloc(20);
		snprintf(diffargs[2], 20, "-r%s",
		    ((CommitIdFile *)(fn->data))->prev_revision);

		diffargs[3] = xmalloc(20);
		snprintf(diffargs[3], 20, "-r%s",
		    ((CommitIdFile *)(fn->data))->revision);

		diffargs[4] = xmalloc(strlen(commitid->repo) + 1 +
		    strlen(fn->key) + 1);
		sprintf(diffargs[4], "%s/%s", commitid->repo, fn->key);

		patch(sizeof(diffargs) / sizeof(diffargs[0]), diffargs);

		free(diffargs[4]);
		free(diffargs[3]);
		free(diffargs[2]);
	}

	return 0;
}
