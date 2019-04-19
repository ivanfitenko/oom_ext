#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>

#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>

#include <linux/sysctl.h>

#include <linux/vmalloc.h>

#include <linux/file.h>
#include <linux/fs.h>

#include <linux/notifier.h>

#define OOM_EXT_MBYTE 1048576
#define OOM_EXT_FLAG_MAXPATH 256
#define OOM_EXT_WORK_QUEUE_NAME "oom_ext"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,27)
#define OLD_INSERT_AT_HEAD
#include <linux/oom.h>
#else
#define OLD_INSERT_AT_HEAD , 0
extern int register_oom_notifier(struct notifier_block *nb);
extern int unregister_oom_notifier(struct notifier_block *nb);
#endif

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ivan Fitenko <sin@hostingnitro.com>");
MODULE_DESCRIPTION("support custom actions on OOM condition");

#define OOM_EXT_CTL 563 /* a random number, high enough */
enum {OOM_EXT_VAL=1, OOM_EXT_OTHER};

/* sysctl defaults */
long gracetime = 0;   /* time in oom before panic, sec. 0 to disable */
long resettime = 300; /* default time out of oom to consider it ended */
int bufsize = 32; /* emergency buffer is 32M by default. 0 to disable */
int crashflag = 1; /* set crash flag by default, remove if oom ended */
static char crashflag_name[OOM_EXT_FLAG_MAXPATH] = "/oomflag"; /* flag name */

/* sysctl controls */
static ctl_table oom_ext_table[] = {
	{
	.ctl_name = OOM_EXT_VAL,
	.procname = "gracetime",
	.data = &gracetime,
	.maxlen = sizeof(int),
	.mode = 0644,
	.child = NULL,
	.proc_handler = &proc_dointvec,
	.strategy = &sysctl_intvec,
	},
	{
	.ctl_name = OOM_EXT_VAL,
	.procname = "resettime",
	.data = &resettime,
	.maxlen = sizeof(int),
	.mode = 0644,
	.child = NULL,
	.proc_handler = &proc_dointvec,
	.strategy = &sysctl_intvec,
	},
	{
	.ctl_name = OOM_EXT_VAL,
	.procname = "bufsize",
	.data = &bufsize,
	.maxlen = sizeof(int),
	.mode = 0644,
	.child = NULL,
	.proc_handler = &proc_dointvec,
	.strategy = &sysctl_intvec,
	},
	{
	.ctl_name = OOM_EXT_VAL,
	.procname = "crashflag",
	.data = &crashflag,
	.maxlen = sizeof(int),
	.mode = 0644,
	.child = NULL,
	.proc_handler = &proc_dointvec,
	.strategy = &sysctl_intvec,
	},
	{
	.ctl_name = OOM_EXT_VAL,
	.procname = "crashflag_name",
	.data = &crashflag_name,
	.maxlen = OOM_EXT_FLAG_MAXPATH,
	.mode = 0644,
	.child = NULL,
	.proc_handler = &proc_dostring,
	.strategy = &sysctl_string,
	},
        {0}
        };



/* sysfs directory */
static ctl_table oom_ext_kern_table[] = {
        {OOM_EXT_CTL, "oom_ext", NULL, 0, 0555, oom_ext_table},
        {0}
        };

/* the parent directory */
static ctl_table oom_ext_root_table[] = {
        {CTL_VM, "vm", NULL, 0, 0555, oom_ext_kern_table},
        {0}
        };

static struct ctl_table_header *oom_ext_table_header;

int oom_ext_event_handler (struct notifier_block *self,
			     unsigned long oom_val, void *oom_data);

static int oom_ext_flag = 0;
static unsigned long oom_ext_now = 0;
static unsigned long oom_ext_grace_start = 0;
static unsigned long oom_ext_reset_wait = 0;
static int oom_ext_bufsize = 0;

struct file *oom_flag_file;

int file_done;

char * oom_ext_emergency_buffer;
int oom_ext_have_emergency_buffer;
unsigned long oom_ext_i;

/* OOM Event Notifier Definition */
static struct notifier_block oom_ext_nb = {
	oom_ext_event_handler,
	NULL,
	0
};

static int die = 0;/* set this to 1 for shutdown */
static struct workqueue_struct *oom_ext_workqueue;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,27)
static void intrpt_routine(struct work_struct *);
static DECLARE_DELAYED_WORK(Task, intrpt_routine);
static void intrpt_routine(struct work_struct* irrelevant)
#else
static struct work_struct Task;
static void intrpt_routine(void *);
static DECLARE_WORK(Task, intrpt_routine, NULL);
static void intrpt_routine(void *irrelevant)
#endif
{
	/* 
	 * if an oom event flag is set, start/update the reset wait
	 * period counters and acknowledge event by clearing the flag
	*/
	if (oom_ext_flag) {
	    oom_ext_flag=0;
	    oom_ext_reset_wait=jiffies;
	    /* we can't log anything while in OOM, so do it here */
	    printk (KERN_ALERT "OOM state reported\n");
	    if (crashflag) {
	        printk (KERN_ALERT "dropping oom flag\n");
		oom_flag_file = filp_open("/oomflag",
				O_WRONLY|O_CREAT|O_DIRECT,0644);
		filp_close(oom_flag_file,0);
		file_done=1;
	    }
	} else if (oom_ext_reset_wait) {
	    /*
	     * the grace period ends when OOM state wasn't observed
	     * during SOMEVALUE seconds
	    */
	    if (((long)jiffies-(long)oom_ext_reset_wait)/HZ > resettime) {
		    oom_ext_reset_wait = 0;
		    oom_ext_grace_start = 0;
		    file_done = 0;
        	    printk (KERN_ALERT "all ok,resetting oom_ext timers\n");
        	    /* attempt to restore buffers */
        	    if (!oom_ext_have_emergency_buffer) {
        		printk (KERN_INFO "OOM_EXT: re-populating emergency buffer...");
			if ( (oom_ext_emergency_buffer 
					= (char*) vmalloc(bufsize*OOM_EXT_MBYTE)) ) {
			    for (oom_ext_i=0; 
				    oom_ext_i< (bufsize*OOM_EXT_MBYTE); oom_ext_i++) {
				oom_ext_emergency_buffer[oom_ext_i] = '0';
		    	    }
		    	    oom_ext_bufsize = bufsize;
		    	    oom_ext_have_emergency_buffer = 1;
		    	} else {
		    	    printk (KERN_ALERT "OOM_EXT: buffer allocation failed\n");
		    	    printk (KERN_ALERT "OOM_EXT: will retry...");
		    	    bufsize = oom_ext_bufsize;
		    	    oom_ext_have_emergency_buffer = 0;
		    	}
	    	    }
    	    } 
	} else { /* normal operation cycle */
	    if (bufsize != oom_ext_bufsize || 
					(!oom_ext_have_emergency_buffer) ) {
        	printk (KERN_INFO 
        		"OOM_EXT: emergency buffer resized, re-populating...");
        	if (oom_ext_emergency_buffer) /* a useless check just in case */
        	    vfree (oom_ext_emergency_buffer);
		if ( (oom_ext_emergency_buffer = 
			(char*) vmalloc(bufsize*OOM_EXT_MBYTE)) ) {
		    for (oom_ext_i=0; 
			    oom_ext_i< (bufsize*OOM_EXT_MBYTE); oom_ext_i++) {
			oom_ext_emergency_buffer[oom_ext_i] = '0';
		    }
		    oom_ext_bufsize = bufsize;
		    oom_ext_have_emergency_buffer = 1;
		} else {
		    printk (KERN_ALERT "OOM_EXT: failed to resize buffer\n");
		    printk (KERN_ALERT "OOM_EXT: will try old size...\n");
		    bufsize = oom_ext_bufsize;
		    oom_ext_have_emergency_buffer = 0;
		}
	    }
	}
	/*
	 * If cleanup wants us to die
 	*/
	if (die == 0)
	queue_delayed_work(oom_ext_workqueue, &Task, 100);
}



/* OOM Notification Event Handler */
int oom_ext_event_handler (struct notifier_block *self,
 unsigned long oom_val, void *oom_data)
{
	if (oom_ext_have_emergency_buffer) {
	    printk (KERN_ALERT "OOM_EXT: dropping emergency buffer\n");
	    vfree (oom_ext_emergency_buffer);
	    oom_ext_have_emergency_buffer = 0;
	}
	if (!oom_ext_grace_start)
	    oom_ext_grace_start=oom_ext_now=jiffies;
	else
	    oom_ext_now=jiffies;
	/*
	* when we're too long in OOM state, it could be useful to trigger
	* kernel panic to reboot the system with sysctl kern.panic=1
	*/
	if (gracetime) {
	    if (((long)oom_ext_now-(long)oom_ext_grace_start)/HZ > gracetime) {
		/*
		* if we need to leave panic token on fs, don't panic
		* until we try to set it
		*/
		if (!crashflag || file_done)
		    panic("I NEAR BIRD: too long in oom\n ");
	    }
	}
	oom_ext_flag=1;
	return 0;
}

int init_module(void)
{
        oom_ext_table_header = register_sysctl_table(oom_ext_root_table 
    							    OLD_INSERT_AT_HEAD);
	/* 
	 * Put the task in the work_timer task queue, so it will be executed at
	 * next timer interrupt
	 */
	oom_ext_workqueue = create_workqueue(OOM_EXT_WORK_QUEUE_NAME);
	queue_delayed_work(oom_ext_workqueue, &Task, 100);
	register_oom_notifier (&oom_ext_nb);
	file_done = 0;
	/*emergency buffer*/
	oom_ext_have_emergency_buffer = 0;
	printk (KERN_INFO "OOM_EXT: populating emergency buffer...");
	/* buffer size goes in megabytes */
	if ( (oom_ext_emergency_buffer = (char*) vmalloc(bufsize*OOM_EXT_MBYTE)) ) {
	    for (oom_ext_i=0; oom_ext_i< (bufsize*OOM_EXT_MBYTE); oom_ext_i++) {
		oom_ext_emergency_buffer[oom_ext_i] = '0';
	    }
	    oom_ext_have_emergency_buffer = 1;
	} else {
	    printk (KERN_ALERT "OOM_EXT: buffer allocation failed on init!\n");
	    printk (KERN_ALERT "OOM_EXT: will not retry.\n");
	    bufsize = 0;
	    oom_ext_have_emergency_buffer = 0;
	}
	oom_ext_bufsize = bufsize;
	printk(KERN_INFO "OOM_EXT enabled\n");
	return 0;
}

void cleanup_module(void)
{
	die = 1;/* keep intrp_routine from queueing itself */
	cancel_delayed_work(&Task);/* no "new ones" */
	flush_workqueue(oom_ext_workqueue);/* wait till all "old ones" finished */
	destroy_workqueue(oom_ext_workqueue);
	unregister_sysctl_table(oom_ext_table_header);
	unregister_oom_notifier (&oom_ext_nb);

	if (oom_ext_have_emergency_buffer) {
	    vfree (oom_ext_emergency_buffer);
	}
	printk(KERN_INFO "OOM_EXT disabled\n");
}

