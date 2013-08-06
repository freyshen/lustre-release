/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.gnu.org/licenses/gpl-2.0.htm
 *
 * GPL HEADER END
 */
/*
 * (C) Copyright 2012 Commissariat a l'energie atomique et aux energies
 *     alternatives
 *
 */
/* HSM copytool program for POSIX filesystem-based HSM's.
 *
 * An HSM copytool daemon acts on action requests from Lustre to copy files
 * to and from an HSM archive system. This one in particular makes regular
 * POSIX filesystem calls to a given path, where an HSM is presumably mounted.
 *
 * This particular tool can also import an existing HSM archive.
 */

#include <utime.h>
#include <sys/xattr.h>
#include <dirent.h>
#include <stdlib.h>
#include <sys/errno.h>
#include <lustre/lustre_idl.h>
#include <lustre/lustreapi.h>

/* Progress reporting period */
#define REPORT_INTERVAL_DEFAULT 30
/* HSM hash subdir permissions */
#define DIR_PERM S_IRWXU
/* HSM hash file permissions */
#define FILE_PERM (S_IRUSR | S_IWUSR)

#define ONE_MB 0x100000

/* copytool uses a 32b bitmask field to register with kuc
 * archive num = 0 => all
 * archive num from 1 to 32
 */
#define MAX_ARCHIVE_CNT (sizeof(__u32) * 8)

enum ct_action {
	CA_IMPORT = 1,
	CA_REBIND,
	CA_MAXSEQ,
	CA_DAEMON,
};

struct options {
	int			 o_copy_attrs;
	int			 o_dry_run;
	int			 o_abort_on_error;
	int			 o_shadow_tree;
	int			 o_verbose;
	int			 o_copy_xattrs;
	int			 o_archive_cnt;
	int			 o_archive_id[MAX_ARCHIVE_CNT];
	int			 o_report_int;
	unsigned long long	 o_bandwidth;
	size_t			 o_chunk_size;
	enum ct_action		 o_action;
	char			*o_mnt;
	char			*o_hsm_root;
	char			*o_src; /* for import, or rebind */
	char			*o_dst; /* for import, or rebind */
};

/* everything else is zeroed */
struct options opt = {
	.o_copy_attrs = 1,
	.o_shadow_tree = 1,
	.o_verbose = LLAPI_MSG_WARN,
	.o_copy_xattrs = 1,
	.o_report_int = REPORT_INTERVAL_DEFAULT,
	.o_chunk_size = ONE_MB,
};

/* The LLAPI will hold an open FD on lustre for us. Additionally open one on
 * the archive FS root to make sure it doesn't drop out from under us (and
 * remind the admin to shutdown the copytool before unmounting). */
static int arc_fd = -1;

static int err_major;
static int err_minor;

static char cmd_name[PATH_MAX];
static char fs_name[MAX_OBD_NAME + 1];

static struct hsm_copytool_private *ctdata;


#define CT_ERROR(format, ...) \
	llapi_printf(LLAPI_MSG_ERROR, "%s: "format, cmd_name, ## __VA_ARGS__)
#define CT_DEBUG(format, ...) \
	llapi_printf(LLAPI_MSG_DEBUG, "%s: "format, cmd_name, ## __VA_ARGS__)
#define CT_WARN(format, ...) \
	llapi_printf(LLAPI_MSG_WARN, "%s: "format, cmd_name, ## __VA_ARGS__)
#define CT_TRACE(format, ...) \
	llapi_printf(LLAPI_MSG_INFO, "%s: "format, cmd_name, ## __VA_ARGS__)

static void usage(const char *name, int rc)
{
	fprintf(stdout,
	" Usage: %s [options]... <mode> <lustre_mount_point>\n"
	"The Lustre HSM Posix copy tool can be used as a daemon or "
	"as a command line tool\n"
	"The Lustre HSM daemon acts on action requests from Lustre\n"
	"to copy files to and from an HSM archive system.\n"
	"This POSIX-flavored daemon makes regular POSIX filesystem calls\n"
	"to an HSM mounted at a given hsm_root.\n"
	"   -d, --daemon        Daemon mode, run in background\n"
	" Options:\n"
	"   --no-attr           Don't copy file attributes\n"
	"   --no-shadow         Don't create shadow namespace in archive\n"
	"   --no-xattr          Don't copy file extended attributes\n"
	"The Lustre HSM tool performs administrator-type actions\n"
	"on a Lustre HSM archive.\n"
	"This POSIX-flavored tool can link an existing HSM namespace\n"
	"into a Lustre filesystem.\n"
	" Usage:\n"
	"   %s [options] --import <src> <dst> <lustre_mount_point>\n"
	"      import an archived subtree at\n"
	"       <src> (relative to hsm_root) into the Lustre filesystem at\n"
	"       <dst> (absolute)\n"
	"   %s [options] --rebind <old_FID> <new_FID> <lustre_mount_point>\n"
	"      rebind an entry in the HSM to a new FID\n"
	"       <old_FID> old FID the HSM entry is bound to\n"
	"       <new_FID> new FID to bind the HSM entry to\n"
	"   %s [options] --rebind <list_file> <lustre_mount_point>\n"
	"      perform the rebind operation for all FID in the list file\n"
	"       each line of <list_file> consists of <old_FID> <new_FID>\n"
	"   %s [options] --max-sequence <fsname>\n"
	"       return the max fid sequence of archived files\n"
	"   -A, --archive <#>        Archive number (repeatable)\n"
	"   -p, --hsm-root <path>    Target HSM mount point\n"
	"   -q, --quiet              Produce less verbose output\n"
	"   -v, --verbose            Produce more verbose output\n"
	"   -c, --chunk-size <sz>    I/O size used during data copy\n"
	"                            (unit can be used, default is MB)\n"
	"   --abort-on-error         Abort operation on major error\n"
	"   --dry-run                Don't run, just show what would be done\n"
	"   --bandwidth <bw>         Limit I/O bandwidth (unit can be used\n,"
	"                            default is MB)\n",
	cmd_name, cmd_name, cmd_name, cmd_name, cmd_name);

	exit(rc);
}

static int ct_parseopts(int argc, char * const *argv)
{
	struct option long_opts[] = {
		{"abort-on-error", no_argument,	      &opt.o_abort_on_error, 1},
		{"abort_on_error", no_argument,	      &opt.o_abort_on_error, 1},
		{"archive",	   required_argument, NULL,		   'A'},
		{"bandwidth",	   required_argument, NULL,		   'b'},
		{"chunk-size",	   required_argument, NULL,		   'c'},
		{"chunk_size",	   required_argument, NULL,		   'c'},
		{"daemon",	   no_argument,	      NULL,		   'd'},
		{"dry-run",	   no_argument,	      &opt.o_dry_run,	    1},
		{"help",	   no_argument,	      NULL,		   'h'},
		{"hsm-root",	   required_argument, NULL,		   'p'},
		{"hsm_root",	   required_argument, NULL,		   'p'},
		{"import",	   no_argument,	      NULL,		   'i'},
		{"max-sequence",   no_argument,	      NULL,		   'M'},
		{"max_sequence",   no_argument,	      NULL,		   'M'},
		{"no-attr",	   no_argument,	      &opt.o_copy_attrs,    0},
		{"no_attr",	   no_argument,	      &opt.o_copy_attrs,    0},
		{"no-shadow",	   no_argument,	      &opt.o_shadow_tree,   0},
		{"no_shadow",	   no_argument,	      &opt.o_shadow_tree,   0},
		{"no-xattr",	   no_argument,	      &opt.o_copy_xattrs,   0},
		{"no_xattr",	   no_argument,	      &opt.o_copy_xattrs,   0},
		{"quiet",	   no_argument,	      NULL,		   'q'},
		{"rebind",	   no_argument,	      NULL,		   'r'},
		{"report",	   required_argument, &opt.o_report_int,    0},
		{"verbose",	   no_argument,	      NULL,		   'v'},
		{0, 0, 0, 0}
	};
	int			 c;
	unsigned long long	 value;
	unsigned long long	 unit;

	optind = 0;
	while ((c = getopt_long(argc, argv, "A:b:c:dhiMp:qruv",
				long_opts, NULL)) != -1) {
		switch (c) {
		case 'A':
			if ((opt.o_archive_cnt >= MAX_ARCHIVE_CNT) ||
			    (atoi(optarg) >= MAX_ARCHIVE_CNT)) {
				CT_ERROR("archive number must be less"
					 "than %lu\n", MAX_ARCHIVE_CNT);
				return -E2BIG;
			}
			opt.o_archive_id[opt.o_archive_cnt] = atoi(optarg);
			opt.o_archive_cnt++;
			break;
		case 'b': /* -b and -c have both a number with unit as arg */
		case 'c':
			unit = ONE_MB;
			if (llapi_parse_size(optarg, &value, &unit, 0) < 0) {
				CT_ERROR("bad value for -%c '%s'\n", c, optarg);
				return -EINVAL;
			}
			if (c == 'c')
				opt.o_chunk_size = value;
			else
				opt.o_bandwidth = value;
			break;
		case 'd':
			opt.o_action = CA_DAEMON;
			break;
		case 'h':
			usage(argv[0], 0);
		case 'i':
			opt.o_action = CA_IMPORT;
			break;
		case 'M':
			opt.o_action = CA_MAXSEQ;
			break;
		case 'p':
			opt.o_hsm_root = optarg;
			break;
		case 'q':
			opt.o_verbose--;
			break;
		case 'r':
			opt.o_action = CA_REBIND;
			break;
		case 'v':
			opt.o_verbose++;
			break;
		case 0:
			break;
		default:
			CT_ERROR("unrecognized option '%s'\n",
				 argv[optind - 1]);
			return -EINVAL;
		}
	}

	switch (opt.o_action) {
	case CA_IMPORT:
		/* src dst mount_point */
		if (argc != optind + 3) {
			CT_ERROR("--import requires 2 arguments\n");
			return -EINVAL;
		}
		opt.o_src = argv[optind++];
		opt.o_dst = argv[optind++];
		break;
	case CA_REBIND:
		/* FID1 FID2 mount_point or FILE mount_point */
		if (argc == optind + 2) {
			opt.o_src = argv[optind++];
			opt.o_dst = NULL;
		} else if (argc == optind + 3) {
			opt.o_src = argv[optind++];
			opt.o_dst = argv[optind++];
		} else {
			CT_ERROR("--rebind requires 1 or 2 arguments\n");
			return -EINVAL;
		}
		break;
	case CA_MAXSEQ:
	default:
		/* just mount point */
		break;
	}

	if (argc != optind + 1) {
		CT_ERROR("no mount point specified\n");
		return -EINVAL;
	}

	opt.o_mnt = argv[optind];

	CT_TRACE("action=%d src=%s dst=%s mount_point=%s\n",
		 opt.o_action, opt.o_src, opt.o_dst, opt.o_mnt);

	if (!opt.o_dry_run && opt.o_hsm_root == NULL) {
		CT_ERROR("must specify a HSM root\n");
		return -EINVAL;
	}

	if (opt.o_action == CA_IMPORT) {
		if (opt.o_src && opt.o_src[0] == '/') {
			CT_ERROR("source path must be relative to HSM root.\n");
			return -EINVAL;
		}

		if (opt.o_dst && opt.o_dst[0] != '/') {
			CT_ERROR("destination path must be absolute.\n");
			return -EINVAL;
		}
	}

	return 0;
}

/* mkdir -p path */
static int ct_mkdir_p(const char *path)
{
	char	*saved, *ptr;
	int	 rc;

	ptr = strdup(path);
	saved = ptr;
	while (*ptr == '/')
		ptr++;

	while ((ptr = strchr(ptr, '/')) != NULL) {
		*ptr = '\0';
		rc = mkdir(saved, DIR_PERM);
		*ptr = '/';
		if (rc < 0 && errno != EEXIST) {
			CT_ERROR("'%s' mkdir failed (%s)\n", path,
				 strerror(errno));
			free(saved);
			return -errno;
		}
		ptr++;
	}

	free(saved);

	return 0;
}

static int ct_save_stripe(int src_fd, const char *src, const char *dst)
{
	char			 lov_file[PATH_MAX];
	char			 lov_buf[XATTR_SIZE_MAX];
	struct lov_user_md	*lum;
	int			 rc;
	int			 fd;
	ssize_t			 xattr_size;

	snprintf(lov_file, sizeof(lov_file), "%s.lov", dst);
	CT_TRACE("saving stripe info of '%s' in %s\n", src, lov_file);

	xattr_size = fgetxattr(src_fd, XATTR_LUSTRE_LOV, lov_buf,
			       sizeof(lov_buf));
	if (xattr_size < 0) {
		CT_ERROR("'%s' cannot get stripe info on (%s)\n", src,
			 strerror(errno));
		return -errno;
	}

	lum = (struct lov_user_md *)lov_buf;

	if (lum->lmm_magic == LOV_USER_MAGIC_V1 ||
	    lum->lmm_magic == LOV_USER_MAGIC_V3) {
		/* Set stripe_offset to -1 so that it is not interpreted as a
		 * hint on restore. */
		lum->lmm_stripe_offset = -1;
	}

	fd = open(lov_file, O_TRUNC | O_CREAT | O_WRONLY, FILE_PERM);
	if (fd < 0) {
		CT_ERROR("'%s' cannot open (%s)\n", lov_file, strerror(errno));
		return -errno;
	}

	rc = write(fd, lum, xattr_size);
	if (rc < 0) {
		CT_ERROR("'%s' cannot write %d bytes (%s)\n",
			 lov_file, xattr_size, strerror(errno));
		close(fd);
		return -errno;
	}

	rc = close(fd);
	if (rc < 0) {
		CT_ERROR("'%s' cannot close (%s)\n", lov_file, strerror(errno));
		return -errno;
	}

	return 0;
}

static int ct_load_stripe(const char *src, struct lov_user_md_v3 *lum,
			  size_t *lum_size)
{
	char	 lov_file[PATH_MAX];
	int	 rc;
	int	 fd;

	snprintf(lov_file, sizeof(lov_file), "%s.lov", src);
	CT_TRACE("reading stripe rules from '%s' for '%s'\n", lov_file, src);

	fd = open(lov_file, O_RDONLY);
	if (fd < 0) {
		CT_ERROR("'%s' cannot open (%s)\n", lov_file, strerror(errno));
		return -ENODATA;
	}

	rc = read(fd, lum, *lum_size);
	if (rc < 0) {
		CT_ERROR("'%s' cannot read %lu bytes (%s)\n", lov_file,
			 lum_size, strerror(errno));
		close(fd);
		return -ENODATA;
	}

	*lum_size = (size_t)rc;
	close(fd);

	return 0;
}

static int ct_restore_stripe(const char *src, const char *dst, int dst_fd)
{
	int			 rc;
	char			 lov_buf[XATTR_SIZE_MAX];
	size_t			 lum_size = sizeof(lov_buf);

	rc = ct_load_stripe(src, (struct lov_user_md_v3 *)lov_buf, &lum_size);
	if (rc) {
		CT_WARN("'%s' cannot get stripe rules (%s), use default\n",
			src, strerror(-rc));
		return 0;
	}

	rc = fsetxattr(dst_fd, XATTR_LUSTRE_LOV, lov_buf, lum_size, XATTR_CREATE);
	if (rc < 0) {
		CT_ERROR("'%s' cannot set striping (%s)\n",
			 dst, strerror(errno));
		return -errno;
	}

	return 0;
}

/* non-blocking read or write */
static int nonblock_rw(bool wr, int fd, char *buf, int size)
{
	int rc;

	if (wr)
		rc = write(fd, buf, size);
	else
		rc = read(fd, buf, size);

	if ((rc < 0) && (errno == -EAGAIN)) {
		fd_set set;
		struct timeval timeout;

		timeout.tv_sec = opt.o_report_int;

		FD_ZERO(&set);
		FD_SET(fd, &set);
		if (wr)
			rc = select(FD_SETSIZE, NULL, &set, NULL, &timeout);
		else
			rc = select(FD_SETSIZE, &set, NULL, NULL, &timeout);
		if (rc < 0)
			return -errno;
		if (rc == 0)
			/* Timed out, we read nothing */
			return -EAGAIN;

		/* Should be available now */
		if (wr)
			rc = write(fd, buf, size);
		else
			rc = read(fd, buf, size);
	}

	if (rc < 0)
		rc = -errno;

	return rc;
}

static int ct_copy_data(struct hsm_copyaction_private *hcp, const char *src,
			const char *dst, int src_fd, int dst_fd,
			const struct hsm_action_item *hai, long hal_flags)
{
	struct hsm_extent	 he;
	struct stat		 src_st;
	struct stat		 dst_st;
	char			*buf;
	__u64			 wpos = 0;
	__u64			 rpos = 0;
	__u64			 rlen;
	time_t			 last_print_time = time(0);
	int			 rsize;
	int			 wsize;
	int			 bufoff = 0;
	int			 rc = 0;

	CT_TRACE("going to copy data from '%s' to %s\n", src, dst);

	buf = malloc(opt.o_chunk_size);
	if (buf == NULL)
		return -ENOMEM;

	if (fstat(src_fd, &src_st) < 0) {
		CT_ERROR("'%s' stat failed (%s)\n", src, strerror(errno));
		return -errno;
	}

	if (!S_ISREG(src_st.st_mode)) {
		CT_ERROR("'%s' not a regular file\n", src);
		return -EINVAL;
	}

	rc = lseek(src_fd, hai->hai_extent.offset, SEEK_SET);
	if (rc < 0) {
		CT_ERROR("'%s' seek to read to "LPU64" (len %zu)"
			 " failed (%s)\n",
			 src, hai->hai_extent.offset, src_st.st_size,
			 strerror(errno));
		rc = -errno;
		goto out;
	}

	if (fstat(dst_fd, &dst_st) < 0) {
		CT_ERROR("'%s' stat failed (%s)\n", dst, strerror(errno));
		return -errno;
	}

	if (!S_ISREG(dst_st.st_mode)) {
		CT_ERROR("'%s' not a regular file\n", dst);
		return -EINVAL;
	}

	rc = lseek(dst_fd, hai->hai_extent.offset, SEEK_SET);
	if (rc < 0) {
		CT_ERROR("'%s' seek to write to "LPU64" failed (%s)\n", src,
			 hai->hai_extent.offset, strerror(errno));
		rc = -errno;
		goto out;
	}

	he.offset = hai->hai_extent.offset;
	he.length = 0;
	rc = llapi_hsm_action_progress(hcp, &he, 0);
	if (rc) {
		/* Action has been canceled or something wrong
		 * is happening. Stop copying data. */
		CT_ERROR("%s->'%s' progress returned err %d\n", src, dst, rc);
		goto out;
	}

	errno = 0;
	/* Don't read beyond a given extent */
	rlen = (hai->hai_extent.length == -1LL) ?
		src_st.st_size : hai->hai_extent.length;

	while (wpos < rlen) {
		int chunk = (rlen - wpos > opt.o_chunk_size) ?
			    opt.o_chunk_size : rlen - wpos;

		/* Only read more if we wrote everything in the buffer */
		if (wpos == rpos) {
			rsize = nonblock_rw(0, src_fd, buf, chunk);
			if (rsize == 0)
				/* EOF */
				break;

			if (rsize == -EAGAIN) {
				/* Timed out */
				rsize = 0;
				if (rpos == 0) {
					/* Haven't read anything yet, let's
					 * give it back to the coordinator
					 * for rescheduling */
					rc = -EAGAIN;
					break;
				}
			}

			if (rsize < 0) {
				CT_ERROR("'%s' read failed (%s)\n", src,
					 strerror(-rsize));
				rc = rsize;
				break;
			}

			rpos += rsize;
			bufoff = 0;
		}

		wsize = nonblock_rw(1, dst_fd, buf + bufoff, rpos - wpos);
		if (wsize == -EAGAIN)
			/* Timed out */
			wsize = 0;

		if (wsize < 0) {
			CT_ERROR("'%s' write failed (%s)\n", dst,
				 strerror(-wsize));
			rc = wsize;
			break;
		}

		wpos += wsize;
		bufoff += wsize;

		if (opt.o_bandwidth != 0) {
			static unsigned long long	tot_bytes;
			static time_t			start_time, last_time;
			time_t				now = time(0);
			double				tot_time, excess;
			unsigned int			sleep_time;

			if (now > last_time + 5) {
				tot_bytes = 0;
				start_time = last_time = now;
			}

			tot_bytes += wsize;
			tot_time = now - start_time;
			if (tot_time < 1)
				tot_time = 1;

			excess = tot_bytes - tot_time * opt.o_bandwidth;
			sleep_time = excess * 1000000 / opt.o_bandwidth;
			if ((now - start_time) % 10 == 1)
				CT_TRACE("bandwith control: excess=%E"
					 " sleep for %dus\n",
					 excess, sleep_time);

			if (excess > 0)
				usleep(sleep_time);

			last_time = now;
		}

		if (time(0) >= last_print_time + opt.o_report_int) {
			last_print_time = time(0);
			CT_TRACE("%%"LPU64" ", 100 * wpos / rlen);
			he.length = wpos;
			rc = llapi_hsm_action_progress(hcp, &he, 0);
			if (rc) {
				/* Action has been canceled or something wrong
				 * is happening. Stop copying data. */
				CT_ERROR("%s->'%s' progress returned err %d\n",
					 src, dst, rc);
				goto out;
			}
		}
		rc = 0;
	}
	CT_TRACE("\n");

out:
	/*
	 * truncate restored file
	 * size is taken from the archive this is done to support
	 * restore after a force release which leaves the file with the
	 * wrong size (can big bigger than the new size)
	 */
	if ((hai->hai_action == HSMA_RESTORE) &&
	    (src_st.st_size < dst_st.st_size)) {
		/*
		 * make sure the file is on disk before reporting success.
		 */
		rc = ftruncate(dst_fd, src_st.st_size);
		if (rc < 0) {
			rc = -errno;
			CT_ERROR("'%s' final truncate to %lu failed (%s)\n",
				 dst, src_st.st_size, strerror(-rc));
			err_major++;
		}
	}

	if (rc == 0) {
		rc = fsync(dst_fd);
		if (rc < 0) {
			rc = -errno;
			CT_ERROR("'%s' fsync failed (%s)\n", dst,
				 strerror(-rc));
			err_major++;
		}
	}

	free(buf);

	return rc;
}

/* Copy file attributes from file src to file dest */
static int ct_copy_attr(const char *src, const char *dst, int src_fd,
			int dst_fd)
{
	struct stat	st;
	struct timeval	times[2];

	if (fstat(src_fd, &st) < 0) {
		CT_ERROR("'%s' stat failed (%s)\n",
		     src, strerror(errno));
		return -errno;
	}

	times[0].tv_sec = st.st_atime;
	times[0].tv_usec = 0;
	times[1].tv_sec = st.st_mtime;
	times[1].tv_usec = 0;
	if (fchmod(dst_fd, st.st_mode) < 0 ||
	    fchown(dst_fd, st.st_uid, st.st_gid) < 0 ||
	    futimes(dst_fd, times) < 0)
		CT_ERROR("'%s' fchmod fchown or futimes failed (%s)\n", src,
			 strerror(errno));
		return -errno;
	return 0;
}

static int ct_copy_xattr(const char *src, const char *dst, int src_fd,
			 int dst_fd, bool is_restore)
{
	char	 list[XATTR_LIST_MAX];
	char	 value[XATTR_SIZE_MAX];
	char	*name;
	ssize_t	 list_len;
	int	 rc;

	list_len = flistxattr(src_fd, list, sizeof(list));
	if (list_len < 0)
		return -errno;

	name = list;
	while (name < list + list_len) {
		rc = fgetxattr(src_fd, name, value, sizeof(value));
		if (rc < 0)
			return -errno;

		/* when we restore, we do not restore lustre xattr */
		if (!is_restore ||
		    (strncmp(XATTR_TRUSTED_PREFIX, name,
			     sizeof(XATTR_TRUSTED_PREFIX) - 1) != 0)) {
			rc = fsetxattr(dst_fd, name, value, rc, 0);
			CT_TRACE("'%s' fsetxattr of '%s' rc=%d (%s)\n",
				 dst, name, rc, strerror(errno));
			/* lustre.* attrs aren't supported on other FS's */
			if (rc < 0 && errno != EOPNOTSUPP) {
				CT_ERROR("'%s' fsetxattr of '%s' failed (%s)\n",
					 dst, name, strerror(errno));
				return -errno;
			}
		}
		name += strlen(name) + 1;
	}

	return 0;
}

static int ct_path_lustre(char *buf, int sz, const char *mnt,
			  const lustre_fid *fid)
{
	return snprintf(buf, sz, "%s/%s/fid/"DFID_NOBRACE, mnt,
			dot_lustre_name, PFID(fid));
}

static int ct_path_archive(char *buf, int sz, const char *archive_dir,
			   const lustre_fid *fid)
{
	return snprintf(buf, sz, "%s/%04x/%04x/%04x/%04x/%04x/%04x/"
			DFID_NOBRACE, archive_dir,
			(fid)->f_oid       & 0xFFFF,
			(fid)->f_oid >> 16 & 0xFFFF,
			(unsigned int)((fid)->f_seq       & 0xFFFF),
			(unsigned int)((fid)->f_seq >> 16 & 0xFFFF),
			(unsigned int)((fid)->f_seq >> 32 & 0xFFFF),
			(unsigned int)((fid)->f_seq >> 48 & 0xFFFF),
			PFID(fid));
}

static bool ct_is_retryable(int err)
{
	return err == -ETIMEDOUT;
}

static int ct_begin(struct hsm_copyaction_private **phcp,
		    const struct hsm_action_item *hai)
{
	char	 src[PATH_MAX];
	int	 rc;

	rc = llapi_hsm_action_begin(phcp, ctdata, hai, false);
	if (rc < 0) {
		ct_path_lustre(src, sizeof(src), opt.o_mnt, &hai->hai_fid);
		CT_ERROR("'%s' copy start failed (%s)\n", src, strerror(-rc));
	}

	return rc;
}

static int ct_fini(struct hsm_copyaction_private **phcp,
		   const struct hsm_action_item *hai, int flags, int ct_rc)
{
	char	lstr[PATH_MAX];
	int	rc;

	CT_TRACE("Action completed, notifying coordinator "
		 "cookie="LPX64", FID="DFID", flags=%d err=%d\n",
		 hai->hai_cookie, PFID(&hai->hai_fid),
		 flags, -ct_rc);

	ct_path_lustre(lstr, sizeof(lstr), opt.o_mnt, &hai->hai_fid);
	rc = llapi_hsm_action_end(phcp, &hai->hai_extent, flags, abs(ct_rc));
	if (rc == -ECANCELED)
		CT_ERROR("'%s' completed action has been canceled: "
			 "cookie="LPX64", FID="DFID"\n", lstr, hai->hai_cookie,
			 PFID(&hai->hai_fid));
	else if (rc < 0)
		CT_ERROR("'%s' copy end failed (%s)\n", lstr, strerror(-rc));
	else
		CT_TRACE("'%s' copy end ok (rc=%d)\n", lstr, rc);

	return rc;
}

static int ct_archive(const struct hsm_action_item *hai, const long hal_flags)
{
	struct hsm_copyaction_private	*hcp = NULL;
	char				 src[PATH_MAX];
	char				 dst[PATH_MAX];
	int				 rc;
	int				 rcf = 0;
	bool				 rename_needed = false;
	int				 ct_flags = 0;
	int				 open_flags;
	int				 src_fd = -1;
	int				 dst_fd = -1;

	rc = ct_begin(&hcp, hai);
	if (rc < 0)
		goto fini_major;

	/* we fill archive so:
	 * source = data FID
	 * destination = lustre FID
	 */
	ct_path_lustre(src, sizeof(src), opt.o_mnt, &hai->hai_dfid);
	ct_path_archive(dst, sizeof(dst), opt.o_hsm_root, &hai->hai_fid);
	if (hai->hai_extent.length == -1) {
		/* whole file, write it to tmp location and atomically
		 * replace old archived file */
		strncat(dst, "_tmp", sizeof(dst) - strlen(dst) - 1);
		/* we cannot rely on the same test because ct_copy_data()
		 * updates hai_extent.length */
		rename_needed = true;
	}

	CT_TRACE("'%s' archived to %s\n", src, dst);

	if (opt.o_dry_run) {
		rc = 0;
		goto fini_major;
	}

	rc = ct_mkdir_p(dst);
	if (rc < 0) {
		CT_ERROR("'%s' mkdir_p failed (%s)\n", dst, strerror(-rc));
		goto fini_major;
	}

	src_fd = open(src, O_RDONLY | O_NOATIME | O_NONBLOCK | O_NOFOLLOW);
	if (src_fd == -1) {
		CT_ERROR("'%s' open read failed (%s)\n", src, strerror(errno));
		rc = -errno;
		goto fini_major;
	}

	open_flags = O_WRONLY | O_NOFOLLOW | O_NONBLOCK;
	/* If extent is specified, don't truncate an old archived copy */
	open_flags |= ((hai->hai_extent.length == -1) ? O_TRUNC : 0) | O_CREAT;

	dst_fd = open(dst, open_flags, FILE_PERM);
	if (dst_fd == -1) {
		CT_ERROR("'%s' open write failed (%s)\n", dst, strerror(errno));
		rc = -errno;
		goto fini_major;
	}

	/* saving stripe is not critical */
	rc = ct_save_stripe(src_fd, src, dst);
	if (rc < 0)
		CT_ERROR("'%s' cannot save file striping info in '%s' (%s)\n",
			 src, dst, strerror(-rc));

	rc = ct_copy_data(hcp, src, dst, src_fd, dst_fd, hai, hal_flags);
	if (rc < 0) {
		CT_ERROR("'%s' data copy failed to '%s' (%s)\n",
			 src, dst, strerror(-rc));
		goto fini_major;
	}

	CT_TRACE("'%s' data archived to '%s' done\n", src, dst);

	/* attrs will remain on the MDS; no need to copy them, except possibly
	  for disaster recovery */
	if (opt.o_copy_attrs) {
		rc = ct_copy_attr(src, dst, src_fd, dst_fd);
		if (rc < 0) {
			CT_ERROR("'%s' attr copy failed to '%s' (%s)\n",
				 src, dst, strerror(-rc));
			rcf = rc;
		}
		CT_TRACE("'%s' attr file copied to archive '%s'\n",
			 src, dst);
	}

	/* xattrs will remain on the MDS; no need to copy them, except possibly
	 for disaster recovery */
	if (opt.o_copy_xattrs) {
		rc = ct_copy_xattr(src, dst, src_fd, dst_fd, false);
		if (rc < 0) {
			CT_ERROR("'%s' xattr copy failed to '%s' (%s)\n",
				 src, dst, strerror(-rc));
			rcf = rcf ? rcf : rc;
		}
		CT_ERROR("'%s' xattr file copied to archive '%s'\n",
			 src, dst);
	}

	if (rename_needed == true) {
		char	 tmp_src[PATH_MAX];
		char	 tmp_dst[PATH_MAX];

		/* atomically replace old archived file */
		ct_path_archive(src, sizeof(src), opt.o_hsm_root,
				&hai->hai_fid);
		rc = rename(dst, src);
		if (rc < 0) {
			CT_ERROR("'%s' renamed to '%s' failed (%s)\n", dst, src,
				 strerror(errno));
			rc = -errno;
			goto fini_major;
		}
		/* rename lov file */
		snprintf(tmp_src, sizeof(tmp_src), "%s.lov", src);
		snprintf(tmp_dst, sizeof(tmp_dst), "%s.lov", dst);
		rc = rename(tmp_dst, tmp_src);
		if (rc < 0)
			CT_ERROR("'%s' renamed to '%s' failed (%s)\n",
				 tmp_dst, tmp_src, strerror(errno));
	}

	if (opt.o_shadow_tree) {
		/* Create a namespace of softlinks that shadows the original
		 * Lustre namespace.  This will only be current at
		 * time-of-archive (won't follow renames).
		 * WARNING: release won't kill these links; a manual
		 * cleanup of dead links would be required.
		 */
		char		 buf[PATH_MAX];
		long long	 recno = -1;
		int		 linkno = 0;
		char		*ptr;
		int		 depth = 0;
		int		 sz;

		sprintf(buf, DFID, PFID(&hai->hai_fid));
		sprintf(src, "%s/shadow/", opt.o_hsm_root);

		ptr = opt.o_hsm_root;
		while (*ptr)
			(*ptr++ == '/') ? depth-- : 0;

		rc = llapi_fid2path(opt.o_mnt, buf, src + strlen(src),
				    sizeof(src) - strlen(src), &recno, &linkno);
		if (rc < 0) {
			CT_ERROR("'%s' fid2path failed (%s)\n", buf,
				 strerror(-rc));
			rcf = rcf ? rcf : rc;
			goto fini_minor;
		}

		/* Figure out how many parent dirs to symlink back */
		ptr = src;
		while (*ptr)
			(*ptr++ == '/') ? depth++ : 0;
		sprintf(buf, "..");
		while (--depth > 1)
			strcat(buf, "/..");

		ct_path_archive(dst, sizeof(dst), buf, &hai->hai_fid);

		if (ct_mkdir_p(src)) {
			CT_ERROR("'%s' mkdir_p failed (%s)\n", src,
				 strerror(errno));
			rcf = rcf ? rcf : -errno;
			goto fini_minor;
		}
		/* symlink already exists ? */
		sz = readlink(src, buf, sizeof(buf));
		if (sz >= 0) {
			buf[sz] = '\0';
			if (sz == 0 || strncmp(buf, dst, sz) != 0) {
				if (unlink(src) && errno != ENOENT) {
					CT_ERROR("'%s' unlink symlink failed "
						 "(%s)\n", src,
						 strerror(errno));
					rcf = rcf ? rcf : -errno;
					goto fini_minor;
				/* unlink old symlink done */
				CT_TRACE("'%s' remove old symlink pointing"
					 " to '%s'\n", src, buf);
				}
			} else {
				/* symlink already ok */
				CT_TRACE("'%s' symlink already pointing"
					 " to '%s'\n", src, dst);
				rcf = 0;
				goto fini_minor;
			}
		}
		if (symlink(dst, src)) {
			CT_ERROR("'%s' symlink to '%s' failed (%s)\n", src, dst,
				 strerror(errno));
			rcf = rcf ? rcf : -errno;
			goto fini_minor;
		}
		CT_TRACE("'%s' symlink to '%s' done\n", src, dst);
	}
fini_minor:
	if (rcf)
		err_minor++;
	goto out;


fini_major:
	err_major++;

	unlink(dst);
	if (ct_is_retryable(rc))
		ct_flags |= HP_FLAG_RETRY;

	rcf = rc;

out:
	if (!(src_fd < 0))
		close(src_fd);

	if (!(dst_fd < 0))
		close(dst_fd);

	if (hcp != NULL)
		rc = ct_fini(&hcp, hai, ct_flags, rcf);

	return rc;
}

static int ct_restore(const struct hsm_action_item *hai, const long hal_flags)
{
	struct hsm_copyaction_private	*hcp = NULL;
	char				 src[PATH_MAX];
	char				 dst[PATH_MAX];
	int				 rc;
	int				 flags = 0;
	int				 src_fd = -1;
	int				 dst_fd = -1;
	lustre_fid			 dfid;

	rc = ct_begin(&hcp, hai);
	if (rc)
		goto fini;

	/* we fill lustre so:
	 * source = lustre FID in the backend
	 * destination = data FID = volatile file
	 */

	/* build backend file name from released file FID */
	ct_path_archive(src, sizeof(src), opt.o_hsm_root, &hai->hai_fid);

	/* get the FID of the volatile file */
	rc = llapi_hsm_action_get_dfid(hcp, &dfid);
	if (rc < 0) {
		CT_ERROR("restoring "DFID", cannot get FID of created volatile"
			 " file (%s)\n", PFID(&hai->hai_fid), strerror(-rc));
		goto fini;
	}

	/* build volatile "file name", for messages */
	snprintf(dst, sizeof(dst), "{VOLATILE}="DFID, PFID(&dfid));

	CT_TRACE("'%s' restore data to '%s'\n", src, dst);

	if (opt.o_dry_run) {
		rc = 0;
		goto fini;
	}

	src_fd = open(src, O_RDONLY | O_NOATIME | O_NONBLOCK | O_NOFOLLOW);
	if (src_fd < 0) {
		CT_ERROR("'%s' open for read failed (%s)\n", src,
			 strerror(errno));
		rc = -errno;
		goto fini;
	}

	dst_fd = llapi_hsm_action_get_fd(hcp);

	/* the layout cannot be allocated through .fid so we have to
	 * restore a layout */
	rc = ct_restore_stripe(src, dst, dst_fd);
	if (rc) {
		CT_ERROR("'%s' cannot restore file striping info from '%s'"
			 " (%s)\n", dst, src, strerror(-rc));
		err_major++;
		goto fini;
	}

	rc = ct_copy_data(hcp, src, dst, src_fd, dst_fd, hai, hal_flags);
	if (rc < 0) {
		CT_ERROR("'%s' data copy to '%s' failed (%s)\n", src, dst,
			 strerror(-rc));
		err_major++;
		if (ct_is_retryable(rc))
			flags |= HP_FLAG_RETRY;
		goto fini;
	}

	CT_TRACE("'%s' data restore done to %s\n", src, dst);

fini:
	if (hcp != NULL)
		rc = ct_fini(&hcp, hai, flags, rc);

	/* object swaping is done by cdt at copy end, so close of volatile file
	 * cannot be done before */
	if (!(src_fd < 0))
		close(src_fd);

	if (!(dst_fd < 0))
		close(dst_fd);

	return rc;
}

static int ct_remove(const struct hsm_action_item *hai, const long hal_flags)
{
	struct hsm_copyaction_private	*hcp = NULL;
	char				 dst[PATH_MAX];
	int				 rc;

	rc = ct_begin(&hcp, hai);
	if (rc < 0)
		goto fini;

	ct_path_archive(dst, sizeof(dst), opt.o_hsm_root, &hai->hai_fid);

	CT_TRACE("'%s' removed file\n", dst);

	if (opt.o_dry_run) {
		rc = 0;
		goto fini;
	}

	rc = unlink(dst);
	if (rc < 0) {
		rc = -errno;
		CT_ERROR("'%s' unlink failed (%s)\n", dst, strerror(-rc));
		err_minor++;
		goto fini;
	}

fini:
	if (hcp != NULL)
		rc = ct_fini(&hcp, hai, 0, rc);

	return rc;
}

static int ct_report_error(const struct hsm_action_item *hai, int flags,
			   int errval)
{
	struct hsm_copyaction_private	*hcp;
	int				 rc;

	rc = llapi_hsm_action_begin(&hcp, ctdata, hai, true);
	if (rc)
		return rc;

	rc = llapi_hsm_action_end(&hcp, &hai->hai_extent, flags, abs(errval));

	return rc;
}

static int ct_process_item(struct hsm_action_item *hai, const long hal_flags)
{
	int	rc = 0;

	if (opt.o_verbose >= LLAPI_MSG_INFO || opt.o_dry_run) {
		/* Print the original path */
		char		fid[128];
		char		path[PATH_MAX];
		long long	recno = -1;
		int		linkno = 0;

		sprintf(fid, DFID, PFID(&hai->hai_fid));
		CT_TRACE("'%s' action %s reclen %d, cookie="LPX64"\n",
			 fid, hsm_copytool_action2name(hai->hai_action),
			 hai->hai_len, hai->hai_cookie);
		rc = llapi_fid2path(opt.o_mnt, fid, path,
				    sizeof(path), &recno, &linkno);
		if (rc < 0)
			CT_ERROR("'%s' fid2path failed (%s)\n", fid,
				 strerror(-rc));
		else
			CT_TRACE("'%s' processing file\n", path);
	}

	switch (hai->hai_action) {
	/* set err_major, minor inside these functions */
	case HSMA_ARCHIVE:
		rc = ct_archive(hai, hal_flags);
		break;
	case HSMA_RESTORE:
		rc = ct_restore(hai, hal_flags);
		break;
	case HSMA_REMOVE:
		rc = ct_remove(hai, hal_flags);
		break;
	case HSMA_CANCEL:
		CT_TRACE("'%s' cancel not implemented\n", opt.o_mnt);
		/* Don't report progress to coordinator for this cookie:
		 * the copy function will get ECANCELED when reporting
		 * progress. */
		err_minor++;
		return 0;
		break;
	default:
		CT_ERROR("'%s' unknown action %d\n", opt.o_mnt,
			 hai->hai_action);
		err_minor++;
		ct_report_error(hai, 0, -EINVAL);
	}

	return 0;
}

struct ct_th_data {
	long			 hal_flags;
	struct hsm_action_item	*hai;
};

static void *ct_thread(void *data)
{
	struct ct_th_data *cttd = data;
	int rc;

	rc = ct_process_item(cttd->hai, cttd->hal_flags);

	free(cttd->hai);
	free(cttd);
	pthread_exit((void *)(intptr_t)rc);
}

static int ct_process_item_async(const struct hsm_action_item *hai,
				 long hal_flags)
{
	pthread_attr_t		 attr;
	pthread_t		 thread;
	struct ct_th_data	*data;
	int			 rc;

	data = malloc(sizeof(*data));
	if (data == NULL)
		return -ENOMEM;

	data->hai = malloc(hai->hai_len);
	if (data->hai == NULL) {
		free(data);
		return -ENOMEM;
	}

	memcpy(data->hai, hai, hai->hai_len);
	data->hal_flags = hal_flags;

	rc = pthread_attr_init(&attr);
	if (rc != 0) {
		CT_ERROR("'%s' pthread_attr_init: %s\n", opt.o_mnt,
			 strerror(rc));
		free(data->hai);
		free(data);
		return -rc;
	}

	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	rc = pthread_create(&thread, &attr, ct_thread, data);
	if (rc != 0)
		CT_ERROR("'%s' thread create: (%s)\n", opt.o_mnt, strerror(rc));

	pthread_attr_destroy(&attr);
	return 0;
}

static int ct_import_one(const char *src, const char *dst)
{
	char		newarc[PATH_MAX];
	lustre_fid	fid;
	struct stat	st;
	int		rc;

	CT_TRACE("'%s' importing from %s\n", dst, src);

	if (stat(src, &st) < 0) {
		CT_ERROR("'%s' stat failed (%s)\n", src, strerror(errno));
		return -errno;
	}

	if (opt.o_dry_run)
		return 0;

	rc = llapi_hsm_import(dst,
			      opt.o_archive_cnt ? opt.o_archive_id[0] : 0,
			      &st, 0, 0, 0, 0, NULL, &fid);
	if (rc < 0) {
		CT_ERROR("'%s' import from '%s' failed (%s)\n", dst, src,
			 strerror(-rc));
		return -rc;
	}

	ct_path_archive(newarc, sizeof(newarc), opt.o_hsm_root, &fid);

	rc = ct_mkdir_p(newarc);
	if (rc < 0) {
		CT_ERROR("'%s' mkdir_p failed (%s)\n", newarc, strerror(-rc));
		err_major++;
		return rc;

	}

	/* Lots of choices now: mv, ln, ln -s ? */
	rc = link(src, newarc); /* hardlink */
	if (rc < 0) {
		CT_ERROR("'%s' link to '%s' failed (%s)\n", newarc, src,
			 strerror(errno));
		err_major++;
		return -errno;
	}
	CT_TRACE("'%s' imported from '%s'=='%s'\n", dst, newarc, src);

	return 0;
}

static char *path_concat(const char *dirname, const char *basename)
{
	char	*result;
	int	 dirlen = strlen(dirname);

	result = malloc(dirlen + strlen(basename) + 2);
	if (result == NULL)
		return NULL;

	memcpy(result, dirname, dirlen);
	result[dirlen] = '/';
	strcpy(result + dirlen + 1, basename);

	return result;
}

static int ct_import_recurse(const char *relpath)
{
	DIR		*dir;
	struct dirent	 ent, *cookie = NULL;
	char		*srcpath, *newpath;
	int		 rc;

	if (relpath == NULL)
		return -EINVAL;

	srcpath = path_concat(opt.o_hsm_root, relpath);
	if (srcpath == NULL) {
		err_major++;
		return -ENOMEM;
	}

	dir = opendir(srcpath);
	if (dir == NULL) {
		/* Not a dir, or error */
		if (errno == ENOTDIR) {
			/* Single regular file case, treat o_dst as absolute
			   final location. */
			rc = ct_import_one(srcpath, opt.o_dst);
		} else {
			CT_ERROR("'%s' opendir failed (%s)\n", srcpath,
				 strerror(errno));
			err_major++;
			rc = -errno;
		}
		free(srcpath);
		return rc;
	}
	free(srcpath);

	while (1) {
		rc = readdir_r(dir, &ent, &cookie);
		if (rc != 0) {
			CT_ERROR("'%s' readdir_r failed (%s)\n", relpath,
				 strerror(errno));
			err_major++;
			rc = -errno;
			goto out;
		} else if ((rc == 0) && (cookie == NULL)) {
			/* end of directory */
			break;
		}

		if (!strcmp(ent.d_name, ".") ||
		    !strcmp(ent.d_name, ".."))
			continue;

		/* New relative path */
		newpath = path_concat(relpath, ent.d_name);
		if (newpath == NULL) {
			err_major++;
			rc = -ENOMEM;
			goto out;
		}

		if (ent.d_type == DT_DIR) {
			rc = ct_import_recurse(newpath);
		} else {
			char src[PATH_MAX];
			char dst[PATH_MAX];

			sprintf(src, "%s/%s", opt.o_hsm_root, newpath);
			sprintf(dst, "%s/%s", opt.o_dst, newpath);
			/* Make the target dir in the Lustre fs */
			rc = ct_mkdir_p(dst);
			if (rc == 0) {
				/* Import the file */
				rc = ct_import_one(src, dst);
			} else {
				CT_ERROR("'%s' ct_mkdir_p failed (%s)\n", dst,
					 strerror(-rc));
				err_major++;
			}
		}

		if (rc != 0) {
			CT_ERROR("'%s' importing failed\n", newpath);
			if (err_major && opt.o_abort_on_error) {
				free(newpath);
				goto out;
			}
		}
		free(newpath);
	}

	rc = 0;
out:
	closedir(dir);
	return rc;
}

static int ct_rebind_one(const lustre_fid *old_fid, const lustre_fid *new_fid)
{
	char src[PATH_MAX];
	char dst[PATH_MAX];

	CT_TRACE("rebind "DFID" to "DFID"\n", PFID(old_fid), PFID(new_fid));

	ct_path_archive(src, sizeof(src), opt.o_hsm_root, old_fid);
	ct_path_archive(dst, sizeof(dst), opt.o_hsm_root, new_fid);

	if (!opt.o_dry_run) {
		ct_mkdir_p(dst);
		if (rename(src, dst)) {
			CT_ERROR("'%s' rename to '%s' failed (%s)\n", src, dst,
				 strerror(errno));
			return -errno;
		}
		/* rename lov file */
		strncat(src, ".lov", sizeof(src) - strlen(src) - 1);
		strncat(dst, ".lov", sizeof(dst) - strlen(dst) - 1);
		if (rename(src, dst))
			CT_ERROR("'%s' rename to '%s' failed (%s)\n", src, dst,
				 strerror(errno));

	}
	return 0;
}

static bool fid_is_file(lustre_fid *fid)
{
	return fid_is_norm(fid) || fid_is_igif(fid);
}

static bool should_ignore_line(const char *line)
{
	int	i;

	for (i = 0; line[i] != '\0'; i++) {
		if (isspace(line[i]))
			continue;
		else if (line[i] == '#')
			return true;
		else
			return false;
	}

	return true;
}

static int ct_rebind_list(const char *list)
{
	int		 rc;
	FILE		*filp;
	ssize_t		 r;
	char		*line = NULL;
	size_t		 line_size = 0;
	unsigned int	 nl = 0;
	unsigned int	 ok = 0;

	filp = fopen(list, "r");
	if (filp == NULL) {
		CT_ERROR("'%s' open failed (%s)\n", list, strerror(errno));
		return -errno;
	}

	/* each line consists of 2 FID */
	while ((r = getline(&line, &line_size, filp)) != -1) {
		lustre_fid	old_fid;
		lustre_fid	new_fid;

		/* Ignore empty and commented out ('#...') lines. */
		if (should_ignore_line(line))
			continue;

		nl++;

		rc = sscanf(line, SFID" "SFID, RFID(&old_fid), RFID(&new_fid));
		if (rc != 6 || !fid_is_file(&old_fid) ||
		    !fid_is_file(&new_fid)) {
			CT_ERROR("'%s' FID expected near '%s', line %u\n",
				 list, line, nl);
			err_major++;
			continue;
		}

		if (ct_rebind_one(&old_fid, &new_fid))
			err_major++;
		else
			ok++;
	}

	fclose(filp);

	if (line)
		free(line);

	/* return 0 if all rebinds were sucessful */
	CT_TRACE("'%s' %u lines read, %u rebind successful\n", list, nl, ok);

	return ok == nl ? 0 : -1;
}

static int ct_rebind(void)
{
	int	rc;

	if (opt.o_dst) {
		lustre_fid	old_fid;
		lustre_fid	new_fid;

		if (sscanf(opt.o_src, SFID, RFID(&old_fid)) != 3 ||
		    !fid_is_file(&old_fid)) {
			CT_ERROR("'%s' invalid FID format\n", opt.o_src);
			return -EINVAL;
		}

		if (sscanf(opt.o_dst, SFID, RFID(&new_fid)) != 3 ||
		    !fid_is_file(&new_fid)) {
			CT_ERROR("'%s' invalid FID format\n", opt.o_dst);
			return -EINVAL;
		}

		rc = ct_rebind_one(&old_fid, &new_fid);

		return rc;
	}

	/* o_src is a list file */
	rc = ct_rebind_list(opt.o_src);

	return rc;
}

static int ct_dir_level_max(const char *dirpath, __u16 *sub_seqmax)
{
	DIR		*dir;
	int		 rc;
	__u16		 sub_seq;
	struct dirent	 ent, *cookie = NULL;

	*sub_seqmax = 0;

	dir = opendir(dirpath);
	if (dir == NULL) {
		rc = -errno;
		CT_ERROR("'%s' failed to open directory (%s)\n", opt.o_hsm_root,
			 strerror(-rc));
		return rc;
	}

	while ((rc = readdir_r(dir, &ent, &cookie)) == 0) {
		if (cookie == NULL)
			/* end of directory.
			 * rc is 0 and seqmax contains the max value. */
			goto out;

		if (!strcmp(ent.d_name, ".") || !strcmp(ent.d_name, ".."))
			continue;

		if (sscanf(ent.d_name, "%hx", &sub_seq) != 1) {
			CT_TRACE("'%s' unexpected dirname format, "
				 "skip entry.\n", ent.d_name);
			continue;
		}
		if (sub_seq > *sub_seqmax)
			*sub_seqmax = sub_seq;
	}
	rc = -errno;
	CT_ERROR("'%s' readdir_r failed (%s)\n", dirpath, strerror(-rc));

out:
	closedir(dir);
	return rc;
}

static int ct_max_sequence(void)
{
	int   rc, i;
	char  path[PATH_MAX];
	__u64 seq = 0;
	__u16 subseq;

	strncpy(path, opt.o_hsm_root, sizeof(path));
	/* FID sequence is stored in top-level directory names:
	 * hsm_root/16bits (high weight)/16 bits/16 bits/16 bits (low weight).
	 */
	for (i = 0; i < 4; i++) {
		rc = ct_dir_level_max(path, &subseq);
		if (rc != 0)
			return rc;
		seq |= ((__u64)subseq << ((3 - i) * 16));
		sprintf(path + strlen(path), "/%04x", subseq);
	}

	printf("max_sequence: %016Lx\n", seq);

	return 0;
}

static void handler(int signal)
{
	psignal(signal, "exiting");
	/* If we don't clean up upon interrupt, umount thinks there's a ref
	 * and doesn't remove us from mtab (EINPROGRESS). The lustre client
	 * does successfully unmount and the mount is actually gone, but the
	 * mtab entry remains. So this just makes mtab happier. */
	llapi_hsm_copytool_unregister(&ctdata);
	_exit(1);
}

/* Daemon waits for messages from the kernel; run it in the background. */
static int ct_daemon(void)
{
	int	rc;

	rc = daemon(1, 1);
	if (rc < 0) {
		CT_ERROR("%d: cannot start as daemon (%s)", getpid(),
			 strerror(errno));
		return -errno;
	}

	rc = llapi_hsm_copytool_register(&ctdata, opt.o_mnt, 0,
					 opt.o_archive_cnt, opt.o_archive_id);
	if (rc < 0) {
		CT_ERROR("%d: cannot start copytool interface: %s\n", getpid(),
			 strerror(-rc));
		return rc;
	}

	signal(SIGINT, handler);
	signal(SIGTERM, handler);

	while (1) {
		struct hsm_action_list	*hal;
		struct hsm_action_item	*hai;
		int			 msgsize;
		int			 i = 0;

		CT_TRACE("%d: waiting for message from kernel\n", getpid());

		rc = llapi_hsm_copytool_recv(ctdata, &hal, &msgsize);
		if (rc == -ESHUTDOWN) {
			CT_TRACE("%d: shutting down", getpid());
			break;
		} else if (rc == -EAGAIN) {
			continue; /* msg not for us */
		} else if (rc < 0) {
			CT_WARN("%d: message receive: (%s)\n", getpid(),
				strerror(-rc));
			err_major++;
			if (opt.o_abort_on_error)
				break;
			else
				continue;
		}

		CT_TRACE("%d: copytool fs=%s archive#=%d item_count=%d\n",
			 getpid(), hal->hal_fsname, hal->hal_archive_id,
			 hal->hal_count);

		if (strcmp(hal->hal_fsname, fs_name) != 0) {
			CT_ERROR("'%s' invalid fs name, expecting: %s\n",
				 hal->hal_fsname, fs_name);
			err_major++;
			if (opt.o_abort_on_error)
				break;
			else
				continue;
		}

		hai = hai_first(hal);
		while (++i <= hal->hal_count) {
			if ((char *)hai - (char *)hal > msgsize) {
				CT_ERROR("'%s' item %d past end of message!\n",
					 opt.o_mnt, i);
				err_major++;
				rc = -EPROTO;
				break;
			}
			rc = ct_process_item_async(hai, hal->hal_flags);
			if (rc < 0)
				CT_ERROR("'%s' item %d process err: %s\n",
					 opt.o_mnt, i, strerror(-rc));
			if (opt.o_abort_on_error && err_major)
				break;
			hai = hai_next(hai);
		}

		llapi_hsm_action_list_free(&hal);

		if (opt.o_abort_on_error && err_major)
			break;
	}

	llapi_hsm_copytool_unregister(&ctdata);

	return rc;
}

static int ct_setup(void)
{
	int	rc;

	/* set llapi message level */
	llapi_msg_set_level(opt.o_verbose);

	arc_fd = open(opt.o_hsm_root, O_DIRECTORY);
	if (arc_fd < 0) {
		CT_ERROR("cannot open archive at '%s': %s\n", opt.o_hsm_root,
			 strerror(errno));
		return -errno;
	}

	rc = llapi_search_fsname(opt.o_mnt, fs_name);
	if (rc) {
		CT_ERROR("cannot find a Lustre filesystem mounted at: %s\n",
			 opt.o_mnt);
		return -rc;
	}

	return rc;
}

static int ct_cleanup(void)
{
	if (arc_fd < 0)
		return 0;

	if (close(arc_fd) < 0) {
		CT_ERROR("cannot close archive: %s.\n", strerror(errno));
		return -errno;
	}

	return 0;
}

int main(int argc, char **argv)
{
	int	rc;

	strncpy(cmd_name, basename(argv[0]), sizeof(cmd_name));
	rc = ct_parseopts(argc, argv);
	if (rc) {
		CT_ERROR("try '%s --help' for more information.\n", cmd_name);
		return -rc;
	}

	ct_setup();

	switch (opt.o_action) {
	case CA_IMPORT:
		rc = ct_import_recurse(opt.o_src);
		break;
	case CA_REBIND:
		rc = ct_rebind();
		break;
	case CA_MAXSEQ:
		rc = ct_max_sequence();
		break;
	case CA_DAEMON:
		rc = ct_daemon();
		break;
	default:
		CT_ERROR("no action specified. Try '%s --help' for more "
			 "information.\n", cmd_name);
		rc = -EINVAL;
		break;
	}

	if (opt.o_action != CA_MAXSEQ)
		CT_TRACE("%s(%d) finished, errs: %d major, %d minor, "
			 "rc=%d (%s)\n", argv[0], getpid(), err_major,
			 err_minor, rc, strerror(-rc));

	ct_cleanup();

	return -rc;
}

