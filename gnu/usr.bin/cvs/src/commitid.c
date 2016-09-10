#include "cvs.h"

#include <sys/types.h>
#include <assert.h>

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
	char fmt[20];
	int res;

	if (!strlen(id))
		return NULL;

	/* %04u%64s%07lu */
	snprintf(fmt, sizeof(fmt), "%%0%du%%%ds%%0%dlu",
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
	out->commitid = xstrdup(id);
	out->version = version;
	out->hash = xstrdup(hash);
	out->changeset = changeset;
	out->repo = xstrdup(repo);
	out->files = getlist();
	out->genesis = 0;

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
	char *line = NULL, *tab = NULL, *files, *ep;
	const char *errstr;
	ssize_t len;
	size_t ps = 0;
	long long findcs = -1;
	int res = 0, isint = 0, x;

	if (findid != NULL && !strlen(findid))
		findid = NULL;

	if (findid != NULL && (strcmp(findid, "0") == 0 ||
	strcmp(findid, "genesis") == 0))
		return commitid_genesis();

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
	 * TODO: if we're asking for latest (findid == NULL && findcs == -1),
	 * seek to the end of the file and walk backwards
	 */

	while ((len = getline(&line, &ps, fp)) != -1) {
		if (line[len - 1] == '\n') {
			line[len - 1] = '\0';
			len--;
		}

		if ((tab = strstr(line, "\t")) == NULL)
			continue;

		files = malloc(strlen(tab) + 1);
		strlcpy(files, tab + 1, strlen(tab) + 1);

		tab[0] = '\0';
		len = strlen(line);

		if ((tmpid = commitid_parse(repo, line)) == NULL)
			error(1, 0, "failed parsing commandid line %s", line);

		if (findcs >= 0) {
			/* match on changeset id */
			if (tmpid->changeset == findcs)
				res = 1;
		} else if (findid == NULL)
			/* keep matching to find latest commitid */
			res = 1;
		else if (strcmp(findid, tmpid->commitid) == 0) {
			/*
			 * need to go hunting - match on first part of commitid
			 * chars, allowing for shortened unless it matches more
			 * than one
			 */
			if (res) {
				res = 0;
				if (retid) {
					commitid_free(retid);
					retid = NULL;
				}
				error(0, 0, "commitid \"%s\" is ambiguous",
				    findid);
				break;
			}

			res = 1;
		}

		if (res && tmpid) {
			char *file;

			retid = tmpid;
			tmpid = NULL;

			if (previd)
				retid->previous = xstrdup(previd->commitid);
			else {
				CommitId *genesis = commitid_genesis();
				retid->previous = xstrdup(genesis->commitid);
				commitid_free(genesis);
			}

			while ((file = strsep(&files, "\t")) != NULL) {
				Node *f;
				char r1[15], r2[15];
				char *fname;
				int ret;
				CommitIdFile *cif;

				if (*file == '\0')
					break;

				fname = xmalloc(strlen(file));
				if (sscanf(file, "%[^:]:%[^:]:%s", &r1, &r2,
				    fname) != 3)
					error(1, 0, "failed parsing commitid "
					    "file spec %s", file);

				f = getnode();
				f->key = xstrdup(fname);
				f->data = xmalloc(sizeof(CommitIdFile));
				((CommitIdFile *)(f->data))->revision =
				    xstrdup(r2);
				((CommitIdFile *)(f->data))->prev_revision =
				    xstrdup(r1);
				addnode(retid->files, f);

				free(fname);
			}

			if (findcs >= 0 ||
			    (findid != NULL &&
			    strlen(findid) == COMMITID_LENGTH))
				/* no possible duplicates, finish early */
				break;

			if (previd)
				commitid_free(previd);

			previd = retid;
		} else {
			if (previd)
				commitid_free(previd);

			previd = tmpid;
			tmpid = NULL;
		}
	}
	fclose(fp);

	if (previd != retid)
		commitid_free(previd);

	if (retid)
		retid->repo = xstrdup(repo);

	return retid;
}

CommitId *
commitid_gen_start(char *repo, unsigned long changeset)
{
	CommitId *out;

	if ((repo == NULL || !strlen(repo)) && changeset)
		error(1, 0, "creating commitid in blank repo with changeset "
		    "%d", changeset);

	out = xmalloc(sizeof(CommitId));
	out->repo = xstrdup(repo);
	out->version = COMMITID_VERSION;
	out->hash = xmalloc((SHA256_DIGEST_LENGTH * 2) + 1);
	out->changeset = changeset;
	out->commitid = xmalloc(COMMITID_LENGTH + 1);
	out->files = getlist();
	out->genesis = (changeset == 0);

	SHA256Init(&out->sha_ctx);

	return out;
}

/* hook mechanism for cvs_output() */
CommitId *_cur_capture_commitid;
void
_commitid_gen_add_output_hash(const char *str, size_t len)
{
	if (!_cur_capture_commitid)
		error(1, 0, "running through %s with no commitid\n", __func__);

#ifdef DEBUG
	fprintf(stderr, "%s", str);
#endif
	commitid_gen_add_buf(_cur_capture_commitid, (uint8_t *)str, len);
}

void
commitid_gen_add_show(CommitId *id)
{
	_cur_capture_commitid = id;
	cvs_output_capture = _commitid_gen_add_output_hash;
	show_commitid(id);
	_cur_capture_commitid = NULL;
}

int
commitid_gen_add_buf(CommitId *id, uint8_t *buf, size_t len)
{
	SHA256Update(&id->sha_ctx, buf, len);

	return 1;
}

void
commitid_gen_add_diff(CommitId *id, char *filename, char *rcsfile, char *r1,
    char *r2)
{
	Node *f;
	CommitIdFile *cif;

	if (f = findnode(id->files, filename)) {
		if (f->data != NULL &&
		    strcmp(((CommitIdFile *)f->data)->revision, r2) == 0)
			error(1, 0, "file %s with rev %s already exists in "
			    "file list\n", filename);
	}

#ifdef DEBUG
	printf("%s: %s, %s, %s, %s\n", __func__, filename, rcsfile, r1, r2);
#endif

	f = getnode();
	f->key = xstrdup(filename);
	f->data = xmalloc(sizeof(CommitIdFile));
	cif = (CommitIdFile *)f->data;
	cif->rcsfile = xstrdup(rcsfile);
	cif->revision = xstrdup(r2);
	cif->prev_revision = xstrdup(r1);
	addnode(id->files, f);
}

int
commitid_gen_add_rand(CommitId *id, size_t len)
{
	char *rbuf = xmalloc(100);

	arc4random_buf(rbuf, 100);
	commitid_gen_add_buf(id, rbuf, 100);
	free(rbuf);

	return 1;
}

int
commitid_gen_final(CommitId *id)
{
	uint8_t *thash = xmalloc((SHA256_DIGEST_LENGTH * 2) + 1);
	char fmt[20];
	int i;

	SHA256Final(thash, &id->sha_ctx);

	/* digest to hex */
	id->hash[0] = '\0';
	for (i = 0; i < SHA256_DIGEST_LENGTH; i++)
		snprintf(id->hash, ((i * 2) + 2 + 1), "%s%02x", id->hash,
		    thash[i]);

	free(thash);

	/* %04u%64s%07lu */
	snprintf(fmt, sizeof(fmt), "%%0%du%%%ds%%0%dlu",
	    COMMITID_VERSION_LENGTH, COMMITID_HASH_LENGTH,
	    COMMITID_CHANGESET_LENGTH);

	snprintf(id->commitid, COMMITID_LENGTH + 1, fmt, id->version,
	    id->hash, id->changeset);

	assert(strlen(id->commitid) == COMMITID_LENGTH);

	return 1;
}

void
commitid_store(CommitId *id)
{
	FILE *fp;
	Node *head, *fn, *n;
	RCSNode *rcs;
	RCSVers *delta;
	char *rev;

	if (id->genesis)
		goto write_log;

	head = id->files->list;
	for (fn = head->next; fn != head; fn = fn->next) {
		char *trcs, *tdir;

		if (((CommitIdFile *)fn->data)->rcsfile == NULL)
			error(1, 0, "can't store commitid for file %s "
			    "without rcsfile", fn->key);

		trcs = xmalloc(strlen(current_parsed_root->directory) + 1 +
		    strlen(id->repo) + 1);
		snprintf(trcs, strlen(current_parsed_root->directory) + 1 +
		    strlen(id->repo) + 1, "%s/%s",
		    current_parsed_root->directory, id->repo);

		rcs = RCS_parse(fn->key, trcs);
		if (rcs == NULL)
			error(1, 0, "can't find RCS file %s in %s", fn->key,
			    trcs);

		RCS_fully_parse(rcs);

		rev = RCS_gettag(rcs, ((CommitIdFile *)fn->data)->revision, 1,
		    NULL);
		if (rev == NULL)
			error (1, 0, "%s: no revision %s", rcs->path,
			    ((CommitIdFile *)fn->data)->revision);

#ifdef DEBUG
		printf("adding commitid %s to rev %s of %s\n",
			id->commitid, rev,
			((CommitIdFile *)fn->data)->rcsfile);
#endif

		n = findnode(rcs->versions, rev);
		delta = (RCSVers *)n->data;

		if (delta->other_delta == NULL)
		    delta->other_delta = getlist();

		if (n = findnode(delta->other_delta, "commitid"))
			n->data = xstrdup(id->commitid);
		else {
			n = getnode();
			n->type = RCSFIELD;
			n->key = xstrdup("commitid");
			n->data = xstrdup(id->commitid);
			addnode(delta->other_delta, n);
		}

		RCS_rewrite(rcs, NULL, NULL);

		free(rcs);
		free(rev);
		free(n);
	}

	/* all written, append to repo-specific log */
write_log:
	fp = open_file(commitid_filename(id->repo, id->genesis), "a");
	fprintf(fp, "%s", id->commitid);

	if (!id->genesis) {
		head = id->files->list;
		for (fn = head->next; fn != head; fn = fn->next) {
			CommitIdFile *cif = (CommitIdFile *)fn->data;
			fprintf(fp, "\t%s:%s:%s", cif->prev_revision,
			    cif->revision, fn->key);
		}
	}

	fprintf(fp, "\n");
	fclose (fp);
}
