#include <sha2.h>

#define COMMITID_VERSION		1
#define COMMITID_VERSION_LENGTH		4
#define COMMITID_HASH_LENGTH		(SHA256_DIGEST_LENGTH * 2)
#define COMMITID_CHANGESET_LENGTH	7
#define COMMITID_LENGTH			(COMMITID_VERSION_LENGTH + \
					    COMMITID_HASH_LENGTH + \
					    COMMITID_CHANGESET_LENGTH)

struct commitid {
	char *commitid;
	int version;
	char *hash;
	unsigned long changeset;
	List *files;

	char *previous;

	SHA2_CTX sha_ctx;
};

typedef struct commitid CommitId;
