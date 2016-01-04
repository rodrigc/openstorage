// gcc layer.c hash.c -DEXPERIMENTAL_ -DFILE_OFFSET_BITS=64 -lfuse -lulockmgr -lpthread -c

#ifndef EXPERIMENTAL_
#define EXPERIMENTAL_
#endif

#ifdef EXPERIMENTAL_

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/time.h>
#include <sys/types.h>

#include "hash.h"
#include "layer.h"

// A reference to the root inode.
static struct inode *root_inode;

// All layers in the system.
static struct layer *layer_head = NULL;
static hashtable_t *layer_hash;
static pthread_mutex_t layer_lock;

// Guards against a deleted inode getting free'd if someone
// is still referencing it.
static pthread_rwlock_t inode_reaper_lock;

// Allocate an inode, add it to the layer and link it to the namespace.
// Initial reference is 1.
struct inode *alloc_inode(struct inode *parent, char *name,
	mode_t mode, struct layer *layer)
{
	struct inode *inode;
	char *dupname;
	char *base;
	int ret = 0;

	inode = (struct inode *)malloc(sizeof(struct inode));
	if (!inode) {
		ret = -1;
		goto done;
	}

	dupname = strdup(name);
	if (!dupname) {
		ret = -1;
		goto done;
	}

	inode->ref = 1;

	inode->deleted = false;

	base = basename(name);
	inode->name = strdup(base);
	if (!inode->name) {
		ret = -1;
		goto done;
	}

	inode->atime = inode->mtime = inode->ctime = time(NULL);
	inode->uid = getuid();
	inode->gid = getgid();
	inode->mode = mode;

	if (mode & S_IFREG) {
		// XXX this needs to point to a block device.
		inode->f = tmpfile();
		if (!inode->f) {
			ret = -1;
			goto done;
		}
	}

	pthread_mutex_init(&inode->lock, NULL);

	inode->layer = layer;

	if (parent) {
		inode->parent = parent;

		pthread_mutex_lock(&parent->lock);
		{
			inode->next = parent->child;
			parent->child = inode;
		}
		pthread_mutex_unlock(&parent->lock);
	}

	inode->child = NULL;
	inode->next = NULL;

	if (layer) {
		ht_set(layer->children, name, inode);
	}

done:
	if (dupname) {
		free(dupname);
	}

	if (ret) {
		if (inode) {
			if (inode->f) {
				fclose(inode->f);
			}

			if (inode->name) {
				free(inode->name);
			}

			free(inode);
			inode = NULL;
		}
	}

	return inode;
}

// Get's the owner layer given a path.
static struct layer *get_layer(const char *path, char **new_path)
{
	struct layer *layer = NULL;
	char *p, *layer_id = NULL;
	int i, id;

	*new_path = NULL;

	layer_id = strdup(path + 1);
	if (!layer_id) {
		fprintf(stderr, "Warning, cannot allocate memory.\n");
		goto done;
	}

	p = strchr(layer_id, '/');
	if (p) *p = 0;

	*new_path = strchr(path+1, '/');
	if (!*new_path) {
		// Must be a request for root.
		*new_path = "/";
	}

	layer = ht_get(layer_hash, layer_id);

done:
	if (layer_id) {
		free(layer_id);
	}

	if (!layer) {
		errno = ENOENT;
	}

	return layer;
}

// Locate an inode given a path.  If 'follow' is specified, then search
// all linked layers for the path.  Create one if 'create' flag is specified.
// Increment reference count on the returned inode.
struct inode *ref_inode(const char *path, bool follow, bool create, mode_t mode)
{
	struct layer *layer = NULL;
	struct layer *parent_layer = NULL;
	struct inode *inode = NULL;
	struct inode *parent = NULL;
	char file[PATH_MAX];
	char *fixed_path = NULL;
	char *dir;
	int i;

	if (!strcmp(path, "/")) {
		return root_inode;
	}

	pthread_rwlock_rdlock(&inode_reaper_lock);

	errno = 0;

	parent_layer = layer = get_layer(path, &fixed_path);
	if (!layer) {
		goto done;
	}

	strncpy(file, fixed_path, sizeof(file));
	dir = dirname(file);

	while (layer) {
		// See if this layer has 'path'
		inode = ht_get(layer->children, fixed_path);
		if (inode) {
			pthread_mutex_lock(&inode->lock);
			{
				inode->ref++;
			}
			pthread_mutex_unlock(&inode->lock);

			goto done;
		}

		// See if this layer contains the parent directory.  We give
		// preference to the upper layers.
		if (!parent) {
			parent = ht_get(layer->children, dir);
			parent_layer = layer;
		}

		if (!follow) {
			break;
		}

		layer = layer->parent;
	}

	// If we did not find the file and create mode was requested, construct
	// a file path in the appropriate layer.
	if (!inode && create) {
		if (!parent) {
			fprintf(stderr, "Warning, create mode requested on %s, but no layer "
					"could be found that could create this file\n", fixed_path);
		} else {
			inode = alloc_inode(parent, fixed_path, mode, parent_layer);
		}
	}

done:

	pthread_rwlock_unlock(&inode_reaper_lock);

	if (!inode) {
		if (!errno) {
			errno = ENOENT;
		}
	}

	return inode;
}

// Decrement ref count on an inode.  A deleted inode with a ref count of 0
// will be garbage collected.
void deref_inode(struct inode *inode)
{
	if (inode == root_inode) {
		return;
	}

	pthread_mutex_lock(&inode->lock);
	{
		inode->ref--;
	}
	pthread_mutex_unlock(&inode->lock);
}

// Get statbuf on an inode.
void stat_inode(struct inode *inode, struct stat *stbuf)
{
	if (inode->f) {
		fstat(fileno(inode->f), stbuf);
	} else {
		stbuf->st_mode = inode->mode;
		stbuf->st_nlink = inode->nlink;
		stbuf->st_uid = inode->uid;
		stbuf->st_gid = inode->gid;
		stbuf->st_size = inode->size;
		stbuf->st_atime = inode->atime;
		stbuf->st_mtime = inode->mtime;
		stbuf->st_ctime = inode->ctime;
		stbuf->st_ino = 0;
	}
}

// Must be called with reference held.
void delete_inode(struct inode *inode)
{
	if (inode == root_inode) {
		fprintf(stderr, "Warning, trying to delete root inode.\n");
		return;
	}

	// This will be recycled when the ref counts go to 0.
	inode->deleted = true;
}

// Create a layer and link it to a parent.  Parent can be "" or NULL.
int create_layer(char *id, char *parent_id)
{
	struct layer *parent = NULL;
	struct layer *layer = NULL;
	char *str = NULL;
	int ret = 0;

	if (!layer_hash) {
		errno = EINVAL;
		ret = -errno;
		goto done;
	}

	layer = ht_get(layer_hash, id);
	if (layer) {
		layer = NULL;
		errno = EEXIST;
		ret = -errno;
		goto done;
	}

	if (parent_id && parent_id != "") {
		parent = ht_get(layer_hash, parent_id);
		if (!parent) {
			fprintf(stderr, "Warning, cannot find parent layer %s.\n", parent_id);
			errno = ENOENT;
			ret = -errno;
			goto done;
		}
	}

	layer = calloc(1, sizeof(struct layer));
	if (!layer) {
		ret = -errno;
		goto done;
	}

	strncpy(layer->id, id, sizeof(layer->id));

	layer->children = ht_create(65536, id);

	// Layer namespace creation.
	layer->root = alloc_inode(NULL, "/", 0777 | S_IFDIR, layer);
	if (layer->root == NULL) {
		ret = -errno;
		goto done;
	}

	deref_inode(layer->root);

	// Layer linkages.
	layer->upper = false;
	layer->parent = parent;
	pthread_mutex_lock(&layer_lock);
	{
		ht_set(layer_hash, id, layer);
		layer->next = layer_head;
		if (layer_head) {
			layer_head->prev = layer;
		}
		layer_head = layer;
	}
	pthread_mutex_unlock(&layer_lock);

done:
	if (str) {
		free(str);
	}

	if (ret) {
		if (layer) {
			free(layer);
		}
	}

	return ret;
}

static int remove_inodes(struct layer *layer)
{
	// TODO First mark all inodes for deletion.

	// TODO Lazily remove all inodes based on refcounts.

	return 0;
}

int remove_layer(char *id)
{
	int ret = 0;

	pthread_mutex_lock(&layer_lock);
	{
		struct layer *layer = ht_get(layer_hash, id);

		if (layer) {
			ht_remove(layer_hash, id);
			if (layer->next) {
				layer->next->prev = layer->prev;
			}

			if (layer->prev) {
				layer->prev->next = layer->next;
			}

			if (layer == layer_head) {
				layer_head = layer->next;
			}

			if (remove_inodes(layer)) {
				errno = EBUSY;
				ret = -1;
			}
		} else {
			errno = ENOENT;
			ret = -1;
		}
	}
	pthread_mutex_unlock(&layer_lock);

	return ret;
}

// Returns true if layer exists.
int check_layer(char *id)
{
	bool ret = false;

	pthread_mutex_lock(&layer_lock);
	{
		struct layer *layer = ht_get(layer_hash, id);
		if (layer) {
			ret = true;
		}
	}
	pthread_mutex_unlock(&layer_lock);

	return ret;
}

// Fill buf with the dir entries of the root FS.
int root_fill(fuse_fill_dir_t filler, char *buf)
{
	int ret = 0;

	pthread_mutex_lock(&layer_lock);
	{
		struct layer *layer = layer_head;

		while (layer) {
			struct stat st;
			char d_name[8];

			snprintf(d_name, sizeof(d_name), "%s", layer->id);

			st.st_mode = layer->root->mode;
			st.st_nlink = layer->root->nlink;
			st.st_uid = layer->root->uid;
			st.st_gid = layer->root->gid;
			st.st_size = layer->root->size;
			st.st_atime = layer->root->atime;
			st.st_mtime = layer->root->mtime;
			st.st_ctime = layer->root->ctime;
			st.st_ino = 0;

			if (filler(buf, d_name, &st, 0)) {
				errno = ENOMEM;
				ret = -1;

				fprintf(stderr, "Warning, Filler too full on root.\n");
				break;
			}

			layer = layer->next;
		}
	}
	pthread_mutex_unlock(&layer_lock);

	return ret;
}

// Mark a layer as the top most layer.
int set_upper(char *id)
{
	struct layer *layer = NULL;

	layer = ht_get(layer_hash, id);
	if (!layer) {
		errno = ENOENT;
		return -1;
	}

	layer->upper = true;

	errno = 0;
	return 0;
}

// Unmark a layer as the top most layer.
int unset_upper(char *id)
{
	struct layer *layer = NULL;

	layer = ht_get(layer_hash, id);
	if (!layer) {
		errno = ENOENT;
		return -1;
	}

	layer->upper = false;

	errno = 0;
	return 0;
}

int init_layers()
{
	layer_hash = ht_create(65536, "layers");
	if (!layer_hash) {
		return -1;
	}

	root_inode = alloc_inode(NULL, "/", 0777 | S_IFDIR, NULL);

	pthread_rwlock_init(&inode_reaper_lock, 0);
	pthread_mutex_init(&layer_lock, 0);

	return 0;
}

#endif // EXPERIMENTAL_
