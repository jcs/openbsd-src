#include "cvs.h"

#include <sys/types.h>
#include <assert.h>

char *
commitids_filename(void)
{
	char *repo = Short_Repository(Name_Repository(NULL, NULL));
	char *fn;

	fn = xmalloc(strlen(current_parsed_root->directory) +
	    sizeof(CVSROOTADM) + sizeof(CVSROOTADM_COMMITIDS) + 1 +
	    strlen(repo) + 1);
	sprintf(fn, "%s/%s/%s-%s", current_parsed_root->directory, CVSROOTADM,
	    CVSROOTADM_COMMITIDS, repo);

	return fn;
}

int
commitids_logging(void)
{
	char *fn;
	int res;

	fn = commitids_filename();

	res = isfile(fn) && isreadable(fn);

	free(fn);
	return res;
}

FILE *
commitid_logfile(void)
{
	char *fn;
	FILE *fp;
	struct stat st;

	if (!commitids_logging())
		return NULL;

	fn = commitids_filename();

	if ((fp = fopen(fn, "r")) < 0)
		error(1, errno, "can't read %s", fn);
	if (stat(fn, &st) < 0)
		error(1, errno, "can't stat %s", fn);

	free(fn);

	return fp;
}

CommitId *
commitid_parse(char *id)
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

	out->genesis = (out->changeset == 0);

	return out;
}

void
commitid_free(CommitId *id)
{
	if (id == NULL)
		return;

	if (id->commitid != NULL)
		free(id->commitid);
	if (id->hash != NULL)
		free(id->hash);
	if (id->files != NULL)
		dellist(&id->files);

	free(id);
}

CommitId *
commitid_find(char *findid)
{
	FILE *fp;
	CommitId *tmpid = NULL, *retid = NULL, *previd = NULL;
	char *line = NULL, *tab = NULL, *files, *ep;
	const char *errstr;
	ssize_t len;
	size_t ps = 0;
	long long findcs = -1;
	int res = 0, genesis = 0, isint = 0, x;

	fp = commitid_logfile();

	if (findid != NULL && !strlen(findid))
		findid = NULL;

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
	 * TODO: if we're asking for latest (findid == NULL), seek to the end
	 * of the file and walk backwards
	 */

	while ((len = getline(&line, &ps, fp)) != -1) {
		if (line[len - 1] == '\n') {
			line[len - 1] = '\0';
			len--;
		}

		if ((tab = strstr(line, "\t")) == NULL) {
			if (genesis)
				error(1, 0, "non-genesis commit with no "
				    "files: %s", line);
			else
				genesis = 1;

			files = xstrdup("");
		} else {
			files = malloc(strlen(tab) + 1);
			strlcpy(files, tab + 1, strlen(tab) + 1);

			tab[0] = '\0';
			len = strlen(line);
		}

		if ((tmpid = commitid_parse(line)) == NULL)
			error(1, 0, "failed parsing commandid line %s", line);

		if (findcs >= 0) {
			/* match on changeset id */
			if (tmpid->changeset == findcs)
				res = 1;
		} else if (findid == NULL)
			/* keep matching to find latest commitid */
			res = 1;
		else if (strncmp(findid, tmpid->commitid, strlen(findid)) == 0) {
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
			else
				retid->previous = NULL;

			retid->files = getlist();

			while ((file = strsep(&files, "\t")) != NULL) {
				Node *f;

				if (*file == '\0')
					break;

				f = getnode();
				f->key = xstrdup(file);
				f->data = NULL;
				addnode(retid->files, f);
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

	return retid;
}

#if 0
void
commitid_generate(uint8_t *hash)
{
	char fmt[7];
	int changeset = 0;

	if (global_session_id == NULL)
		global_session_id = xmalloc(COMMITID_LENGTH + 1);

	/* commitid version */
	sprintf(global_session_id, "1");

	if (hash == NULL) {
		char *r = xmalloc(100);
		SHA2_CTX ctx;
		int i;

		/* not tracking changesets/commitids, generate random one */
		changeset = 0;

		arc4random_buf(r, 100);

		hash = xmalloc(SHA256_DIGEST_LENGTH);
		SHA256Init(&ctx);
		SHA256Update(&ctx, r, 100);
		SHA256Final(hash, &ctx);

		free(r);

		/* digest of random data */
		for (i = 0; i < SHA256_DIGEST_LENGTH; i++)
			snprintf(global_session_id, (1 + (i * 2) + 2 + 1),
			    "%s%02x", global_session_id, hash[i]);

		free(hash);
	} else
		snprintf(global_session_id, COMMITID_LENGTH + 1, "%s%s",
		    global_session_id, (char *)hash);

	/* changeset number */
	snprintf(fmt, sizeof(fmt), "%%s%%0%dd", COMMITID_CHANGESET_LENGTH);
	snprintf(global_session_id, COMMITID_LENGTH + 1, fmt,
	    global_session_id, changeset);

	assert(strlen(global_session_id) == COMMITID_LENGTH);
}
#endif

CommitId *
commitid_gen_start(unsigned long changeset)
{
	CommitId *out;

	out = xmalloc(sizeof(CommitId));
	out->version = COMMITID_VERSION;
	out->hash = xmalloc((SHA256_DIGEST_LENGTH * 2) + 1);
	out->changeset = changeset;
	out->commitid = xmalloc(COMMITID_LENGTH + 1);

	SHA256Init(&out->sha_ctx);

	return out;
}

int
commitid_gen_add_file(CommitId *id, char *filename)
{
	FILE *fp;
	size_t line_len = 8192;
	char *line = xmalloc(line_len);
	int nread;

	/* XXX: this needs to do a diff of the new file, not the raw file */

	fp = open_file(filename, "r");

	while ((nread = fread(line, 1, line_len, fp)) > 0)
		SHA256Update(&id->sha_ctx, line, nread);

	if (ferror(fp))
	    error(1, errno, "cannot read %s", filename);

	fclose(fp);

	return 1;
}

int
commitid_gen_add_buf(CommitId *id, uint8_t *buf, size_t len)
{
	SHA256Update(&id->sha_ctx, buf, len);

	return 1;
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
