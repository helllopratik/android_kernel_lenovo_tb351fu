#ifndef _LINUX_WAKELOCK_H
#define _LINUX_WAKELOCK_H
#include <linux/pm_wakeup.h>
struct wake_lock { int dummy; };
enum { WAKE_LOCK_SUSPEND };
static inline void wake_lock_init(void *l, int t, const char *n) {}
static inline void wake_lock(void *l) {}
static inline void wake_unlock(void *l) {}
static inline void wake_lock_destroy(void *l) {}
#endif
