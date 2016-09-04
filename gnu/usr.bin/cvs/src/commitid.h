#define COMMITID_VERSION		1
#define COMMITID_CHANGESET_LENGTH	7
#define COMMITID_LENGTH			(1 + (SHA256_DIGEST_LENGTH * 2) + \
					    COMMITID_CHANGESET_LENGTH)

struct commitid {
	char *commitid;
	int version;
	char *hash;
	uint32_t changeset;
	List *files;
};

typedef struct commitid CommitId;
