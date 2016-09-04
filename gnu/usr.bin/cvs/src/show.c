#include <assert.h>
#include "cvs.h"
#include "getline.h"

static const char *const show_usage[] = {
	"Usage: %s %s [commitid]\n",
	"(Specify the --help global option for a list of other help options)\n",
	NULL
};

int
show(int argc, char **argv)
{
	char *tcommitid = NULL;
	CommitId *commitid;
	Node *head, *fn;
	int didlog = 0;

	if (argc == 2)
		tcommitid = xstrdup(argv[1]);
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

	if ((commitid = commitid_find(tcommitid)) == NULL)
		error(1, 0, "commitid not found: %s", tcommitid);

	if (commitid->parent != NULL) {
		cvs_output("Parent:  ", 0);
		cvs_output(commitid->parent, 0);
		cvs_output("\n", 1);
	}

	cvs_output("Commitid: ", 0);
	cvs_output(commitid->commitid, 0);
	cvs_output("\n", 1);

	/*
	 * walk changeset file list, find the commitid revision in each file
	 * and generate a diff -uNP on it.  the log messages should be the same
	 * on every file, so just print the first one.
	 */
	head = commitid->files->list;
	for (fn = head->next; fn != head; fn = fn->next) {
		Node *revhead, *rev, *p;
		RCSNode *rcsfile;
    		char *repo = Name_Repository(NULL, NULL);
		struct revlist *revlist;
		struct option_revlist *r;
		RCSVers *ver;
		char *thisver, *prevver;
		char *diffargs[] = { "diff", "-uNp", "-r", "-r", "" };

		rcsfile = RCS_parse(fn->key, repo);
		if (rcsfile == NULL)
			error(1, 0, "can't find file %s in %s", fn->key, repo);

		RCS_fully_parse(rcsfile);

		revhead = rcsfile->versions->list;
		rev = revhead->next;
		ver = (RCSVers *)rev->data;

		if (rev == revhead)
			error(1, 0, "%s: no previous version to %s",
			    fn->key, ver->version);

		p = findnode(ver->other_delta, "commitid");
		if (p == NULL || !p->data)
			continue;

		if (strcmp(commitid->commitid, p->data) != 0)
			continue;

		if (!didlog) {
			int year, mon, mday, hour, min, sec;
			char buf[1024];
			char *line;

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

		/*
		 * generate a diff between this version and this file's
		 * previous version, blindly assuming they are in order
		 */
		thisver = xstrdup(ver->version);

		rev = rev->next;
		if (rev == revhead)
			/* end of the line, generated from nothing */
			prevver = xstrdup("0");
		else {
			ver = (RCSVers *)rev->data;
			prevver = xstrdup(ver->version);
		}

		diffargs[2] = xmalloc(2 + strlen(prevver));
		sprintf(diffargs[2], "-r%s", prevver);

		diffargs[3] = xmalloc(2 + strlen(thisver));
		sprintf(diffargs[3], "-r%s", thisver);

		diffargs[4] = xstrdup(fn->key);

		diff(sizeof(diffargs) / sizeof(diffargs[0]), diffargs);

		free(diffargs[4]);
		free(diffargs[3]);
		free(diffargs[2]);
		free(prevver);
		free(thisver);
	}

	return 0;
}
