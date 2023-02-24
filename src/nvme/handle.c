// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * This file is part of libnvme.
 * Copyright (c) 2023
 *
 * Authors: Vikash Kumar <vikash.k5@samsung.com>
 */
#include <errno.h>
#include <getopt.h>
#include <fcntl.h>
#include <inttypes.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <dirent.h>
#include <libgen.h>
#include <signal.h>
#include <syslog.h>
#include <linux/fs.h>

#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef CONFIG_XNVME
    #include <libxnvme.h>
    #include <libxnvme_nvm.h>
#endif

#include "mi.h"
#include "handle.h"

static bool is_chardev(struct nvme_dev *dev)
{
        return S_ISCHR(dev->direct.stat.st_mode);
}

static bool is_blkdev(struct nvme_dev *dev)
{
        return S_ISBLK(dev->direct.stat.st_mode);
}

static int open_dev_direct(struct nvme_dev **devp, char *devstr, int flags)
{
        struct nvme_dev *dev;
        int err;

        dev = calloc(1, sizeof(*dev));
        if (!dev)
                return -1;

        dev->type = NVME_DEV_DIRECT;
        dev->name = basename(devstr);
        err = open(devstr, flags);
        if (err < 0) {
                perror(devstr);
                goto err_free;
        }
        dev->direct.hdl.fd = err;
        dev->direct.hdl.dev_type = NVME_DEV_DIRECT;
        err = fstat(dev->direct.hdl.fd, &dev->direct.stat);
        if (err < 0) {
                perror(devstr);
                goto err_close;
        }
        if (!is_chardev(dev) && !is_blkdev(dev)) {
                fprintf(stderr, "%s is not a block or character device\n",
                        devstr);
                err = -ENODEV;
                goto err_close;
        }
        *devp = dev;
        return 0;

err_close:
        close(dev_hdl(dev)->fd);
err_free:
        free(dev);
        return err;
}

static int parse_mi_dev(char *dev, unsigned int *net, uint8_t *eid,
                        unsigned int *ctrl)
{
        int rc;

        /* <net>,<eid>:<ctrl-id> form */
        rc = sscanf(dev, "mctp:%u,%hhu:%u", net, eid, ctrl);
        if (rc == 3)
                return 0;

        /* <net>,<eid> form, implicit ctrl-id = 0 */
        *ctrl = 0;
        rc = sscanf(dev, "mctp:%u,%hhu", net, eid);
        if (rc == 2)
                return 0;

        return -1;
}

static int open_dev_mi_mctp(struct nvme_dev **devp, char *devstr)
{
        unsigned int net, ctrl_id;
        struct nvme_dev *dev;
        unsigned char eid;
        int rc;

        rc = parse_mi_dev(devstr, &net, &eid, &ctrl_id);
        if (rc) {
                fprintf(stderr, "invalid device specifier '%s'\n", devstr);
                return rc;
        }

        dev = calloc(1, sizeof(*dev));
        if (!dev)
                return -1;

        dev->type = NVME_DEV_MI;
        dev->name = devstr;

        /* todo: verbose argument */
        dev->mi.root = nvme_mi_create_root(stderr, LOG_WARNING);
        if (!dev->mi.root)
                goto err_free;

        dev->mi.ep = nvme_mi_open_mctp(dev->mi.root, net, eid);
        if (!dev->mi.ep)
                goto err_free_root;

        dev->mi.ctrl = nvme_mi_init_ctrl(dev->mi.ep, ctrl_id);
        if (!dev->mi.ctrl)
                goto err_close_ep;

        *devp = dev;
        return 0;

err_close_ep:
        nvme_mi_close(dev->mi.ep);
err_free_root:
        nvme_mi_free_root(dev->mi.root);
err_free:
        free(dev);
        return -1;
}

#ifdef CONFIG_XNVME
static int open_dev_xnvme(struct nvme_dev **devp, char *devname)
{
        int err;
        struct nvme_dev *dev;
        struct xnvme_dev *xnvme_handle = NULL;
        struct xnvme_opts opts = {0};
        opts.rdwr = 1;
        opts.create_mode = S_IRUSR | S_IWUSR;
        opts.nsid = 0x1;

        xnvme_handle = xnvme_dev_open(devname, &opts);
        if (!xnvme_handle) {
                printf("xnvme_dev failed \n");
                err = -1;
                goto perror;
        }

        dev = calloc(1, sizeof(*dev));
        if (!dev)
                return -1;

        dev->type = NVME_DEV_XNVME;
        dev->name = basename(devname);
        dev->direct.hdl.xdev = xnvme_handle;
        dev->direct.hdl.dev_type = NVME_DEV_XNVME;

        *devp = dev;
        return 0;

perror:
        perror(devname);
        return err;
}
#endif

static int check_arg_dev(int argc, char **argv)
{
        if (optind >= argc) {
                errno = EINVAL;
                perror(argv[0]);
                return -EINVAL;
        }
        return 0;
}

int get_dev(struct nvme_dev **dev, int argc, char **argv, int flags)
{
        char *devname;
        int ret;

        ret = check_arg_dev(argc, argv);
        if (ret)
                return ret;

        devname = argv[optind];
        if (!strncmp(devname, "mctp:", strlen("mctp:")))
                ret = open_dev_mi_mctp(dev, devname);
        else
	{
#ifdef CONFIG_XNVME
                ret = open_dev_xnvme(dev, devname);
#else
		ret = open_dev_direct(dev, devname, flags);
#endif
	}

        return ret;
}

void dev_close(struct nvme_dev *dev)
{
        switch (dev->type) {
        case NVME_DEV_DIRECT:
                close(dev->direct.hdl.fd);
                break;
        case NVME_DEV_XNVME:
#ifdef CONFIG_XNVME
                xnvme_dev_close((struct xnvme_dev*)dev->direct.hdl.xdev);
#endif
		break;
        case NVME_DEV_MI:
                nvme_mi_close(dev->mi.ep);
                nvme_mi_free_root(dev->mi.root);
                break;
        }
        free(dev);
}
