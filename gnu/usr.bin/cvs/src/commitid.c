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

#include <sys/types.h>
#include <assert.h>
#include "cvs.h"

char *
commitid_repo_base(void)
{
	char *repo = Short_Repository(Name_Repository(NULL, NULL));
	char *slash;

	slash = strchr(repo, '/');
	if (slash)
		*slash = '\0';

	/*
	 * this could happen if someone committed to two root-level dirs at
	 * once, which we don't want anyway.
	 */
	if (repo[0] == '\0')
		error(1, 0, "invalid repo base");

	return repo;
}

char *
commitid_filename(char *repo, int genesis)
{
	char *fn;

	if (!genesis && (repo == NULL || repo[0] == '\0'))
		error(1, 0, "invalid repo");

	if (genesis) {
		fn = xmalloc(strlen(current_parsed_root->directory) +
		    sizeof(CVSROOTADM) + sizeof(CVSROOTADM_COMMITID_0) + 1);
		sprintf(fn, "%s/%s/%s", current_parsed_root->directory,
		    CVSROOTADM, CVSROOTADM_COMMITID_0);
	} else {
		fn = xmalloc(strlen(current_parsed_root->directory) +
		    sizeof(CVSROOTADM) + sizeof(CVSROOTADM_COMMITIDS) + 1 +
		    strlen(repo) + 1);
		sprintf(fn, "%s/%s/%s-%s", current_parsed_root->directory,
		    CVSROOTADM, CVSROOTADM_COMMITIDS, repo);
	}

	return fn;
}

FILE *
commitid_logfile(char *repo)
{
	char *fn;
	FILE *fp;

	fn = commitid_filename(repo, 0);

	if ((fp = fopen(fn, "r")) == NULL)
		return NULL;

	free(fn);

	return fp;
}

CommitId *
commitid_parse(char *repo, char *id)
{
	CommitId *out;
	unsigned int version;
	char hash[COMMITID_HASH_LENGTH];
	unsigned long changeset = 0;
	char fmt[22];
	int res;

	if (!strlen(id))
		return NULL;

	/* %04u-%64s-%07lu */
	snprintf(fmt, sizeof(fmt), "%%0%du-%%%ds-%%0%dlu",
	    COMMITID_VERSION_LENGTH, COMMITID_HASH_LENGTH,
	    COMMITID_CHANGESET_LENGTH);

	res = sscanf(id, fmt, &version, &hash, &changeset);
	if (res != 3) {
		error(0, 0, "malformed commitid %s", id);
		return NULL;
	}

	/* eventually be able to parse old versions */
	if (version != COMMITID_VERSION)
		return NULL;

	out = xmalloc(sizeof(CommitId));
	memset(out, 0, sizeof(CommitId));
	out->commitid = xstrdup(id);
	out->version = version;
	out->hash = xstrdup(hash);
	out->changeset = changeset;
	out->repo = xstrdup(repo);
	out->files = getlist();

	return out;
}

CommitId *
commitid_genesis(void)
{
	char *fn;
	FILE *fp;
	ssize_t len, ps = 0;
	char *line = NULL;
	CommitId *genesis;

	fn = commitid_filename(NULL, 1);
	if ((fp = fopen(fn, "r")) == NULL)
		return NULL;

	if (!(len = getline(&line, &ps, fp))) {
		fclose(fp);
		return NULL;
	}

	if (line[len - 1] == '\n') {
		line[len - 1] = '\0';
		len--;
	}

	genesis = commitid_parse(NULL, line);
	if (genesis == NULL)
		error(1, 0, "failed parsing genesis line %s", line);

	genesis->genesis = 1;

	return genesis;
}

void
commitid_free(CommitId *id)
{
	if (id == NULL)
		return;

	if (id->repo != NULL)
		free(id->repo);
	if (id->commitid != NULL)
		free(id->commitid);
	if (id->hash != NULL)
		free(id->hash);
	if (id->files != NULL)
		dellist(&id->files);

	free(id);
}

CommitId *
commitid_find(char *repo, char *findid)
{
	FILE *fp;
	CommitId *tmpid = NULL, *retid = NULL, *previd = NULL;
	char *line = NULL, *tab = NULL, *files = NULL, *ep, *revspec;
	ssize_t len;
	size_t ps = 0;
	long long findcs = -1;
	int isint = 0, x;
	CommitId *genesis;

	genesis = commitid_genesis();
	if (genesis == NULL) {
		error(1, 0, "commitid history tracking not enabled");
		return NULL;
	}

	if (findid != NULL && !strlen(findid))
		findid = NULL;

	if (findid != NULL &&
	    (strcmp(findid, "0") == 0 || strcmp(findid, "genesis") == 0))
		return genesis;

	fp = commitid_logfile(repo);
	if (fp == NULL)
		return NULL;

	if (findid != NULL && strlen(findid)) {
		isint = 1;
		for (x = 0; x < strlen(findid); x++)
			if (!isdigit(findid[x]))
				isint = 0;

		if (isint) {
			findcs = strtoul(findid, &ep, 10);
			if (findid == ep || *ep != '\0')
				findcs = -1;
		}
	}

	/*
	 * if we just want the latest commitid, seek to the end of the file and
	 * position fp right after the fourth-to-last newline (we must read two
	 * commitids to set the final's parent)
	 */
	if (findid == NULL && findcs == -1) {
		int nlines = 0;
		char c;

		fseek(fp, 0, SEEK_END);
		while (ftell(fp) > 0 && nlines != 4) {
			if (fseek(fp, -1, SEEK_CUR) || ftell(fp) <= 0 ||
			    fread(&c, 1, 1, fp) != 1 || fseek(fp, -1, SEEK_CUR))
				break;

			if (c == '\n')
				nlines++;
		}
	}

	while ((len = getline(&line, &ps, fp)) != -1) {
		int match = 0;

		if (line[len - 1] == '\n') {
			line[len - 1] = '\0';
			len--;
		}

		if ((tab = strchr(line, '\t')) == NULL)
			continue;

		*tab = '\0';
		tab++;

		if ((tmpid = commitid_parse(repo, line)) == NULL)
			error(1, 0, "failed parsing commandid line %s", line);

		if (findcs >= 0) {
			/* match on changeset id */
			if (tmpid->changeset == findcs)
				match = 1;
		} else if (findid == NULL)
			/* keep matching to find final commitid */
			match = 1;
		else if (strncmp(findid, tmpid->commitid,
		    strlen(findid)) == 0) {
			/*
			 * need to go hunting - match on first part of commitid
			 * chars, allowing for shortened unless it matches more
			 * than one
			 */
			if (retid != NULL) {
				match = 0;
				if (previd) {
					commitid_free(previd);
					previd = NULL;
				}
				retid = NULL;
				error(0, 0, "commitid \"%s\" is ambiguous",
				    findid);
				break;
			}

			match = 1;
		}

		if (match) {
			retid = tmpid;
			tmpid = NULL;

			if (previd) {
				if (previd->changeset != retid->changeset - 1) {
					error(0, 0, "commitid \"%s\" previous "
					    "incorrectly \"%s\"",
					    retid->commitid, previd->commitid);
					retid = NULL;
					break;
				}

				retid->previous = xstrdup(previd->commitid);
			} else if (retid->changeset == 0) {
				CommitId *genesis = commitid_genesis();
				retid->previous = xstrdup(genesis->commitid);
				commitid_free(genesis);
			}

			if (files != NULL)
				free(files);
			files = xmalloc(strlen(tab) + 2);
			strlcpy(files, tab, strlen(tab) + 1);

			if (findcs >= 0 ||
			    (findid != NULL &&
			    strlen(findid) == COMMITID_LENGTH))
				/* no possible duplicates, finish early */
				break;

			/*
			 * assuming we loop again, we weren't the final match,
			 * so stage us to be the next one's previous
			 */
			if (previd)
				commitid_free(previd);
			previd = retid;
		} else {
			if (previd && previd != retid)
				commitid_free(previd);
			previd = tmpid;
			tmpid = NULL;
		}
	}
	fclose(fp);
	free(line);

	if (retid) {
		/* have a match to return, parse file list */
		retid->repo = xstrdup(repo);

		if (files == NULL)
			error(1, 0, "found commitid match but no files");

		while ((revspec = strsep(&files, "\t")) != NULL) {
			Node *f;
			char *r1, *r2, *fspec, *branch, *fname;
			CommitIdFile *cif;

			if (*revspec == '\0')
				break;

			r1 = xmalloc(strlen(revspec) - 2 + 1);
			r2 = xmalloc(strlen(revspec) - 2 + 1);
			fspec = xmalloc(strlen(revspec) - 3 + 1);
			if (sscanf(revspec, "%[^:]:%[^:]:%s",
			    r1, r2, fspec) != 3)
				error(1, 0, "failed parsing commitid "
				    "revision spec %s", revspec);

			/* ":%[^:]:" won't match "::" */
			if (*fspec == ':') {
				branch = xstrdup("");
				fname = xstrdup(fspec + 1);
			} else {
				branch = xmalloc(strlen(fspec) - 2 + 1);
				fname = xmalloc(strlen(fspec) - 2 + 1);
				if (sscanf(fspec, "%[^:]:%s", branch,
				    fname) != 2)
					error(1, 0, "failed parsing "
					    "branch/file %s", fspec);
			}

			f = getnode();
			f->key = xmalloc(strlen(fname) + 1 + strlen(r2) + 1);
			snprintf(f->key, strlen(fname) + 1 + strlen(r2),
			    "%s:%s", fname, r2);
			f->data = xmalloc(sizeof(CommitIdFile));
			cif = (CommitIdFile *)(f->data);
			cif->filename = xstrdup(fname);
			cif->revision = xstrdup(r2);
			cif->prev_revision = xstrdup(r1);
			cif->branch = xstrdup(branch);
			addnode(retid->files, f);

			free(fname);
			free(branch);
			free(r2);
			free(r1);
		}
	}

	if (previd != NULL && previd != retid)
		commitid_free(previd);
	if (files != NULL)
		free(files);

	if (retid && retid->changeset == 1) {
		if (retid->previous == NULL)
			retid->previous = xstrdup(genesis->commitid);

		if (strcmp(retid->previous, genesis->commitid) != 0)
			error(1, 0, "changeset 1 has invalid previous: %s",
			    retid->previous);
	}

	return retid;
}

CommitId *
commitid_gen_start(char *repo, unsigned long changeset)
{
	CommitId *out;

	if ((repo == NULL || !strlen(repo)) && changeset)
		error(1, 0, "creating commitid in blank repo with changeset "
		    "%lu", changeset);

	out = xmalloc(sizeof(CommitId));
	memset(out, 0, sizeof(CommitId));
	out->repo = xstrdup(repo);
	out->version = COMMITID_VERSION;
	out->hash = xmalloc(COMMITID_HASH_LENGTH + 1);
	out->changeset = changeset;
	out->commitid = xmalloc(COMMITID_LENGTH + 1);
	out->files = getlist();
	out->genesis = (changeset == 0);

	SHA512_256Init(&out->sha_ctx);

	return out;
}

CommitId *
commitid_gen_start_legacy(char *repo)
{
	CommitId *out;
	int i = 0;
	uint32_t c;

	out = xmalloc(sizeof(CommitId));
	memset(out, 0, sizeof(CommitId));
	out->repo = xstrdup(repo);
	out->commitid = xmalloc(COMMITID_LEGACY_LENGTH + 1);
	out->files = getlist();
	out->legacy = 1;

	while (i <= COMMITID_LEGACY_LENGTH) {
		c = arc4random_uniform(75) + 48;
		if ((c >= 48 && c <= 57) || (c >= 65 && c <= 90) ||
		    (c >= 97 && c <= 122)) {
			out->commitid[i] = c;
			i++;
		}
	}
	out->commitid[COMMITID_LEGACY_LENGTH] = '\0';

	return out;
}

/* hook mechanism for cvs_output() */
CommitId *_cur_capture_commitid;
void
_commitid_gen_add_output_hash(const char *str, size_t len)
{
	if (_cur_capture_commitid == NULL)
		error(1, 0, "running through %s with no commitid\n", __func__);

#ifdef DEBUG
	fprintf(stderr, "%s", str);
#endif
	commitid_gen_add_buf(_cur_capture_commitid, (uint8_t *)str, len);
}

void
commitid_gen_add_show(CommitId *id)
{
	if (id->legacy)
		return;

	_cur_capture_commitid = id;
	cvs_output_capture = _commitid_gen_add_output_hash;
	show_commitid(id);
	_cur_capture_commitid = NULL;
}

void
commitid_gen_add_buf(CommitId *id, uint8_t *buf, size_t len)
{
	if (id->legacy)
		return;

	SHA512_256Update(&id->sha_ctx, buf, len);
}

void
commitid_gen_add_diff(CommitId *id, char *filename, char *rcsfile, char *r1,
    char *r2, char *branch)
{
	Node *f;
	CommitIdFile *cif;

	if ((f = findnode(id->files, filename))) {
		if (f->data != NULL &&
		    strcmp(((CommitIdFile *)f->data)->revision, r2) == 0)
			error(1, 0, "file %s with rev %s already exists in "
			    "file list\n", filename, r2);
	}

#ifdef DEBUG
	fprintf(stderr, "%s: %s, %s, %s, %s\n", __func__, filename, rcsfile,
	    r1, r2);
#endif

	f = getnode();
	f->key = xmalloc(strlen(filename) + 1 + strlen(r2) + 1);
	snprintf(f->key, strlen(filename) + 1 + strlen(r2), "%s:%s", filename,
	    r2);
	f->data = xmalloc(sizeof(CommitIdFile));
	bzero(f->data, sizeof(CommitIdFile));
	cif = (CommitIdFile *)f->data;
	cif->filename = xstrdup(filename);
	cif->rcsfile = xstrdup(rcsfile);
	cif->revision = xstrdup(r2);
	cif->prev_revision = xstrdup(r1);
	if (branch != NULL)
		cif->branch = xstrdup(branch);
	addnode(id->files, f);
}

void
commitid_gen_add_rand(CommitId *id, size_t len)
{
	char *rbuf = xmalloc(len);

	arc4random_buf(rbuf, len);
	commitid_gen_add_buf(id, rbuf, len);
	free(rbuf);
}

void
commitid_gen_final(CommitId *id)
{
	uint8_t *thash = xmalloc(COMMITID_HASH_LENGTH + 1);
	char fmt[22];
	int i;

	if (id->legacy)
		return;

	SHA512_256Final(thash, &id->sha_ctx);

	/* digest to hex */
	id->hash[0] = '\0';
	for (i = 0; i < (COMMITID_HASH_LENGTH / 2); i++)
		snprintf(id->hash, ((i * 2) + 2 + 1), "%s%02x", id->hash,
		    thash[i]);

	free(thash);

	/* %04u-%64s-%07lu */
	snprintf(fmt, sizeof(fmt), "%%0%du-%%%ds-%%0%dlu",
	    COMMITID_VERSION_LENGTH, COMMITID_HASH_LENGTH,
	    COMMITID_CHANGESET_LENGTH);

	snprintf(id->commitid, COMMITID_LENGTH + 1, fmt, id->version,
	    id->hash, id->changeset);

	assert(strlen(id->commitid) == COMMITID_LENGTH);
}

void
commitid_store(CommitId *id)
{
	FILE *fp;
	Node *head, *fn, *n;
	RCSNode *rcs;
	RCSVers *delta;
	char *rev;
	int wrotefiles = 0;

	if (id->genesis)
		goto write_log;

	head = id->files->list;
	for (fn = head->next; fn != head; fn = fn->next) {
		char *trcs;
		CommitIdFile *cif = (CommitIdFile *)fn->data;

		if (cif->rcsfile == NULL)
			error(1, 0, "can't store commitid for file %s "
			    "without rcsfile", cif->filename);

		trcs = xmalloc(strlen(current_parsed_root->directory) + 1 +
		    strlen(id->repo) + 1);
		snprintf(trcs, strlen(current_parsed_root->directory) + 1 +
		    strlen(id->repo) + 1, "%s/%s",
		    current_parsed_root->directory, id->repo);

		rcs = RCS_parse(cif->filename, trcs);
		if (rcs == NULL)
			error(1, 0, "can't find RCS file %s in %s",
			    cif->filename, trcs);

		RCS_fully_parse(rcs);

		rev = RCS_gettag(rcs, cif->revision, 1, NULL);
		if (rev == NULL)
			error (1, 0, "%s: no revision %s", rcs->path,
			    cif->revision);

#ifdef DEBUG
		fprintf(stderr, "adding commitid %s to rev %s of %s (%s)\n",
			id->commitid, rev, cif->rcsfile, cif->branch ?
			cif->branch : "head");
#endif

		n = findnode(rcs->versions, rev);
		delta = (RCSVers *)n->data;

		if (delta->other_delta == NULL)
		    delta->other_delta = getlist();

		if ((n = findnode(delta->other_delta, "commitid")))
			n->data = xstrdup(id->commitid);
		else {
			n = getnode();
			n->type = RCSFIELD;
			n->key = xstrdup("commitid");
			n->data = xstrdup(id->commitid);
			addnode(delta->other_delta, n);
		}

		RCS_rewrite(rcs, NULL, NULL);

		if (!wrotefiles)
			wrotefiles++;

		free(rcs);
		free(rev);
		free(n);
	}

	if (id->legacy || (!id->genesis && !wrotefiles))
		return;

	/* all written, append to repo-specific log */
write_log:
	fp = open_file(commitid_filename(id->repo, id->genesis), "a");
	fprintf(fp, "%s", id->commitid);

	if (!id->genesis) {
		head = id->files->list;
		for (fn = head->next; fn != head; fn = fn->next) {
			CommitIdFile *cif = (CommitIdFile *)fn->data;
			fprintf(fp, "\t%s:%s:%s:%s",
			    cif->prev_revision,
			    cif->revision,
			    cif->branch == NULL ? "" : cif->branch,
			    cif->filename);
		}
	}

	fprintf(fp, "\n");
	fclose (fp);
}
