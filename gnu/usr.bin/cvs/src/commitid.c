#include "cvs.h"

#include <sys/types.h>
#include <sha2.h>
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
	char hash[SHA256_DIGEST_LENGTH * 2];
	unsigned long changeset = 0;
	char fmt[13];
	int res;

	if (strlen(id) != COMMITID_LENGTH)
		return NULL;

	/* %1u%64s%07lu */
	snprintf(fmt, 13, "%%1u%%%ds%%0%dlu",
	    (SHA256_DIGEST_LENGTH * 2), COMMITID_CHANGESET_LENGTH);

	res = sscanf(id, fmt, &version, &hash, &changeset);
	if (res != 3) {
		error(0, 0, "malformed commitid %s", id);
		return NULL;
	}

	if (version != COMMITID_VERSION)
		return NULL;

	out = xmalloc(sizeof(CommitId));
	out->commitid = xstrdup(id);
	out->version = version;
	out->hash = xstrdup(hash);
	out->changeset = changeset;

	return out;
}

void
commitid_free(CommitId *id)
{
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
	CommitId *tmpid, *retid = NULL, *previd = NULL;
	char *line = NULL, *tab = NULL, *files;
	ssize_t len;
	size_t ps = 0;
	int res = 0;

	fp = commitid_logfile();

	if (findid != NULL && !strlen(findid))
		findid = NULL;

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

		if ((tmpid = commitid_parse(line)) == NULL)
			error(1, 0, "failed parsing commandid line %s", line);

		if (findid == NULL)
			/* want latest commitid */
			res = 1;
		else {
			/*
			 * need to go hunting - match on first part of commitid
			 * chars, allowing for shortened unless it matches more
			 * than one
			 */
			if (strncmp(findid, tmpid->commitid,
			    strlen(findid)) == 0) {
				if (res) {
					res = 0;
					if (retid)
						commitid_free(retid);
					error(0, 0, "commitid \"%s\" is "
					    "ambiguous", findid);
					break;
				}

				res = 1;

				if (strlen(findid) == COMMITID_LENGTH)
					/* no possible duplicates */
					break;
			}
		}

		if (res && tmpid) {
			char *file;

			if (retid)
				commitid_free(retid);
			retid = tmpid;

			if (previd)
				retid->parent = xstrdup(previd->commitid);
			else
				retid->parent = NULL;

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
		}
		else if (!res)
			previd = tmpid;
	}
	fclose(fp);

	if (previd)
		commitid_free(previd);

	return retid;
}

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
