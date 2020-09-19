#ifndef XENSTORE_H_
#define XENSTORE_H_

int xenstore_write(const char *key, char *value);
int xenstore_read(const char *key, char *value, int value_length);
int xenstore_ls(const char *key, char *values, int value_length);

#define IDS_COUNT 128
#define IDS_LEN (32 * IDS_COUNT)

#define xenstore_foreach(path, name) \
	char __xs_fe_ids[IDS_LEN]; \
	\
	int __xs_fe_len = xenstore_ls(path, __xs_fe_ids, IDS_LEN); \
	\
	if (__xs_fe_len < 0 || strcmp(__xs_fe_ids, "ENOENT") == 0) { \
		__xs_fe_ids[0] = 0; \
	} \
	\
	for (char* name = __xs_fe_ids; *name != 0; name += strlen(name) + 1)

#define XS_MSG_LEN 256

#define xenstore_scanf(path, key, fmt, ...) \
	({ \
		char xs_key[XS_MSG_LEN]; \
		char xs_value[XS_MSG_LEN]; \
		int err; \
		\
		sprintf(xs_key, "%s/%s", path, key); \
		memset(xs_value, 0, sizeof xs_value); \
		err = xenstore_read(xs_key, xs_value, sizeof xs_value); \
		if (err >= 0) { \
			err = sscanf(xs_value, fmt, __VA_ARGS__); \
		} \
		err; \
	})

#define xenstore_printf(path, key, fmt, ...) \
	({ \
		char xs_key[XS_MSG_LEN]; \
		char xs_value[XS_MSG_LEN]; \
		\
		sprintf(xs_key, "%s/%s", path, key); \
		sprintf(xs_value, fmt, __VA_ARGS__); \
		xenstore_write(xs_key, xs_value); \
	})

#endif /* XENSTORE_H_ */
