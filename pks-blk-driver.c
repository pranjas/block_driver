#include "pks-blk-driver.h"

#define PKS_DBG_FUNC printk(KERN_INFO "%s at line %d\n",__FUNCTION__,__LINE__)

#define PKS_DBG_LOG(fmt,args...) printk(fmt,##args)

#define PKS_DBG_VAR(fmt,X)	#X fmt,X

struct pks_disk 		*pks_disk=NULL;
struct task_struct 		*pks_tasklet_kthread=NULL;
static wait_queue_head_t 	pks_tasklet_kthread_waitq_head;

LIST_HEAD(pks_tasklet_completed_list);
static unsigned char rq_handlers_bmap[PKS_MAX_REQUESTS/(sizeof(char)*8)]={0};
struct rq_handler_struct
{
	struct tasklet_struct 	rq_handler;
	struct list_head	tasklet_list;
	struct request 		*rq;
	int 			rq_handler_idx;
};
static struct rq_handler_struct rq_handlers[PKS_MAX_REQUESTS]={(0)};

struct block_device_operations pks_block_device_operations=
{
	.open=open,
	.release=release,
	.getgeo=getgeo,
	.set_capacity=setcapacity,
};


static int pks_kthread_function(void *arg)
{
	unsigned long flags;
	struct list_head *cur,*next; /*For traversing with safer version*/

do_tasklet_pending_completion_check:
	while(!kthread_should_stop())
	{
	spin_lock_irqsave(&pks_disk->queue_lock,flags);
		if(list_empty(&pks_tasklet_completed_list))
		{
			spin_unlock_irqrestore(&pks_disk->queue_lock,flags);
			wait_event_interruptible(pks_tasklet_kthread_waitq_head,(!list_empty(&pks_tasklet_completed_list) || kthread_should_stop()));
			goto do_tasklet_pending_completion_check; /*Didn't wanted to write locking statement again...*/
		}
		while(!list_empty(&pks_tasklet_completed_list))
		{	
			list_for_each_safe(cur,next,&pks_tasklet_completed_list)
			{
				struct rq_handler_struct *rq_handler_struct=container_of(cur,struct rq_handler_struct,tasklet_list);
				/*Iff(yes 2fs) this tasklet has been run. And we never re-schedule our tasklet from within.*/
				if(!test_bit(TASKLET_STATE_RUN,&rq_handler_struct->rq_handler.state)) /*State must be zero*/
				{
					list_del_init(cur);
					/*Mark this tasklet as free*/
					rq_handlers_bmap[rq_handler_struct->rq_handler_idx/8]&=~(1<<(rq_handler_struct->rq_handler_idx)%8);
					/*Let other requests come in*/
					if(blk_queue_stopped(pks_disk->pks_disk_rqq))
						blk_start_queue(pks_disk->pks_disk_rqq);
				}
				/*Otherwise we continue within the list loop until all such tasklets have been kicked out*/
			}
		}
	spin_unlock_irqrestore(&pks_disk->queue_lock,flags);
	}
return 0;
}

static int pks_start_kthread_for_tasklets(void)
{
	pks_tasklet_kthread=kthread_create(pks_kthread_function,NULL,"pks-disk-d");
	if(!pks_tasklet_kthread)
		return -ENOMEM;
	wake_up_process(pks_tasklet_kthread);
return 0; /*YAY... */
}


/*
 * This should be straight forward. This function is supposed to be called only
 * once while actually creating the disk. This has no meaning if the disk has
 * already been allocated.. GFP_* flags as usual, preferably GFP_KERNEL only should
 * be fine since disk allocation is done initially perhaps as a result of a user space
 * open or at module load time.... duh..
 */

static int pks_alloc_disk_pages(struct pks_disk *pks_disk,gfp_t flags)
{
	int pgs_alloced=0;
	PKS_DBG_FUNC;
	if(!pks_disk) return -1; /*Error condition*/
	while(pgs_alloced!=PKS_DISK_SIZE_IN_PAGES)
	{
		pks_disk->device_buffer[pgs_alloced]=alloc_page(flags);
		if(!pks_disk->device_buffer[pgs_alloced])
		{
			return pgs_alloced; /*Number of pgs that have been given so far*/
		}
		pks_disk->pg_count=++pgs_alloced;
	}
	PKS_DBG_LOG(KERN_INFO "Total pages allocated=%d\n",pgs_alloced);
	PKS_DBG_FUNC;
return pgs_alloced;/*Finally if we are really through return PKS_DISK_SIZE_IN_PAGES*/
}

static void pks_free_disk_pages(struct pks_disk *pks_disk)
{
	PKS_DBG_FUNC;
	while(pks_disk->pg_count)
	{
		__free_page(pks_disk->device_buffer[--pks_disk->pg_count]);
		pks_disk->device_buffer[pks_disk->pg_count]=NULL;
	}
	PKS_DBG_FUNC;
}

int open(struct block_device *blk_dev,fmode_t mode)
{
	struct pks_disk *pks_disk=(struct pks_disk*)blk_dev->bd_disk->private_data;
	PKS_DBG_FUNC;
	if(!pks_disk)
	{
		return -ENODEV;
	}
	pks_disk->mode=mode;
	atomic_inc(&pks_disk->use_count);
	PKS_DBG_FUNC;
	return 0;
}

/*Device release is sort of device closed... But we don't free anything here until someone
 *unloads the damn driver... :P
 */
int release(struct gendisk* disk,fmode_t mode)
{
	struct pks_disk *pks_disk=(struct pks_disk*)(disk->private_data);
	PKS_DBG_FUNC;
	if(!pks_disk)
		return -ENODEV;
	atomic_dec(&pks_disk->use_count);
	if(!atomic_read(&pks_disk->use_count))
	{
		pks_free_disk_pages(pks_disk);
	}
	PKS_DBG_FUNC;
	return 0;
}

int getgeo(struct block_device* blk_dev,struct hd_geometry *hd_geo)
{
	struct pks_disk *pks_disk=(struct pks_disk*)blk_dev->bd_disk->private_data;
	PKS_DBG_FUNC;
	if(!pks_disk) return -ENODEV;

	hd_geo->heads=PKS_DISK_NR_HEADS;
	hd_geo->sectors=pks_nr_disk_sectors(pks_disk);
	hd_geo->cylinders=PKS_DISK_NR_CYLINDERS;
	hd_geo->start=PKS_DISK_START_DATA_SEC; /*Start the data from the very first sector of disk*/
	PKS_DBG_FUNC;
	return 0;
}

/*Not sure when will this be called...*/
unsigned long long setcapacity(struct gendisk * gd,unsigned long long size)
{
	PKS_DBG_FUNC;
	printk(KERN_INFO "Wanna set capacity to %llu\n",size);
	PKS_DBG_FUNC;
	return size;
}

/*
 *This one is pretty simple, you get a request and handle each segment through
 *tasklet. This way we can handle more requests rather than a single request at at time.
 */
static void handle_request(unsigned long args)
{
	struct rq_handler_struct *rq_handler_struct=(struct rq_handler_struct*)args;
	struct request *rq=rq_handler_struct->rq;
	struct bio_vec *bi_vec;
	struct req_iterator iter;
	unsigned long flags;
	struct request_queue *rrq=rq->q;
	int rq_error=0;
	PKS_DBG_FUNC;
	/* IDK when will it not be a file system request!*/
	if(!blk_fs_request(rq))
	{
		rq_error=-1;
		goto handle_request_done;/*Drop if the request isn't from FS*/
	}
	PKS_DBG_LOG(KERN_INFO "request is for data %s\n",(rq_data_dir(rq)?"write":"read"));
	PKS_DBG_LOG(KERN_INFO "Total sectors for request are %d\n",blk_rq_sectors(rq));
	/* 
	 *We do this one segment, that is bio, at at time. Each bio has bio_vec list of sub-segments
	 *this is where the data is to be read/written. 
	 *Each request contains many segments so we iterate over all such segments and process em..
	 *req_iterator points to the currently under processing bio.
	 *bi_vec points to the currently under processing bio_vec.

	 *Each request must of at least 8 sectors, i.e each bio contains at least 8 sectors. This is
	 *a requirement of the block layer. blk_queue_max_sectors,max_hw_sectors both check this. So
	 *I don't think you can make the request have lesser than 8 sectors.

	 *Why 8 you must wonder... well, 512*8=4096=PAGE_SIZE, so kernel wants to read/write in pages
	 *no matter what...A sector is always measured in 512 bytes unit within kernel.
	 
	 *Each bio_vec contains a page which is offset,len pair. Now I was wondering what will a driver
	 *do if the len is not a multiple of sector size? As it turns out for file systems this is usually
	 *the blocksize which is usually PAGE_SIZE and PAGE_SIZE is always a multiple of 512 byte.

	 *Hence it turns out if there's a driver which expects to handle non PAGE_SIZE requests it must
	 *handle the copy operations in its own internal buffer. This WOULD BECOME NASTY!!! ITS UGLY...
	 *and it doesn't make sense!!. 
	 
	 *So if you are developing a file system make sure that block size can't be lower than 512 and must
	 *be a multiple of this sector size. This way everyone stays happy n healthy... :D. YES I did
	 *checked this...
	 */
	rq_for_each_segment(bi_vec,rq,iter)
	{
		sector_t cur_sec=iter.bio->bi_sector;
		PKS_DBG_LOG(KERN_INFO "\nStarting sector for bio is at %lu\n",cur_sec);
		/*Find where on the storage this has to be put*/
		int offset=get_page_offset(cur_sec/PKS_DISK_MIN_SECTOR_XFER);
		PKS_DBG_LOG(KERN_INFO "offset is %d in page\n",offset);
		PKS_DBG_LOG(KERN_INFO "Total sectors for this bio are %d\n",blk_rq_cur_sectors(rq));

		/*These many sectors to transfer in language of device. Here we need to scale down instead
		 *of scale up hence the division by the same factor.
		 */
		int sectors_toxfer=bio_sectors(iter.bio)/PKS_DISK_MIN_SECTOR_XFER;
		int pg_idx=get_page_index(cur_sec/PKS_DISK_MIN_SECTOR_XFER);
		int sector_rem_for_page	=PKS_NR_SECTORS_PER_PAGE-offset;
		unsigned char *pg_address,*buff_address;
		int i=0,bytes_to_copy;
		PKS_DBG_LOG(KERN_INFO PKS_DBG_VAR("=%d\n",sectors_toxfer));
		while(sectors_toxfer>0)
		{
			if(!sector_rem_for_page)
			{
				pg_idx=get_page_index(cur_sec/PKS_DISK_MIN_SECTOR_XFER);
				offset=get_page_offset(cur_sec/PKS_DISK_MIN_SECTOR_XFER);
				sector_rem_for_page=PKS_NR_SECTORS_PER_PAGE-offset;
			}
			/*Finally start transferring data*/
			PKS_DBG_LOG(KERN_INFO PKS_DBG_VAR("=%d\n",pg_idx));
			pg_address=(unsigned char*)page_address(pks_disk->device_buffer[pg_idx])+offset*PKS_DISK_SECTOR_SIZE;
			buff_address=(unsigned char*)page_address(bi_vec->bv_page)+bi_vec->bv_offset;
			
			PKS_DBG_LOG(KERN_INFO "device page address=%p and bio_vec address=%p\n",pg_address,buff_address);
			PKS_DBG_LOG(KERN_INFO "bio_vec len=%d and offset=%d\n",bi_vec->bv_len,bi_vec->bv_offset);
			PKS_DBG_LOG(KERN_WARNING "buffer at %p and pg_address=%p\n",buff_address+i*PKS_DISK_MIN_SECTOR_XFER*KERNEL_SECTOR_SIZE,pg_address);
			bytes_to_copy=PKS_DISK_MIN_SECTOR_XFER*KERNEL_SECTOR_SIZE<bi_vec->bv_len?PKS_DISK_MIN_SECTOR_XFER*KERNEL_SECTOR_SIZE:bi_vec->bv_len;
			PKS_DBG_LOG(KERN_INFO PKS_DBG_VAR("=%d\n",bytes_to_copy));

		/*FIXME: Need to have translation logic here. Everything else seems to work fine. Now tasklets are also working cool...*/
			//PKS_DBG_LOG(KERN_WARNING "Will copy data at buffer address %p of length %d and device address=%p\n",buff_address+i*PKS_DISK_MIN_SECTOR_XFER*KERNEL_SECTOR_SIZE,PKS_DISK_MIN_SECTOR_XFER*KERNEL_SECTOR_SIZE>bi_vec->bv_len?bi_vec->bv_len:,pg_address);
			/*Is the request for write?*/
			if(rq_data_dir(rq))
				memcpy(pg_address,buff_address+i*PKS_DISK_MIN_SECTOR_XFER*KERNEL_SECTOR_SIZE,bytes_to_copy);
			else
				memcpy(buff_address+i*PKS_DISK_MIN_SECTOR_XFER*KERNEL_SECTOR_SIZE,pg_address,bytes_to_copy);
			i++;
			sectors_toxfer--;
			cur_sec+=PKS_DISK_MIN_SECTOR_XFER;
			sector_rem_for_page--;
			offset++;
		}
	}	
handle_request_done:
	if(blk_end_request(rq,rq_error,blk_rq_bytes(rq)))
	{
		PKS_DBG_LOG(KERN_INFO "end request says request is pending!!!\n");
	}
	/*
	 * This might screw up things, but hopefully the same tasklet
	 * won't run again. We need to take locking over the request queue lock
	 * the reason being that we allocate tasklets in the request function so it's only
	 * natural to free em up while holding the queue lock. THIS IS IMPLEMENTATION DEPENDENT...
	 *
	 * FIXME:
	 */
	spin_lock_irqsave(rrq->queue_lock,flags);
		list_add(&rq_handler_struct->tasklet_list,&pks_tasklet_completed_list);
		wake_up_interruptible(&pks_tasklet_kthread_waitq_head);
		PKS_DBG_LOG(KERN_INFO "Unallocating tasklet at index %d\n",rq_handler_struct->rq_handler_idx); 
	spin_unlock_irqrestore(rrq->queue_lock,flags);

	PKS_DBG_FUNC;
}

/*
 *This code will be run whenever there's a request in line
 *Since the request function is called with the request queue lock held,
 *there's no need to take any lock here..period. 
 *What we do here is allocate one stupid tasklet from the tasklet bitmap and return its index.
 */
static int pks_alloc_rq_tasklet(void)
{
	int i=0,j=0;
	PKS_DBG_FUNC;
	for(;i<PKS_MAX_REQUESTS/(sizeof(char)*8);i++)
	{
		if( (rq_handlers_bmap[i] & 0xff)!=0xff)
		{
			for(j=0;j<8;j++){
				if(!(rq_handlers_bmap[i]& (1<<j)))
				{
					rq_handlers_bmap[i]|=(1<<j);
					break;
				}
			}
			PKS_DBG_LOG(KERN_INFO "Allocated tasklet at index=%d\n",i*8+j);
			return i*8+j;
		}
	}
	PKS_DBG_FUNC;
return -1;/*NO Tasklets are available.*/
}

/*
 *This is our request function. This function is responsible to get the request and do something with it.
 *Since we want PKS_MAX_REQUESTS to be under processing atmost we don't handle only a single request
 *at a time. Instead we just make a tasklet and let it bother with the request we dequeue.
 */
static void pks_request_func(struct request_queue *rrq)
{
	struct request *rq=blk_fetch_request(rrq);
	int pks_tasklet_index;
	PKS_DBG_FUNC;
	if(!rq)
	{
		printk("OOPS!!! no request in pipeline\n");
		return;
	}
	
	pks_tasklet_index=pks_alloc_rq_tasklet();
	/*
	 *It might be possible that we are not able to allocate any tasklet since all other tasklets
	 *are already processing requests. Therefore we need to requeue and then stop the request queue
	 *so that we are not bothered anymore until some tasklets have been freed.
	 */
	if(pks_tasklet_index<0)
	{
		blk_requeue_request(rrq,rq);
		blk_stop_queue(rrq);
		return;
	}
	rq_handlers[pks_tasklet_index].rq_handler_idx=pks_tasklet_index;
	rq_handlers[pks_tasklet_index].rq=rq;
	tasklet_init(&rq_handlers[pks_tasklet_index].rq_handler,handle_request,(unsigned long)&rq_handlers[pks_tasklet_index]);
	tasklet_schedule(&rq_handlers[pks_tasklet_index].rq_handler);
	PKS_DBG_FUNC;
}

int __init module_load(void)
{
	int major;
	pks_disk=(struct pks_disk*)kmalloc(sizeof(struct pks_disk),GFP_KERNEL);
	if(!pks_disk) return -1;

	atomic_inc(&pks_disk->use_count);
	if(pks_alloc_disk_pages(pks_disk,GFP_KERNEL)<0) return -1;

	/*Allocate the disk and initialize it*/
	pks_disk->pks_disk_gd=alloc_disk(1); /*We only need a single disk*/

	if(!pks_disk->pks_disk_gd)
	{
free_resources:
		pks_free_disk_pages(pks_disk);
		kfree(pks_disk);
		return -1;
	}
	PKS_DBG_LOG(KERN_INFO "total sectors allocated=%lu\n",pks_nr_disk_sectors(pks_disk));
	/*Get a request queue for this disk and give it the damn spinlock.
	 *This is a lot more reason to use a containing structure rather than using gendisk directly.
	 *The request queue lock is supposed to be given by us not by the block layer. It just uses
	 *it.
	 */
	spin_lock_init(&pks_disk->queue_lock);
	init_waitqueue_head(&pks_tasklet_kthread_waitq_head);

	/*See if we were able to allocate a request queue. At this point its not related to our
	 *gendisk structure yet.
	 */
	pks_disk->pks_disk_rqq=blk_init_queue(pks_request_func,&pks_disk->queue_lock);
	if(!pks_disk->pks_disk_rqq)
	{
		goto free_resources;
	}
	
	/*Give our gendisk its request queue*/
	pks_disk->pks_disk_gd->queue=pks_disk->pks_disk_rqq;

	/*Grab a device number dynamically. There's no other reason for this function!*/
	major=register_blkdev(0,"pks_blk_dev");

	PKS_DBG_LOG(KERN_INFO "registered major number=%d\n",major);
	if(major<0 || pks_start_kthread_for_tasklets()==-ENOMEM)
	{
		blk_cleanup_queue(pks_disk->pks_disk_rqq);
		if(major>0)
			unregister_blkdev(major,"pks_blk_dev");
		goto free_resources;
	}


	/*Assign some stuff here.....*/
	pks_disk->pks_disk_gd->major=major;
	pks_disk->pks_disk_gd->minors=1;
	pks_disk->pks_disk_gd->first_minor=0;

	
	/*These are requrired most!!*/
	pks_disk->pks_disk_gd->fops=&pks_block_device_operations;
	pks_disk->pks_disk_gd->private_data=pks_disk;


	/*Name of the disk must be set, there's gonna be an entry in sysfs and it would
	 *cry in your face if you miss this... :(
	 */
	snprintf(pks_disk->pks_disk_gd->disk_name,sizeof(pks_disk->pks_disk_gd->disk_name)-1,"%s","pranaydisk");
	/* We need to tell kernel how many sectors our disk has. But since kernel only understands
	 * 512 bytes unit sectors we will need to scale it here. So all we need is to multiply by a factor
	 * which makes kernel understand the same number of sectors in 512 byte units.
	 */
	PKS_DBG_LOG(KERN_INFO "setting capacity of disk as %lu\n",pks_nr_disk_sectors(pks_disk)*(PKS_DISK_SECTOR_SIZE/KERNEL_SECTOR_SIZE));
	set_capacity(pks_disk->pks_disk_gd,(pks_nr_disk_sectors(pks_disk))*(PKS_DISK_SECTOR_SIZE/KERNEL_SECTOR_SIZE));

	/* Set the sector size our disk can handle. We don't care about physical block size. Apparently it doesn't work the way I
	 * want it to... huh..
		FROM block/blk-settings.c
	 */
	blk_queue_logical_block_size(pks_disk->pks_disk_rqq,PKS_DISK_SECTOR_SIZE);
	blk_queue_physical_block_size(pks_disk->pks_disk_rqq,PKS_DISK_SECTOR_SIZE);

	
	/* Set the minimum request I/O size our device can handle. Now the function says it sets this
	 * for request thus and not the individual segments.
		FROM block/blk-settings.c
	 * This DIDN'T WORKED for me :(
	 */
	//blk_limits_io_min(pks_disk->pks_disk_rqq,PKS_DISK_SECTOR_SIZE);

	/*When you call this be prepared for the worst. Kernel will call your request function even before
	 *the function below returns. Probably to get partition information i guess...*/
	add_disk(pks_disk->pks_disk_gd);
return 0;
}

void __exit module_unload(void)
{
	unsigned long flags;
	int major=pks_disk->pks_disk_gd->major;
	/*We take no more requests!*/

	spin_lock_irqsave(&pks_disk->queue_lock,flags);
		blk_stop_queue(pks_disk->pks_disk_rqq);
	spin_unlock_irqrestore(&pks_disk->queue_lock,flags);

	/*We stop our kernel thread. Yup this waits for thread... :D*/
	kthread_stop(pks_tasklet_kthread);


/*free queue*/
	blk_cleanup_queue(pks_disk->pks_disk_rqq);
/*Remove disk*/
	del_gendisk(pks_disk->pks_disk_gd);
/*Free pks_disk->device_buffer*/
	pks_free_disk_pages(pks_disk);
/*Free pks_disk*/
	kfree(pks_disk);
	unregister_blkdev(major,"pks_blk_dev");
}

module_init(module_load);
module_exit(module_unload);
