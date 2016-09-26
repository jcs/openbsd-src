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

#include <sha2.h>

#define COMMITID_VERSION		1
#define COMMITID_VERSION_LENGTH		2
#define COMMITID_HASH_LENGTH		(SHA512_256_DIGEST_LENGTH * 2)
#define COMMITID_CHANGESET_LENGTH	7
#define COMMITID_LENGTH			(COMMITID_VERSION_LENGTH + 1 + \
					    COMMITID_HASH_LENGTH + 1 + \
					    COMMITID_CHANGESET_LENGTH)

struct commitid {
	char *repo;
	char *previous;

	char *commitid;
	int version;
	char *hash;
	unsigned long changeset;

	/* hash of files changed, key=filename, val=r1:r2 */
	List *files;

	int genesis;

	SHA2_CTX sha_ctx;
};
typedef struct commitid CommitId;

struct commitid_file {
	char *filename;
	char *rcsfile;
	char *prev_revision;
	char *revision;
	char *branch;
};
typedef struct commitid_file CommitIdFile;
