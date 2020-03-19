/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_IOASID_H
#define __LINUX_IOASID_H

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/notifier.h>

#define INVALID_IOASID ((ioasid_t)-1)
#define INVALID_IOASID_SET (-1)
typedef unsigned int ioasid_t;
typedef ioasid_t (*ioasid_alloc_fn_t)(ioasid_t min, ioasid_t max, void *data);
typedef void (*ioasid_free_fn_t)(ioasid_t ioasid, void *data);

struct ioasid_set {
	int dummy;
};

/**
 * struct ioasid_allocator_ops - IOASID allocator helper functions and data
 *
 * @alloc:	helper function to allocate IOASID
 * @free:	helper function to free IOASID
 * @list:	for tracking ops that share helper functions but not data
 * @pdata:	data belong to the allocator, provided when calling alloc()
 */
struct ioasid_allocator_ops {
	ioasid_alloc_fn_t alloc;
	ioasid_free_fn_t free;
	struct list_head list;
	void *pdata;
};

/* Notification data when IOASID status changed */
enum ioasid_notify_val {
	IOASID_ALLOC = 1,
	IOASID_FREE,
	IOASID_BIND,
	IOASID_UNBIND,
};

/**
 * struct ioasid_nb_args - Argument provided by IOASID core when notifier
 * is called.
 * @id:		the IOASID being notified
 * @sid:	the IOASID set @id belongs to
 * @pdata:	the private data attached to the IOASID
 */
struct ioasid_nb_args {
	ioasid_t id;
	int sid;
	struct ioasid_set *set_token;
	void *pdata;
};
/* Shared IOASID set for reserved for host system use */
extern int system_ioasid_sid;

#define DECLARE_IOASID_SET(name) struct ioasid_set name = { 0 }

#if IS_ENABLED(CONFIG_IOASID)
ioasid_t ioasid_alloc(int sid, ioasid_t min, ioasid_t max,
		      void *private);
void ioasid_free(ioasid_t ioasid);
void *ioasid_find(int sid, ioasid_t ioasid, bool (*getter)(void *));
int ioasid_register_allocator(struct ioasid_allocator_ops *allocator);
void ioasid_unregister_allocator(struct ioasid_allocator_ops *allocator);
int ioasid_attach_data(ioasid_t ioasid, void *data);
int ioasid_add_notifier(struct notifier_block *nb);
void ioasid_remove_notifier(struct notifier_block *nb);
void ioasid_install_capacity(ioasid_t total);
int ioasid_alloc_set(struct ioasid_set *token, ioasid_t quota, int *sid);
void ioasid_free_set(int sid, bool destroy_set);
int ioasid_find_sid(ioasid_t ioasid);
int ioasid_notify(ioasid_t id, enum ioasid_notify_val cmd);

#else /* !CONFIG_IOASID */
static inline ioasid_t ioasid_alloc(int sid, ioasid_t min,
				    ioasid_t max, void *private)
{
	return INVALID_IOASID;
}

static inline void ioasid_free(ioasid_t ioasid)
{
}

int ioasid_alloc_set(struct ioasid_set *token, ioasid_t quota, int *sid)
{
	return -ENOTSUPP;
}

static inline void ioasid_free_set(int sid, bool destroy_set)
{
}

static inline void *ioasid_find(int sid, ioasid_t ioasid, bool (*getter)(void *))
{
	return NULL;
}

static inline int ioasid_add_notifier(struct notifier_block *nb)
{
	return -ENOTSUPP;
}

static inline void ioasid_remove_notifier(struct notifier_block *nb)
{
}

int ioasid_notify(ioasid_t ioasid, enum ioasid_notify_val cmd)
{
	return -ENOTSUPP;
}

static inline int ioasid_register_allocator(struct ioasid_allocator_ops *allocator)
{
	return -ENOTSUPP;
}

static inline void ioasid_unregister_allocator(struct ioasid_allocator_ops *allocator)
{
}

static inline int ioasid_attach_data(ioasid_t ioasid, void *data)
{
	return -ENOTSUPP;
}

static inline void ioasid_install_capacity(ioasid_t total)
{
}
#endif /* CONFIG_IOASID */
#endif /* __LINUX_IOASID_H */
