/* This file contains the main program of the File System.  It consists of
 * a loop that gets messages requesting work, carries out the work, and sends
 * replies.
 *
 * The entry points into this file are:
 *   main:	main program of the File System
 *   reply:	send a reply to a process after the requested work is done
 *
 * Changes:
 *   Mar 23, 2005   allow arbitrary partitions as RAM disk  (Jorrit N. Herder)
 *   Jan 10, 2005   register fkeys with TTY for debug dumps  (Jorrit N. Herder)
 */

struct super_block;		/* proto.h needs to know this */

#include "fs.h"
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <sys/ioc_memory.h>
#include <sys/svrctl.h>
#include <minix/utils.h>
#include <minix/callnr.h>
#include <minix/com.h>
#include <minix/keymap.h>
#include <minix/const.h>
#include "buf.h"
#include "dmap.h"
#include "file.h"
#include "fproc.h"
#include "inode.h"
#include "param.h"
#include "super.h"



FORWARD _PROTOTYPE( void fs_init, (void)				);
FORWARD _PROTOTYPE( int igetenv, (char *var)				);
FORWARD _PROTOTYPE( void get_work, (void)				);
FORWARD _PROTOTYPE( void load_ram, (void)				);
FORWARD _PROTOTYPE( void load_super, (Dev_t super_dev)			);


/*===========================================================================*
 *				main					     *
 *===========================================================================*/
PUBLIC void main()
{
/* This is the main program of the file system.  The main loop consists of
 * three major activities: getting new work, processing the work, and sending
 * the reply.  This loop never terminates as long as the file system runs.
 */
  int error;

  fs_init();

  /* This is the main loop that gets work, processes it, and sends replies. */
  while (TRUE) {
	get_work();		/* sets who and call_nr */

	fp = &fproc[who];	/* pointer to proc table struct */
	super_user = (fp->fp_effuid == SU_UID ? TRUE : FALSE);   /* su? */

 	/* Check for special control messages first. */
        if (call_nr == HARD_STOP) { 
        	do_sync();
        	sys_exit(0);  		/* never returns */
        } else if (call_nr == FKEY_PRESSED) {
        	do_fkey_pressed();
        	continue;		/* get work again */
        }

	/* Call the internal function that does the work. */
	if (call_nr < 0 || call_nr >= NCALLS) { 
		error = ENOSYS;
		printf("FS, warning illegal %d system call by %d\n", call_nr, who);
	} else {
		error = (*call_vec[call_nr])();
	}

	/* Copy the results back to the user and send reply. */
	if (error != SUSPEND) { reply(who, error); }
	if (rdahed_inode != NIL_INODE) {
		read_ahead(); /* do block read ahead */
	}

  }
}


/*===========================================================================*
 *				get_work				     *
 *===========================================================================*/
PRIVATE void get_work()
{  
  /* Normally wait for new input.  However, if 'reviving' is
   * nonzero, a suspended process must be awakened.
   */

  register struct fproc *rp;

  if (reviving != 0) {
	/* Revive a suspended process. */
	for (rp = &fproc[0]; rp < &fproc[NR_PROCS]; rp++) 
		if (rp->fp_revived == REVIVING) {
			who = (int)(rp - fproc);
			call_nr = rp->fp_fd & BYTE;
			m_in.fd = (rp->fp_fd >>8) & BYTE;
			m_in.buffer = rp->fp_buffer;
			m_in.nbytes = rp->fp_nbytes;
			rp->fp_suspended = NOT_SUSPENDED; /*no longer hanging*/
			rp->fp_revived = NOT_REVIVING;
			reviving--;
			return;
		}
	panic("get_work couldn't revive anyone", NO_NUM);
  }

  /* Normal case.  No one to revive. */
  if (receive(ANY, &m_in) != OK) panic("fs receive error", NO_NUM);
  who = m_in.m_source;
  call_nr = m_in.m_type;
}

/*===========================================================================*
 *				buf_pool				     *
 *===========================================================================*/
PRIVATE void buf_pool(void)
{
/* Initialize the buffer pool. */

  register struct buf *bp;

  bufs_in_use = 0;
  front = &buf[0];
  rear = &buf[NR_BUFS - 1];

  for (bp = &buf[0]; bp < &buf[NR_BUFS]; bp++) {
	bp->b_blocknr = NO_BLOCK;
	bp->b_dev = NO_DEV;
	bp->b_next = bp + 1;
	bp->b_prev = bp - 1;
  }
  buf[0].b_prev = NIL_BUF;
  buf[NR_BUFS - 1].b_next = NIL_BUF;

  for (bp = &buf[0]; bp < &buf[NR_BUFS]; bp++) bp->b_hash = bp->b_next;
  buf_hash[0] = front;

}

/*===========================================================================*
 *				reply					     *
 *===========================================================================*/
PUBLIC void reply(whom, result)
int whom;			/* process to reply to */
int result;			/* result of the call (usually OK or error #) */
{
/* Send a reply to a user process. It may fail (if the process has just
 * been killed by a signal), so don't check the return code.  If the send
 * fails, just ignore it.
 */
  int s;
  m_out.reply_type = result;
  s = send(whom, &m_out);
  if (s != OK) printf("FS: couldn't send reply: %d\n", s);
}


/*===========================================================================*
 *				fs_init					     *
 *===========================================================================*/
PRIVATE void fs_init()
{
/* Initialize global variables, tables, etc. */
  register struct inode *rip;
  int key, s, i;
  message mess;

  /* Certain relations must hold for the file system to work at all. Some 
   * extra block_size requirements are checked at super-block-read-in time.
   */
  if (OPEN_MAX > 127) panic("OPEN_MAX > 127", NO_NUM);
  if (NR_BUFS < 6) panic("NR_BUFS < 6", NO_NUM);
  if (V1_INODE_SIZE != 32) panic("V1 inode size != 32", NO_NUM);
  if (V2_INODE_SIZE != 64) panic("V2 inode size != 64", NO_NUM);
  if (OPEN_MAX > 8 * sizeof(long)) panic("Too few bits in fp_cloexec", NO_NUM);

  /* The following initializations are needed to let dev_opcl succeed .*/
  fp = (struct fproc *) NULL;
  who = FS_PROC_NR;

  map_controllers();		/* map controller devices to drivers */
  buf_pool();			/* initialize buffer pool */
  load_ram();			/* init RAM disk, load if it is root */
  load_super(root_dev);		/* load super block for root device */


  /* Initialize the process table with help of the process manager messages. 
   * Expect one message for each system process with its slot number and pid. 
   * When no more processes follow, the magic process number NONE is sent. 
   * Then, stop and synchronize with the PM.
   */
  do {
  	if (OK != (s=receive(PM_PROC_NR, &mess)))
  		panic("FS couldn't receive from PM", s);
  	if (NONE == mess.PR_PROC_NR) break; 

	fp = &fproc[mess.PR_PROC_NR];
	fp->fp_pid = mess.PR_PID;
	rip = get_inode(root_dev, ROOT_INODE);
	dup_inode(rip);
	fp->fp_rootdir = rip;
	fp->fp_workdir = rip;
	fp->fp_realuid = (uid_t) SYS_UID;
	fp->fp_effuid = (uid_t) SYS_UID;
	fp->fp_realgid = (gid_t) SYS_GID;
	fp->fp_effgid = (gid_t) SYS_GID;
	fp->fp_umask = ~0;
   
  } while (TRUE);			/* continue until process NONE */
  mess.m_type = OK;			/* tell PM that we succeeded */
  s=send(PM_PROC_NR, &mess);		/* send synchronization message */

  /* Register function keys with TTY. */
  for (key=SF5; key<=SF6; key++) {
  	if ((i=fkey_enable(key))!=OK) {
  		printf("Warning: FS couldn't register Shift+F%d key: %d\n",
  			key-SF1+1, i);
  	}
  }
}


/*===========================================================================*
 *				igetenv					     *
 *===========================================================================*/
PRIVATE int igetenv(key)
char *key;
{
/* Ask kernel for an integer valued boot environment variable. */
  char value[64];
  int i;

  if ((i = get_mon_param(key, value, sizeof(value))) != OK)
      printf("FS: Warning, couldn't get monitor param: %d\n", i);
  return(atoi(value));
}


/*===========================================================================*
 *				load_ram				     *
 *===========================================================================*/
PRIVATE void load_ram(void)
{
/* Allocate a RAM disk with size given in the boot parameters. If a RAM disk 
 * image is given, the copy the entire image device block-by-block to a RAM 
 * disk with the same size as the image.
 * If the root device is not set, the RAM disk will be used as root instead. 
 */
  register struct buf *bp, *bp1;
  u32_t lcount, ram_size_kb;
  zone_t zones;
  struct super_block *sp, *dsp;
  block_t b;
  Dev_t image_dev;
  int r;
  static char sbbuf[MIN_BLOCK_SIZE];
  int block_size_image, block_size_ram, ramfs_block_size;

  /* Get some boot environment variables. */
  root_dev = igetenv("rootdev");
  image_dev = igetenv("ramimagedev");
  ram_size_kb = igetenv("ramsize");

  /* Open the root device. */
  if (dev_open(root_dev, FS_PROC_NR, R_BIT|W_BIT) != OK) {
	panic("Cannot open root device",NO_NUM);
  }

  /* If we must initialize a ram disk, get details from the image device. */
  if (root_dev == DEV_RAM || root_dev != image_dev) {
  	u32_t fsmax;
	if (dev_open(image_dev, FS_PROC_NR, R_BIT) != OK)
		panic("Cannot open RAM image device", NO_NUM);

	/* Get size of RAM disk image from the super block. */
	sp = &super_block[0];
	sp->s_dev = image_dev;
	if (read_super(sp) != OK) panic("Bad RAM disk image FS", NO_NUM);

	lcount = sp->s_zones << sp->s_log_zone_size;	/* # blks on root dev*/

	/* Stretch the RAM disk file system to the boot parameters size, but
	 * no further than the last zone bit map block allows.
	 */
	if (ram_size_kb*1024 < lcount*sp->s_block_size)
		ram_size_kb = lcount*sp->s_block_size/1024;
	fsmax = (u32_t) sp->s_zmap_blocks * CHAR_BIT * sp->s_block_size;
	fsmax = (fsmax + (sp->s_firstdatazone-1)) << sp->s_log_zone_size;
	if (ram_size_kb*1024 > fsmax*sp->s_block_size)
		ram_size_kb = fsmax*sp->s_block_size/1024;
  }

  /* Tell RAM driver how big the RAM disk must be. */
  m_out.m_type = DEV_IOCTL;
  m_out.PROC_NR = FS_PROC_NR;
  m_out.DEVICE = RAM_DEV;
  m_out.REQUEST = MIOCRAMSIZE;
  m_out.POSITION = ram_size_kb*1024;
  if (sendrec(MEMORY, &m_out) != OK || m_out.REP_STATUS != OK)
	panic("Can't set RAM disk size", NO_NUM);


#if ENABLE_CACHE2
  /* The RAM disk is a second level block cache while not otherwise used. */
  init_cache2(ram_size);
#endif

  /* See if we must load the RAM disk image, otherwise return. */
  if (root_dev != DEV_RAM && root_dev == image_dev)
  	return;

  /* Copy the blocks one at a time from the image to the RAM disk. */
  printf("Loading RAM disk.\33[23CLoaded:    0K ");

  inode[0].i_mode = I_BLOCK_SPECIAL;	/* temp inode for rahead() */
  inode[0].i_size = LONG_MAX;
  inode[0].i_dev = image_dev;
  inode[0].i_zone[0] = image_dev;

  block_size_ram = get_block_size(DEV_RAM);
  block_size_image = get_block_size(image_dev);

  if(block_size_ram != block_size_image) {
  	printf("ram block size: %d image block size: %d\n", 
  		block_size_ram, block_size_image);
  	panic("Sorry, ram disk and image disk block sizes have to be the same.", NO_NUM);
  }

  for (b = 0; b < (block_t) lcount; b++) {
	bp = rahead(&inode[0], b, (off_t)block_size_image * b, block_size_image);
	bp1 = get_block(root_dev, b, NO_READ);
	memcpy(bp1->b_data, bp->b_data, (size_t) block_size_image);
	bp1->b_dirt = DIRTY;
	put_block(bp, FULL_DATA_BLOCK);
	put_block(bp1, FULL_DATA_BLOCK);
	printf("\b\b\b\b\b\b\b%5ldK ", ((long) b * block_size_image)/1024L);
  }

  printf("\rRAM disk of %u kb loaded.\33[K", ram_size_kb);
  if (root_dev == DEV_RAM) printf(" RAM disk is used as root FS.");
  printf("\n\n");

  /* Invalidate and close the image device. */
  invalidate(image_dev);
  dev_close(image_dev);

  /* Resize the RAM disk root file system. */
  if(dev_io(DEV_READ, root_dev, FS_PROC_NR,
  	sbbuf, SUPER_BLOCK_BYTES, MIN_BLOCK_SIZE, 0) != MIN_BLOCK_SIZE) {
  	printf("WARNING: ramdisk read for resizing failed\n");
  }
  dsp = (struct super_block *) sbbuf;
  if(dsp->s_magic == SUPER_V3)
  	ramfs_block_size = dsp->s_block_size;
  else
  	ramfs_block_size = STATIC_BLOCK_SIZE;
  zones = (ram_size_kb * 1024 / ramfs_block_size) >> sp->s_log_zone_size;

  dsp->s_nzones = conv2(sp->s_native, (u16_t) zones);
  dsp->s_zones = conv4(sp->s_native, zones);
  if(dev_io(DEV_WRITE, root_dev, FS_PROC_NR,
  	sbbuf, SUPER_BLOCK_BYTES, MIN_BLOCK_SIZE, 0) != MIN_BLOCK_SIZE) {
  	printf("WARNING: ramdisk write for resizing failed\n");
  }
}


/*===========================================================================*
 *				load_super				     *
 *===========================================================================*/
PRIVATE void load_super(super_dev)
dev_t super_dev;			/* place to get superblock from */
{
  int bad;
  register struct super_block *sp;
  register struct inode *rip;

  /* Initialize the super_block table. */
  for (sp = &super_block[0]; sp < &super_block[NR_SUPERS]; sp++)
  	sp->s_dev = NO_DEV;

  /* Read in super_block for the root file system. */
  sp = &super_block[0];
  sp->s_dev = super_dev;

  /* Check super_block for consistency. */
  bad = (read_super(sp) != OK);
  if (!bad) {
	rip = get_inode(super_dev, ROOT_INODE);	/* inode for root dir */
	if ( (rip->i_mode & I_TYPE) != I_DIRECTORY || rip->i_nlinks < 3) bad++;
  }
  if (bad) panic("Invalid root file system", NO_NUM);

  sp->s_imount = rip;
  dup_inode(rip);
  sp->s_isup = rip;
  sp->s_rd_only = 0;
  return;
}
