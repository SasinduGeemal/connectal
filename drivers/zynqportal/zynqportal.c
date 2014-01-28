/*
 * Generic bridge to memory-mapped hardware
 *
 * Author: Jamey Hicks <jamey.hicks@gmail.com>
 *
 * This file is licensed under the terms of the GNU General Public License
 * version 2.  This program is licensed "as is" without any warranty of any
 * kind, whether express or implied.
 */

#define DEBUG
#include <linux/version.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/poll.h>
#include <linux/uaccess.h>
#include <linux/miscdevice.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/clk.h>
#include <linux/ioctl.h>
#include <linux/dma-buf.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/scatterlist.h>

#include "zynqportal.h"

/////////////////////////////////////////////////////////////
// copied from portal.h


#define DRIVER_NAME "zynqportal"
#define DRIVER_DESCRIPTION "Generic userspace hardware bridge"
#define DRIVER_VERSION "0.1"

#ifdef DEBUG // was KERN_DEBUG
#define driver_devel(format, ...) \
	do { \
		printk(format, ## __VA_ARGS__); \
	} while (0)
#else
#define driver_devel(format, ...)
#endif

#define PORTAL_NAME_SZ 20

struct portal_data {
	struct device *dev;
        struct miscdevice misc;
        wait_queue_head_t wait_queue;
        const char *device_name;
        dma_addr_t dev_base_phys;
        dma_addr_t ind_reg_base_phys;
        void      *dev_base_virt;
        void      *ind_reg_base_virt;
        unsigned char portal_irq;
        int irq_requested;
};

struct portal_client {
    struct portal_data *portal_data;
};

struct portal_init_data {
	struct platform_device *pdev;
        const char *device_name;
};

// 
/////////////////////////////////////////////////////////////

static int portal_parse_hw_info(struct device_node *np,
				struct portal_init_data *init_data)
{
	u32 const *prop;
	int size;

	prop = of_get_property(np, "device-name", &size);
	if (!prop) {
                pr_err("Error %s getting device-name\n", DRIVER_NAME);
		return -EINVAL;
	}
        init_data->device_name = (char *)prop;
        driver_devel("%s: device_name=%s\n", DRIVER_NAME, init_data->device_name);
	return 0;
}

static void dump_ind_regs(const char *prefix, struct portal_data *portal_data)
{
        int i;
        for (i = 0; i < 10; i++) {
                unsigned long regval;
                regval = readl(portal_data->ind_reg_base_virt + i*4);
                driver_devel("%s reg %x value %08lx\n", prefix,
                             i*4, regval);
        }
}

static irqreturn_t portal_isr(int irq, void *dev_id)
{
	struct portal_data *portal_data = (struct portal_data *)dev_id;
	u32 int_status, int_en;


        //dump_ind_regs("ISR a", portal_data);
        int_status = readl(portal_data->ind_reg_base_virt + 0);
	int_en  = readl(portal_data->ind_reg_base_virt + 4);
	driver_devel("%s IRQ %s %d %x %x\n", __func__, portal_data->device_name, irq, int_status, int_en);

	if(int_status){
	  // disable interrupt.  this will be enabled by user mode 
	  // driver  after all the HW->SW FIFOs have been emptied
	  writel(0, portal_data->ind_reg_base_virt + 0x4);
	  
	  // driver_devel("%s IRQ before wake_up_interruptible\n", __func__);
	  //dump_ind_regs("ISR b", portal_data);
	  wake_up_interruptible(&portal_data->wait_queue);
	  //driver_devel("%s IRQ after wake_up_interruptible\n", __func__);
	  //driver_devel("%s blurgh=%p\n", __func__,  *((unsigned int*)NULL));        
	  return IRQ_HANDLED;
	} else {
	  return IRQ_NONE;
	}
}

static int portal_open(struct inode *inode, struct file *filep)
{
        u32 int_status, int_en;
	struct miscdevice *miscdev = filep->private_data;
	struct portal_data *portal_data =
                container_of(miscdev, struct portal_data, misc);
        struct portal_client *portal_client =
                (struct portal_client *)kzalloc(sizeof(struct portal_client), GFP_KERNEL);

        driver_devel("%s: %s ind_reg_base_phys %lx\n", __FUNCTION__, portal_data->device_name,
                     (long)portal_data->ind_reg_base_phys);
        
	//dump_ind_regs("portal_open", portal_data);

        portal_client->portal_data = portal_data;
        filep->private_data = portal_client;

	// sanity check, see if interrupts have been enabled
        //dump_ind_regs("enable interrupts", portal_data);

	if(!portal_data->irq_requested){

	  printk("%s about to call request_irq\n", __func__);
	  // read the interrupt as a sanity check
	  int_status = readl(portal_data->ind_reg_base_virt + 0);
	  int_en  = readl(portal_data->ind_reg_base_virt + 4);
	  driver_devel("%s IRQ %s %x %x\n", __func__, portal_data->device_name, int_status, int_en);

	  if (request_irq(portal_data->portal_irq, portal_isr, IRQF_TRIGGER_HIGH | IRQF_SHARED , portal_data->device_name, portal_data)) {
	    portal_data->portal_irq = 0;
	    printk("%s err_bb\n", __func__);
	    if(portal_data->portal_irq)
	      free_irq(portal_data->portal_irq, portal_data);
	  } else {
	    portal_data->irq_requested = 1;
	  }
	}

	return 0;
}


long portal_unlocked_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
        switch (cmd) {
	case PORTAL_SET_FCLK_RATE: {
		PortalClockRequest request;
		char clkname[8];
		int status = 0;
		struct clk *fclk = NULL;

		if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
			return -EFAULT;

		snprintf(clkname, sizeof(clkname), "FPGA%d", request.clknum);
		fclk = clk_get_sys(clkname, NULL);
		printk(KERN_INFO "[%s:%d] fclk %s %p\n", __FUNCTION__, __LINE__, clkname, fclk);
		if (!fclk)
			return -ENODEV;
		request.actual_rate = clk_round_rate(fclk, request.requested_rate);
		printk(KERN_INFO "[%s:%d] requested rate %ld actual rate %ld\n", __FUNCTION__, __LINE__, request.requested_rate, request.actual_rate);
		if ((status = clk_set_rate(fclk, request.actual_rate))) {
			printk(KERN_INFO "[%s:%d] err\n", __FUNCTION__, __LINE__);
			return status;
		}
                if (copy_to_user((void __user *)arg, &request, sizeof(request)))
                        return -EFAULT;
		return status;
	} break;
        default:
                printk("portal_unlocked_ioctl ENOTTY cmd=%x\n", cmd);
                return -ENOTTY;
        }

        return -ENODEV;
}

int portal_mmap(struct file *filep, struct vm_area_struct *vma)
{
	struct portal_client *portal_client = filep->private_data;
	struct portal_data *portal_data = portal_client->portal_data;
	unsigned long off = portal_data->dev_base_phys;
	unsigned long req_len = vma->vm_end - vma->vm_start + (vma->vm_pgoff << PAGE_SHIFT);

        if (!portal_client)
                return -ENODEV;
        if (vma->vm_pgoff > (~0UL >> PAGE_SHIFT))
                return -EINVAL;

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	vma->vm_pgoff = off >> PAGE_SHIFT;
	vma->vm_flags |= VM_IO;
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,9,0)
	vma->vm_flags |= VM_RESERVED;
#endif
        if (io_remap_pfn_range(vma, vma->vm_start, off >> PAGE_SHIFT,
                               vma->vm_end - vma->vm_start, vma->vm_page_prot))
                return -EAGAIN;
        printk("%s req_len=%lx off=%lx\n", __FUNCTION__, req_len, off);
	if(0)
	  dump_ind_regs(__FUNCTION__, portal_data);

        return 0;
}


unsigned int portal_poll (struct file *filep, poll_table *poll_table)
{
	struct portal_client *portal_client = filep->private_data;
	struct portal_data *portal_data = portal_client->portal_data;
        int int_status = readl(portal_data->ind_reg_base_virt + 0);
        int mask = 0;
        poll_wait(filep, &portal_data->wait_queue, poll_table);
        if (int_status & 1)
                mask = POLLIN | POLLRDNORM;
	if(0)
        printk("%s: %s int_status=%x mask=%x\n", __FUNCTION__, portal_data->device_name, int_status, mask);
        return mask;
}

static int portal_release(struct inode *inode, struct file *filep)
{
	struct portal_client *portal_client = filep->private_data;
	driver_devel("%s inode=%p filep=%p\n", __func__, inode, filep);
	kfree(portal_client);
	//driver_devel("%s blurgh=%d\n", __func__,  *((unsigned int*)NULL)); 
	//dump_stack();
	return 0;
}

static const struct file_operations portal_fops = {
	.open = portal_open,
        .mmap = portal_mmap,
        .unlocked_ioctl = portal_unlocked_ioctl,
        .poll = portal_poll,
	.release = portal_release,
};

int portal_init_driver(struct portal_init_data *init_data)
{
	struct device *dev;
	struct portal_data *portal_data;
        struct miscdevice *miscdev;
	struct resource *reg_res, *irq_res;
	int rc = 0, dev_range=0, reg_range=0;
	dev = &init_data->pdev->dev;

	reg_res = platform_get_resource(init_data->pdev, IORESOURCE_MEM, 0);
	irq_res = platform_get_resource(init_data->pdev, IORESOURCE_IRQ, 0);

	if (!reg_res || !irq_res) {
		pr_err("Error portal resources\n");
		return -ENODEV;
	}

	portal_data = kzalloc(sizeof(struct portal_data), GFP_KERNEL);
	if (!portal_data) {
		pr_err("Error portal allocating internal data\n");
		rc = -ENOMEM;
		goto err_mem;
	}

	portal_data->irq_requested = 0;
        portal_data->device_name = init_data->device_name;
        portal_data->dev_base_phys = reg_res->start;
        portal_data->ind_reg_base_phys = reg_res->start + (3 << 14);
			
	dev_range = reg_res->end - reg_res->start;
	reg_range = 1 << 14;

	portal_data->dev_base_virt = ioremap_nocache(portal_data->dev_base_phys, dev_range);
        portal_data->ind_reg_base_virt = ioremap_nocache(portal_data->ind_reg_base_phys, reg_range);
                
        pr_info("%s ind_reg_base phys %x/%x virt %p\n",
                portal_data->device_name,
                portal_data->ind_reg_base_phys, reg_range, portal_data->ind_reg_base_virt);

        init_waitqueue_head(&portal_data->wait_queue);
	portal_data->portal_irq = irq_res->start;

	portal_data->dev = dev;
	dev_set_drvdata(dev, (void *)portal_data);

        miscdev = &portal_data->misc;
        driver_devel("%s:%d miscdev=%p\n", __func__, __LINE__, miscdev);
        driver_devel("%s:%d portal_data=%p\n", __func__, __LINE__, portal_data);
        miscdev->minor = MISC_DYNAMIC_MINOR;
        miscdev->name = portal_data->device_name;
        miscdev->fops = &portal_fops;
        miscdev->parent = NULL;
        misc_register(miscdev);

        driver_devel("%s:%d about to return\n", __func__, __LINE__);
	return 0;

err_mem:
	if (portal_data) {
		kfree(portal_data);
	}

	dev_set_drvdata(dev, NULL);

	return rc;
}

int portal_deinit_driver(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct portal_data *portal_data = 
                (struct portal_data *)dev_get_drvdata(dev);

	if(portal_data->irq_requested)
	  free_irq(portal_data->portal_irq, portal_data);
	
	kfree(portal_data);

	dev_set_drvdata(dev, NULL);

	return 0;
}

static int portal_of_probe(struct platform_device *pdev)
{
	struct portal_init_data init_data;
	int rc;

        driver_devel("portal_of_probe\n");
	memset(&init_data, 0, sizeof(struct portal_init_data));
	init_data.pdev = pdev;
	rc = portal_parse_hw_info(pdev->dev.of_node, &init_data);
	driver_devel("portal_parse_hw_info returned %d\n", rc);

	if (rc)
	  return rc;
	
	return portal_init_driver(&init_data);
}

static int portal_of_remove(struct platform_device *pdev)
{
  struct device *dev = &pdev->dev;
  struct portal_data *portal_data =  (struct portal_data *)dev_get_drvdata(dev);
  driver_devel("%s:%s\n",__FUNCTION__, pdev->name);
  misc_deregister(&portal_data->misc);
  return portal_deinit_driver(pdev);
}

static struct of_device_id portal_of_match[]
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,9,0)
      __devinitdata
#endif
      = {
	{ .compatible = "linux,ushw-bridge-0.01.a" }, /* old name */
	{ .compatible = "linux,portal-0.01.a" },
	{/* end of table */},
};
MODULE_DEVICE_TABLE(of, portal_of_match);


static struct platform_driver portal_of_driver = {
	.probe = portal_of_probe,
	.remove = portal_of_remove,
	.driver = {
		.owner = THIS_MODULE,
		.name = DRIVER_NAME,
		.of_match_table = portal_of_match,
	},
};


static int __init portal_of_init(void)
{
	if (platform_driver_register(&portal_of_driver)) {
		pr_err("Error portal driver registration\n");
		return -ENODEV;
	}
	return 0;
}

static void __exit portal_of_exit(void)
{
	platform_driver_unregister(&portal_of_driver);
}



#ifndef MODULE
late_initcall(portal_of_init);
#else
module_init(portal_of_init);
module_exit(portal_of_exit);
#endif

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION(DRIVER_DESCRIPTION);
MODULE_VERSION(DRIVER_VERSION);
