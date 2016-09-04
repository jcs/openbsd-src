#include "cvs.h"

#include <sys/types.h>
#include <sha2.h>
#include <assert.h>

void
commitids_filename(char **fn)
{
	char *repo = Short_Repository(Name_Repository(NULL, NULL));

	*fn = xmalloc(strlen(current_parsed_root->directory) +
	    sizeof(CVSROOTADM) + sizeof(CVSROOTADM_COMMITIDS) + 1 +
	    strlen(repo) + 1);
	sprintf(*fn, "%s/%s/%s-%s", current_parsed_root->directory, CVSROOTADM,
	    CVSROOTADM_COMMITIDS, repo);
}

int
commitids_logging(void)
{
	char *fn;
	int res;

	commitids_filename(&fn);

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
	ssize_t len;
	size_t ps = 0;
	int res = 0;

	if (!commitids_logging())
		return NULL;

	commitids_filename(&fn);

	if ((fp = fopen(fn, "r")) < 0)
		error(1, errno, "can't read %s", fn);
	if (stat(fn, &st) < 0)
		error(1, errno, "can't stat %s", fn);

	free(fn);

	return fp;
}

int
commitid_parse(char *id, CommitId *out)
{
	int res;

	unsigned int version;
	char hash[SHA256_DIGEST_LENGTH];
	unsigned long changeset = 0;
	char fmt[14];

	if (strlen(id) != COMMITID_LENGTH)
		return 0;

	/* %1u%64s%07lld */
	res = snprintf(fmt, sizeof(fmt), "%%1u%%%ds%%0%dlld",
	    (SHA256_DIGEST_LENGTH * 2), COMMITID_CHANGESET_LENGTH);
	printf("res of snprintf is %d\n", res);
	printf("%s (%d)\n", fmt, strlen(fmt));

	res = sscanf(id, fmt, &version, &hash, &changeset);
	if (res != 3) {
		error(0, 0, "malformed commitid %s (%s) %d %s %lu (%d)", id, fmt, version,
		hash, changeset, res);
		return 0;
	}

	if (version != COMMITID_VERSION)
		return 0;

	out = xmalloc(sizeof(CommitId));
	out->version = version;
	out->hash = xstrdup(hash);
	out->changeset = changeset;

	return 1;
}

int
commitid_find(char *findid, CommitId *fullid, CommitId *parentid)
{
	FILE *fp;
	List *lfiles;
	CommitId *tmpid = NULL, *previd = NULL;
	char *line = NULL, *tab = NULL, *files = NULL;
	ssize_t len;
	size_t ps = 0;
	int res = 0;

	fp = commitid_logfile();

	if (findid != NULL && !strlen(findid))
		findid = NULL;

	while ((len = getline(&line, &ps, fp)) != -1) {
		if (line[len - 1] == '\n') {
			line[len - 1] = '\0';
			--len;
		}

		if ((tab = strstr(line, "\t")) == NULL)
			continue;

		files = malloc(strlen(tab) + 1);
		strlcpy(files, tab + 1, strlen(tab) + 1);

		tab[0] = '\0';
		len = strlen(line);

		if (!commitid_parse(line, tmpid))
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
					fullid = NULL;
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
			fullid = tmpid;
			if (previd)
				parentid = previd;
		}
		else if (!res)
			previd = tmpid;
	}

	fclose(fp);

#if 0
	if (fullid) {
		char **file;

		while ((file = strsep(&files, "\t")) != NULL) {
			Node *f;

			if (*file == '\0')
				break;

			f = getnode();
			f->key = xstrdup(file);
			f->data = NULL;
			addnode(*lfiles, f);
		}
	}
#endif

	return res;
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
