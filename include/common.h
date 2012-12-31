#ifndef PKS_MOD_H
#define PKS_MOD_H

#ifndef MOD_LICENSE
#define MOD_LICENSE	"GPL"
#endif

#ifndef MOD_AUTHOR
#define MOD_AUTHOR	"Pranay Kumar Srivastava"
#endif

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/semaphore.h>
#include <linux/wait.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/stat.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/list.h>

MODULE_AUTHOR(MOD_AUTHOR);
MODULE_LICENSE(MOD_LICENSE);

#endif /*PKS_MOD_H*/
