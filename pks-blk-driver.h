#include "../include/common.h"
#include <linux/blkdev.h>
#include <linux/interrupt.h>
#include <linux/hdreg.h>
#include <linux/genhd.h>
#define KERNEL_SECTOR_SIZE			(1<<9)
#define __PROTOTYPE__(Function,args) 		Function args
#define PUBLIC					extern
#define PRIVATE					static
#define PKS_MAX_REQUESTS			256


/*
 *Some playing parameters with disk. God knows what they are used for..! Maybe you do as well.. but you aint
 *no God... :P
 */
#define PKS_DISK_NR_HEADS			8
#define PKS_DISK_NR_CYLINDERS			4
#define PKS_DISK_START_DATA_SEC			10

/*
 * This disk isn't a real one, but just a bunch of pages tied together for the module.
 * The disk size can be changed here only while compiling, like you can't turn around your luck with real HDD,
 * wishing a larger space won't make it happen. So don't be a loser and work with what you've :P.
 * */
#define PKS_DISK_SIZE_IN_PAGES		131072/*256*/


/*
 * I seriously don't wanna break off with 512 byte sectors tradition... But WTH... we are gonna try changin 
 * it too just once it all starts workin... :D.
 * */
#define PKS_DISK_SECTOR_SIZE		(512*4)
#define pks_nr_disk_sectors(pks_disk)\
	(pks_disk->pg_count*(PAGE_SIZE/PKS_DISK_SECTOR_SIZE))
#define PKS_NR_SECTORS_PER_PAGE		(PAGE_SIZE/PKS_DISK_SECTOR_SIZE)
#define PKS_DISK_MIN_SECTOR_XFER	(PKS_DISK_SECTOR_SIZE/KERNEL_SECTOR_SIZE)

/*
 * Alright the moment you've been waiting for... what the disk looks like. If you don't get what's
 * written below...Huh, get a screwdriver!
 * */
struct pks_disk
{
	struct gendisk 		*pks_disk_gd;
	struct request_queue 	*pks_disk_rqq;
	atomic_t		use_count;
	struct page		*device_buffer[PKS_DISK_SIZE_IN_PAGES];	
	fmode_t			mode;
	int			pg_count; /*How many actually pages are there!*/
	spinlock_t		queue_lock;
};

/*
 * We probably would need to have a way to identify which page exactly are we need to work with.
 * So let's put some trivial stuff here.
 * */
static inline int get_page_index(sector_t sector)
{
	return ((sector))/PKS_NR_SECTORS_PER_PAGE; /*Get's the index. Sector numbers start from 1, tracks from 0*/
}

static inline int get_page_offset(sector_t sector)
{
	//return ((sector)/(PKS_DISK_SECTOR_SIZE/KERNEL_SECTOR_SIZE))%PAGE_SIZE;
	return sector%PKS_NR_SECTORS_PER_PAGE;
}

PUBLIC struct block_device_operations pks_block_device_operations; 
PUBLIC struct pks_disk *pks_disk;

PUBLIC __PROTOTYPE__(	int open	,		(struct block_device*	,fmode_t));
PUBLIC __PROTOTYPE__(	int release	,		(struct gendisk*	,fmode_t));
PUBLIC __PROTOTYPE__(	int getgeo	,		(struct block_device*	,struct hd_geometry*));
PUBLIC __PROTOTYPE__(	unsigned long long setcapacity,	(struct gendisk*,unsigned long long));

