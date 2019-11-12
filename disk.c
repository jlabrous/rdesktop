/* -*- c-basic-offset: 8 -*-
   rdesktop: A Remote Desktop Protocol client.
   Disk Redirection
   Copyright (C) Jeroen Meijer <jeroen@oldambt7.com> 2003-2008
   Copyright 2003-2011 Peter Astrand <astrand@cendio.se> for Cendio AB

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "disk.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>		/* open, close */
#include <dirent.h>		/* opendir, closedir, readdir */
#include <fnmatch.h>
#include <errno.h>		/* errno */
#include <stdio.h>

#include <utime.h>
#include <time.h>		/* ctime */

#if (defined(HAVE_DIRFD) || (HAVE_DECL_DIRFD == 1))
#define DIRFD(a) (dirfd(a))
#else
#define DIRFD(a) ((a)->DIR_FD_MEMBER_NAME)
#endif

/* TODO: Fix mntent-handling for solaris
 * #include <sys/mntent.h> */
#if (defined(HAVE_MNTENT_H) && defined(HAVE_SETMNTENT))
#include <mntent.h>
#define MNTENT_PATH "/etc/mtab"
#define USE_SETMNTENT
#endif

#ifdef HAVE_SYS_VFS_H
#include <sys/vfs.h>
#endif

#ifdef HAVE_SYS_STATVFS_H
#include <sys/statvfs.h>
#endif

#ifdef HAVE_SYS_STATFS_H
#include <sys/statfs.h>
#endif

#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif

#ifdef HAVE_SYS_MOUNT_H
#include <sys/mount.h>
#endif

#include "rdesktop.h"

#ifdef STAT_STATFS3_OSF1
#define STATFS_FN(path, buf) (statfs(path,buf,sizeof(buf)))
#define STATFS_T statfs
#define USE_STATFS
#endif

#ifdef STAT_STATVFS
#define STATFS_FN(path, buf) (statvfs(path,buf))
#define STATFS_T statvfs
#define USE_STATVFS
#endif

#ifdef STAT_STATVFS64
#define STATFS_FN(path, buf) (statvfs64(path,buf))
#define STATFS_T statvfs64
#define USE_STATVFS
#endif

#if (defined(STAT_STATFS2_FS_DATA) || defined(STAT_STATFS2_BSIZE) || defined(STAT_STATFS2_FSIZE))
#define STATFS_FN(path, buf) (statfs(path,buf))
#define STATFS_T statfs
#define USE_STATFS
#endif

#ifdef STAT_STATFS4
#define STATFS_FN(path, buf) (statfs(path,buf,sizeof(buf),0))
#define STATFS_T statfs
#define USE_STATFS
#endif

#if ((defined(USE_STATFS) && defined(HAVE_STRUCT_STATFS_F_NAMEMAX)) || (defined(USE_STATVFS) && defined(HAVE_STRUCT_STATVFS_F_NAMEMAX)))
#define F_NAMELEN(buf) ((buf).f_namemax)
#endif

#if ((defined(USE_STATFS) && defined(HAVE_STRUCT_STATFS_F_NAMELEN)) || (defined(USE_STATVFS) && defined(HAVE_STRUCT_STATVFS_F_NAMELEN)))
#define F_NAMELEN(buf) ((buf).f_namelen)
#endif

#ifndef F_NAMELEN
#define F_NAMELEN(buf) (255)
#endif

#ifdef HAVE_INOTIFY_H
#include <sys/inotify.h> 
#define INOTIFY_MSG_NUM 1024
#define INOTIFY_BUFF_SIZE ((sizeof(struct inotify_event)+FILENAME_MAX)*INOTIFY_MSG_NUM)
char inotify_buff[INOTIFY_BUFF_SIZE];
#endif

/* Dummy statfs fallback */
#ifndef STATFS_T
struct dummy_statfs_t
{
	long f_bfree;
	long f_bsize;
	long f_bavail;
	long f_blocks;
	int f_namelen;
	int f_namemax;
};

static int
dummy_statfs(struct dummy_statfs_t *buf)
{
	buf->f_blocks = 262144;
	buf->f_bfree = 131072;
	buf->f_bavail = 131072;
	buf->f_bsize = 512;
	buf->f_namelen = 255;
	buf->f_namemax = 255;

	return 0;
}

#define STATFS_T dummy_statfs_t
#define STATFS_FN(path,buf) (dummy_statfs(buf))
#endif

/* ChangeNotify flags. */
#define FILE_NOTIFY_CHANGE_FILE_NAME   0x001
#define FILE_NOTIFY_CHANGE_DIR_NAME    0x002
#define FILE_NOTIFY_CHANGE_ATTRIBUTES  0x004
#define FILE_NOTIFY_CHANGE_SIZE        0x008
#define FILE_NOTIFY_CHANGE_LAST_WRITE  0x010
#define FILE_NOTIFY_CHANGE_LAST_ACCESS 0x020
#define FILE_NOTIFY_CHANGE_CREATION    0x040
#define FILE_NOTIFY_CHANGE_EA          0x080
#define FILE_NOTIFY_CHANGE_SECURITY    0x100
#define FILE_NOTIFY_CHANGE_STREAM_NAME	0x00000200
#define FILE_NOTIFY_CHANGE_STREAM_SIZE	0x00000400
#define FILE_NOTIFY_CHANGE_STREAM_WRITE	0x00000800

#define FILE_NOTIFY_CHANGE_NAME \
	(FILE_NOTIFY_CHANGE_FILE_NAME|FILE_NOTIFY_CHANGE_DIR_NAME)

/* change notify action results */
#define NOTIFY_ACTION_ADDED 1
#define NOTIFY_ACTION_REMOVED 2
#define NOTIFY_ACTION_MODIFIED 3
#define NOTIFY_ACTION_OLD_NAME 4
#define NOTIFY_ACTION_NEW_NAME 5
#define NOTIFY_ACTION_ADDED_STREAM 6
#define NOTIFY_ACTION_REMOVED_STREAM 7
#define NOTIFY_ACTION_MODIFIED_STREAM 8

extern RDPDR_DEVICE g_rdpdr_device[];

FILEINFO g_fileinfo[MAX_OPEN_FILES];
RD_BOOL g_notify_stamp = False;

typedef struct
{
	char name[PATH_MAX];
	char label[PATH_MAX];
	unsigned long serial;
	char type[PATH_MAX];
} FsInfoType;

#ifndef HAVE_INOTIFY_H
static RD_NTSTATUS NotifyInfo(RD_NTHANDLE handle, uint32 info_class, NOTIFY * p);
#endif
static time_t
get_create_time(struct stat *filestat)
{
	time_t ret, ret1;

	ret = MIN(filestat->st_ctime, filestat->st_mtime);
	ret1 = MIN(ret, filestat->st_atime);

	if (ret1 != (time_t) 0)
		return ret1;

	return ret;
}

/* Convert seconds since 1970 to a filetime */
static void
seconds_since_1970_to_filetime(time_t seconds, uint32 * high, uint32 * low)
{
	unsigned long long ticks;

	ticks = (seconds + 11644473600LL) * 10000000;
	*low = (uint32) ticks;
	*high = (uint32) (ticks >> 32);
}

/* Convert seconds since 1970 back to filetime */
static time_t
convert_1970_to_filetime(uint32 high, uint32 low)
{
	unsigned long long ticks;
	time_t val;

	ticks = low + (((unsigned long long) high) << 32);
	ticks /= 10000000;
	ticks -= 11644473600LL;

	val = (time_t) ticks;
	return (val);

}

/* A wrapper for ftruncate which supports growing files, even if the
   native ftruncate doesn't. This is needed on Linux FAT filesystems,
   for example. */
static int
ftruncate_growable(int fd, off_t length)
{
	int ret;
	off_t pos;
	static const char zero = 0;

	/* Try the simple method first */
	if ((ret = ftruncate(fd, length)) != -1)
	{
		return ret;
	}

	/*
	 * Some kind of error. Perhaps we were trying to grow. Retry
	 * in a safe way.
	 */

	/* Get current position */
	if ((pos = lseek(fd, 0, SEEK_CUR)) == -1)
	{
		perror("lseek");
		return -1;
	}

	/* Seek to new size */
	if (lseek(fd, length, SEEK_SET) == -1)
	{
		perror("lseek");
		return -1;
	}

	/* Write a zero */
	if (write(fd, &zero, 1) == -1)
	{
		perror("write");
		return -1;
	}

	/* Truncate. This shouldn't fail. */
	if (ftruncate(fd, length) == -1)
	{
		perror("ftruncate");
		return -1;
	}

	/* Restore position */
	if (lseek(fd, pos, SEEK_SET) == -1)
	{
		perror("lseek");
		return -1;
	}

	return 0;
}

/* Just like open(2), but if a open with O_EXCL fails, retry with
   GUARDED semantics. This might be necessary because some filesystems
   (such as NFS filesystems mounted from a unfsd server) doesn't
   support O_EXCL. GUARDED semantics are subject to race conditions,
   but we can live with that.
*/
static int
open_weak_exclusive(const char *pathname, int flags, mode_t mode)
{
	int ret;
	struct stat filestat;

	ret = open(pathname, flags, mode);
	if (ret != -1 || !(flags & O_EXCL))
	{
		/* Success, or not using O_EXCL */
		return ret;
	}

	/* An error occured, and we are using O_EXCL. In case the FS
	   doesn't support O_EXCL, some kind of error will be
	   returned. Unfortunately, we don't know which one. Linux
	   2.6.8 seems to return 524, but I cannot find a documented
	   #define for this case. So, we'll return only on errors that
	   we know aren't related to O_EXCL. */
	switch (errno)
	{
		case EACCES:
		case EEXIST:
		case EINTR:
		case EISDIR:
		case ELOOP:
		case ENAMETOOLONG:
		case ENOENT:
		case ENOTDIR:
			return ret;
	}

	/* Retry with GUARDED semantics */
	if (stat(pathname, &filestat) != -1)
	{
		/* File exists */
		errno = EEXIST;
		return -1;
	}
	else
	{
		return open(pathname, flags & ~O_EXCL, mode);
	}
}

uint32
disk_add_devices(uint32 * id, char *name, char *mnt)
{
        uint32 i;

        for (i = 0; i< *id;i++) {
          if (g_rdpdr_device[i].device_type == DEVICE_TYPE_NONE) break;
        }

        if (i >= RDPDR_MAX_DEVICES) return (*id);
        
        strncpy(g_rdpdr_device[i].name, name, sizeof(g_rdpdr_device[i].name) - 1);
        if (strlen(name) > (sizeof(g_rdpdr_device[i].name) - 1))
          fprintf(stderr, "share name %s truncated to %s\n", name,g_rdpdr_device[i].name);

        g_rdpdr_device[i].local_path = (char *) xmalloc(strlen(mnt) + 1);
        strcpy(g_rdpdr_device[i].local_path, mnt);
        g_rdpdr_device[i].device_type = DEVICE_TYPE_DISK;
        if ( *id == i ) (*id)++;
        DEBUG(("DISK add %s id=%d max=%d\n",mnt,i,*id));
        return (i);
}

uint32
disk_del_devices(uint32 * id, char *path)
{
  uint32 i;
  for (i = 0; i< *id;i++) {
    if (strcmp(g_rdpdr_device[i].local_path,path) == 0){    
      g_rdpdr_device[i].device_type = DEVICE_TYPE_NONE;
      break;
    }
  }
  DEBUG(("DISK del %s id=%d\n",path,i));
  return i;
}

#if defined(HAVE_INOTIFY_H) || defined(MAKE_PROTO)
/* ADD DEVICE with INOTIFY */

#define AUTOMOUNT_MAX 100

static int automount_fd=-1;
static char *automount_root[AUTOMOUNT_MAX];

int disk_enum_automount(uint32 * id, char *optarg)
{
   if (automount_fd == -1) {
     automount_fd = inotify_init();
   }

   struct dirent *entry;
   int count = 0;
        /* skip the first egual */
   while ( *optarg && (*(optarg++) != '='));

   DIR *dir = opendir(optarg);
   if (dir) {

   int wd = inotify_add_watch (automount_fd, optarg, IN_CREATE | IN_DELETE );
   printf ("disk_enum_automount %s\n",optarg);
   if (wd < AUTOMOUNT_MAX) {
     automount_root[wd] = optarg;
   } else {
     inotify_rm_watch(automount_fd,wd);
   }


   while ( (entry = readdir(dir)) != NULL ) {
     char path[PATH_MAX];
     sprintf(path,"%s/%s",optarg,entry->d_name);
     struct stat buf;
     lstat(path,&buf);
     if (S_ISDIR(buf.st_mode) && (entry->d_name[0] != '.')) {
       disk_add_devices(id, entry->d_name, path);
       count++;
     }
   }
   closedir (dir);
 }
 return count;
}

void automount_add_fds(int *n, fd_set * rfds)
{
  if (automount_fd != -1) {
    FD_SET(automount_fd, rfds);
    *n = MAX(*n, automount_fd);
  }
}

void automount_check_fds(fd_set * rfds,uint32 *maxid)
{
  if (automount_fd == -1) return;

  if (FD_ISSET(automount_fd, rfds)) {
    ssize_t len, i = 0;
    len = read (automount_fd, inotify_buff, INOTIFY_BUFF_SIZE);
    while (i < len) {
      struct inotify_event *pevent = (struct inotify_event *)&inotify_buff[i];
      int wd = pevent->wd;
      if ( wd < AUTOMOUNT_MAX ) {
        char fullpath[PATH_MAX];
        printf ("automount_check_fds %s %s 0x%04X\n",automount_root[wd],pevent->name,pevent->mask);
        
        if ((pevent->mask  & IN_ISDIR) == IN_ISDIR) {
          sprintf(fullpath,"%s/%s",automount_root[wd],pevent->name);
          if (pevent->mask & IN_CREATE) {
            uint32 id = disk_add_devices(maxid, pevent->name, fullpath);
            printf ("DISK add %s %d %d\n",fullpath,id,*maxid);
            if (id < *maxid ) rdpdr_send_device_new( id );
          }
          if (pevent->mask & IN_DELETE ) {
            uint32 id = disk_del_devices(maxid, fullpath);
            printf ("DISK del %s %d %d\n",fullpath,id,*maxid);
            if (id < *maxid ) rdpdr_send_device_del( id );
          }           
        } 
      }
      i += sizeof(struct inotify_event) + pevent->len;
    }
  }
}
#endif /* HAVE_INOTIFY_H */

/* Enumeration of devices from rdesktop.c        */
/* returns numer of units found and initialized. */
/* optarg looks like ':h=/mnt/floppy,b=/mnt/usbdevice1' */
/* when it arrives to this function.             */
int
disk_enum_devices(uint32 * id, char *optarg)
{
	char *pos = optarg;
	char *pos2;
	int count = 0;

	/* skip the first colon */
	optarg++;
	while ((pos = next_arg(optarg, ',')) && *id < RDPDR_MAX_DEVICES)
	{
		pos2 = next_arg(optarg, '=');

		strncpy(g_rdpdr_device[*id].name, optarg, sizeof(g_rdpdr_device[*id].name) - 1);
		if (strlen(optarg) > (sizeof(g_rdpdr_device[*id].name) - 1))
			fprintf(stderr, "share name %s truncated to %s\n", optarg,
				g_rdpdr_device[*id].name);

		g_rdpdr_device[*id].local_path = (char *) xmalloc(strlen(pos2) + 1);
		strcpy(g_rdpdr_device[*id].local_path, pos2);
		g_rdpdr_device[*id].device_type = DEVICE_TYPE_DISK;
		count++;
		(*id)++;

		optarg = pos;
	}
	return count;
}

/* Opens or creates a file or directory */
static RD_NTSTATUS
disk_create(uint32 device_id, uint32 accessmask, uint32 sharemode, uint32 create_disposition,
	    uint32 flags_and_attributes, char *filename, RD_NTHANDLE * phandle)
{
	RD_NTHANDLE handle;
	DIR *dirp;
	int flags, mode;
	char path[PATH_MAX];
	struct stat filestat;

	handle = 0;
	dirp = NULL;
	flags = 0;
	mode = S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;

	if (filename && *filename && filename[strlen(filename) - 1] == '/')
		filename[strlen(filename) - 1] = 0;

	sprintf(path, "%s%s", g_rdpdr_device[device_id].local_path, filename ? filename : "");

	/* Protect against mailicous servers:
	   somelongpath/..     not allowed
	   somelongpath/../b   not allowed
	   somelongpath/..b    in principle ok, but currently not allowed
	   somelongpath/b..    ok
	   somelongpath/b..b   ok
	   somelongpath/b../c  ok
	 */
	if (strstr(path, "/.."))
	{
		return RD_STATUS_ACCESS_DENIED;
	}

	switch (create_disposition)
	{
		case CREATE_ALWAYS:

			/* Delete existing file/link. */
			unlink(path);
			flags |= O_CREAT;
			break;

		case CREATE_NEW:

			/* If the file already exists, then fail. */
			flags |= O_CREAT | O_EXCL;
			break;

		case OPEN_ALWAYS:

			/* Create if not already exists. */
			flags |= O_CREAT;
			break;

		case OPEN_EXISTING:

			/* Default behaviour */
			break;

		case TRUNCATE_EXISTING:

			/* If the file does not exist, then fail. */
			flags |= O_TRUNC;
			break;
	}

	/*printf("Open: \"%s\"  flags: %X, accessmask: %X sharemode: %X create disp: %X\n", path, flags_and_attributes, accessmask, sharemode, create_disposition); */

	/* Get information about file and set that flag ourselfs */
	if ((stat(path, &filestat) == 0) && (S_ISDIR(filestat.st_mode)))
	{
		if (flags_and_attributes & FILE_NON_DIRECTORY_FILE)
			return RD_STATUS_FILE_IS_A_DIRECTORY;
		else
			flags_and_attributes |= FILE_DIRECTORY_FILE;
	}

	if (flags_and_attributes & FILE_DIRECTORY_FILE)
	{
		if (flags & O_CREAT)
		{
			mkdir(path, mode);
		}

		dirp = opendir(path);
		if (!dirp)
		{
			switch (errno)
			{
				case EACCES:

					return RD_STATUS_ACCESS_DENIED;

				case ENOENT:

					return RD_STATUS_NO_SUCH_FILE;

				default:

					perror("opendir");
					return RD_STATUS_NO_SUCH_FILE;
			}
		}
		handle = DIRFD(dirp);
	}
	else
	{

		if (accessmask & GENERIC_ALL
		    || (accessmask & GENERIC_READ && accessmask & GENERIC_WRITE))
		{
			flags |= O_RDWR;
		}
		else if ((accessmask & GENERIC_WRITE) && !(accessmask & GENERIC_READ))
		{
			flags |= O_WRONLY;
		}
		else
		{
			flags |= O_RDONLY;
		}

		handle = open_weak_exclusive(path, flags, mode);
		if (handle == -1)
		{
			switch (errno)
			{
				case EISDIR:

					return RD_STATUS_FILE_IS_A_DIRECTORY;

				case EACCES:

					return RD_STATUS_ACCESS_DENIED;

				case ENOENT:

					return RD_STATUS_NO_SUCH_FILE;
				case EEXIST:

					return RD_STATUS_OBJECT_NAME_COLLISION;
				default:

					perror("open");
					return RD_STATUS_NO_SUCH_FILE;
			}
		}

		/* all read and writes of files should be non blocking */
		if (fcntl(handle, F_SETFL, O_NONBLOCK) == -1)
			perror("fcntl");
	}

	if (handle >= MAX_OPEN_FILES)
	{
		error("Maximum number of open files (%s) reached. Increase MAX_OPEN_FILES!\n",
		      handle);
		exit(EX_SOFTWARE);
	}

	if (dirp)
		g_fileinfo[handle].pdir = dirp;
	else
		g_fileinfo[handle].pdir = NULL;

	g_fileinfo[handle].device_id = device_id;
	g_fileinfo[handle].flags_and_attributes = flags_and_attributes;
	g_fileinfo[handle].accessmask = accessmask;
	strncpy(g_fileinfo[handle].path, path, PATH_MAX - 1);
	g_fileinfo[handle].delete_on_close = False;

	if (accessmask & GENERIC_ALL || accessmask & GENERIC_WRITE)
		g_notify_stamp = True;

	*phandle = handle;
	return RD_STATUS_SUCCESS;
}

static RD_NTSTATUS
disk_close(RD_NTHANDLE handle)
{
	struct fileinfo *pfinfo;

	pfinfo = &(g_fileinfo[handle]);

	if (pfinfo->accessmask & GENERIC_ALL || pfinfo->accessmask & GENERIC_WRITE)
		g_notify_stamp = True;

	rdpdr_abort_io(handle, 0, RD_STATUS_CANCELLED);

	if (pfinfo->pdir)
	{
		if (closedir(pfinfo->pdir) < 0)
		{
			perror("closedir");
			return RD_STATUS_INVALID_HANDLE;
		}

		if (pfinfo->delete_on_close)
			if (rmdir(pfinfo->path) < 0)
			{
				perror(pfinfo->path);
				return RD_STATUS_ACCESS_DENIED;
			}
		pfinfo->delete_on_close = False;
	}
	else
	{
		if (close(handle) < 0)
		{
			perror("close");
			return RD_STATUS_INVALID_HANDLE;
		}
		if (pfinfo->delete_on_close)
			if (unlink(pfinfo->path) < 0)
			{
				perror(pfinfo->path);
				return RD_STATUS_ACCESS_DENIED;
			}

		pfinfo->delete_on_close = False;
	}

	return RD_STATUS_SUCCESS;
}

static RD_NTSTATUS
disk_read(RD_NTHANDLE handle, uint8 * data, uint32 length, uint32 offset, uint32 * result)
{
	int n;

#if 0
	/* browsing dir ????        */
	/* each request is 24 bytes */
	if (g_fileinfo[handle].flags_and_attributes & FILE_DIRECTORY_FILE)
	{
		*result = 0;
		return STATUS_SUCCESS;
	}
#endif

	lseek(handle, offset, SEEK_SET);

	n = read(handle, data, length);

	if (n < 0)
	{
		*result = 0;
		switch (errno)
		{
			case EISDIR:
				/* Implement 24 Byte directory read ??
				   with STATUS_NOT_IMPLEMENTED server doesn't read again */
				/* return STATUS_FILE_IS_A_DIRECTORY; */
				return RD_STATUS_NOT_IMPLEMENTED;
			default:
				perror("read");
				return RD_STATUS_INVALID_PARAMETER;
		}
	}

	*result = n;

	return RD_STATUS_SUCCESS;
}

static RD_NTSTATUS
disk_write(RD_NTHANDLE handle, uint8 * data, uint32 length, uint32 offset, uint32 * result)
{
	int n;

	lseek(handle, offset, SEEK_SET);

	n = write(handle, data, length);

	if (n < 0)
	{
		perror("write");
		*result = 0;
		switch (errno)
		{
			case ENOSPC:
				return RD_STATUS_DISK_FULL;
			default:
				return RD_STATUS_ACCESS_DENIED;
		}
	}

	*result = n;

	return RD_STATUS_SUCCESS;
}

RD_NTSTATUS
disk_query_information(RD_NTHANDLE handle, uint32 info_class, STREAM out)
{
	uint32 file_attributes, ft_high, ft_low;
	struct stat filestat;
	char *path, *filename;

	path = g_fileinfo[handle].path;

	/* Get information about file */
	if (fstat(handle, &filestat) != 0)
	{
		perror("stat");
		out_uint8(out, 0);
		return RD_STATUS_ACCESS_DENIED;
	}

	/* Set file attributes */
	file_attributes = 0;
	if (S_ISDIR(filestat.st_mode))
		file_attributes |= FILE_ATTRIBUTE_DIRECTORY;

	filename = 1 + strrchr(path, '/');
	if (filename && filename[0] == '.')
		file_attributes |= FILE_ATTRIBUTE_HIDDEN;

	if (!file_attributes)
		file_attributes |= FILE_ATTRIBUTE_NORMAL;

	if (!(filestat.st_mode & S_IWUSR))
		file_attributes |= FILE_ATTRIBUTE_READONLY;

	/* Return requested data */
	switch (info_class)
	{
		case FileBasicInformation:
			seconds_since_1970_to_filetime(get_create_time(&filestat), &ft_high,
						       &ft_low);
			out_uint32_le(out, ft_low);	/* create_access_time */
			out_uint32_le(out, ft_high);

			seconds_since_1970_to_filetime(filestat.st_atime, &ft_high, &ft_low);
			out_uint32_le(out, ft_low);	/* last_access_time */
			out_uint32_le(out, ft_high);

			seconds_since_1970_to_filetime(filestat.st_mtime, &ft_high, &ft_low);
			out_uint32_le(out, ft_low);	/* last_write_time */
			out_uint32_le(out, ft_high);

			seconds_since_1970_to_filetime(filestat.st_ctime, &ft_high, &ft_low);
			out_uint32_le(out, ft_low);	/* last_change_time */
			out_uint32_le(out, ft_high);

			out_uint32_le(out, file_attributes);
			break;

		case FileStandardInformation:

			out_uint32_le(out, filestat.st_size);	/* Allocation size */
			out_uint32_le(out, 0);
			out_uint32_le(out, filestat.st_size);	/* End of file */
			out_uint32_le(out, 0);
			out_uint32_le(out, filestat.st_nlink);	/* Number of links */
			out_uint8(out, 0);	/* Delete pending */
			out_uint8(out, S_ISDIR(filestat.st_mode) ? 1 : 0);	/* Directory */
			break;

		case FileObjectIdInformation:

			out_uint32_le(out, file_attributes);	/* File Attributes */
			out_uint32_le(out, 0);	/* Reparse Tag */
			break;

		default:

			unimpl("IRP Query (File) Information class: 0x%x\n", info_class);
			return RD_STATUS_INVALID_PARAMETER;
	}
	return RD_STATUS_SUCCESS;
}

RD_NTSTATUS
disk_set_information(RD_NTHANDLE handle, uint32 info_class, STREAM in, STREAM out)
{
	uint32 length, file_attributes, ft_high, ft_low;
	char *newname, fullpath[PATH_MAX];
	struct fileinfo *pfinfo;
	int mode;
	struct stat filestat;
	time_t write_time, change_time, access_time, mod_time;
	struct utimbuf tvs;
	struct STATFS_T stat_fs;

	pfinfo = &(g_fileinfo[handle]);
	g_notify_stamp = True;
	newname = NULL;

	switch (info_class)
	{
		case FileBasicInformation:
			write_time = change_time = access_time = 0;

			in_uint8s(in, 4);	/* Handle of root dir? */
			in_uint8s(in, 24);	/* unknown */

			/* CreationTime */
			in_uint32_le(in, ft_low);
			in_uint32_le(in, ft_high);

			/* AccessTime */
			in_uint32_le(in, ft_low);
			in_uint32_le(in, ft_high);
			if (ft_low || ft_high)
				access_time = convert_1970_to_filetime(ft_high, ft_low);

			/* WriteTime */
			in_uint32_le(in, ft_low);
			in_uint32_le(in, ft_high);
			if (ft_low || ft_high)
				write_time = convert_1970_to_filetime(ft_high, ft_low);

			/* ChangeTime */
			in_uint32_le(in, ft_low);
			in_uint32_le(in, ft_high);
			if (ft_low || ft_high)
				change_time = convert_1970_to_filetime(ft_high, ft_low);

			in_uint32_le(in, file_attributes);

			if (fstat(handle, &filestat))
				return RD_STATUS_ACCESS_DENIED;

			tvs.modtime = filestat.st_mtime;
			tvs.actime = filestat.st_atime;
			if (access_time)
				tvs.actime = access_time;


			if (write_time || change_time)
				mod_time = MIN(write_time, change_time);
			else
				mod_time = write_time ? write_time : change_time;

			if (mod_time)
				tvs.modtime = mod_time;


			if (access_time || write_time || change_time)
			{
#if WITH_DEBUG_RDP5
				printf("FileBasicInformation access       time %s",
				       ctime(&tvs.actime));
				printf("FileBasicInformation modification time %s",
				       ctime(&tvs.modtime));
#endif
				if (utime(pfinfo->path, &tvs) && errno != EPERM)
					return RD_STATUS_ACCESS_DENIED;
			}

			if (!file_attributes)
				break;	/* not valid */

			mode = filestat.st_mode;

			if (file_attributes & FILE_ATTRIBUTE_READONLY)
				mode &= ~(S_IWUSR | S_IWGRP | S_IWOTH);
			else
				mode |= S_IWUSR;

			mode &= 0777;
#if WITH_DEBUG_RDP5
			printf("FileBasicInformation set access mode 0%o", mode);
#endif

			if (fchmod(handle, mode))
				return RD_STATUS_ACCESS_DENIED;

			break;

		case FileRenameInformation:

			in_uint8s(in, 4);	/* Handle of root dir? */
			in_uint8s(in, 0x1a);	/* unknown */
			in_uint32_le(in, length);

			if (length && (length / 2) >= 256)
				return RD_STATUS_INVALID_PARAMETER;

			rdp_in_unistr(in, length, &newname, &length);
			if (newname == NULL)
				return RD_STATUS_INVALID_PARAMETER;

			convert_to_unix_filename(newname);

			sprintf(fullpath, "%s%s", g_rdpdr_device[pfinfo->device_id].local_path,
				newname);

			free(newname);

			if (rename(pfinfo->path, fullpath) != 0)
			{
				perror("rename");
				return RD_STATUS_ACCESS_DENIED;
			}
			break;

		case FileDispositionInformation:
			/* As far as I understand it, the correct
			   thing to do here is to *schedule* a delete,
			   so it will be deleted when the file is
			   closed. Subsequent
			   FileDispositionInformation requests with
			   DeleteFile set to FALSE should unschedule
			   the delete. See
			   http://www.osronline.com/article.cfm?article=245. */

			/* FileDispositionInformation always sets delete_on_close to true.
			   "STREAM in" includes Length(4bytes) , Padding(24bytes) and SetBuffer(zero byte).
			   Length is always set to zero.
			   [MS-RDPEFS] http://msdn.microsoft.com/en-us/library/cc241305%28PROT.10%29.aspx
			   - 2.2.3.3.9 Server Drive Set Information Request
			 */
			in_uint8s(in, 4);	/* length of SetBuffer */
			in_uint8s(in, 24);	/* padding */


			if ((pfinfo->accessmask &
			     (FILE_DELETE_ON_CLOSE | FILE_COMPLETE_IF_OPLOCKED)))
			{
				/* if file exists in directory , necessary to return RD_STATUS_DIRECTORY_NOT_EMPTY with win2008
				   [MS-RDPEFS] http://msdn.microsoft.com/en-us/library/cc241305%28PROT.10%29.aspx
				   - 2.2.3.3.9 Server Drive Set Information Request
				   - 2.2.3.4.9 Client Drive Set Information Response
				   [MS-FSCC] http://msdn.microsoft.com/en-us/library/cc231987%28PROT.10%29.aspx
				   - 2.4.11 FileDispositionInformation
				   [FSBO] http://msdn.microsoft.com/en-us/library/cc246487%28PROT.13%29.aspx
				   - 4.3.2 Set Delete-on-close using FileDispositionInformation Information Class (IRP_MJ_SET_INFORMATION)
				 */
				if (pfinfo->pdir)
				{
					DIR *dp = opendir(pfinfo->path);
					struct dirent *dir;

					while ((dir = readdir(dp)) != NULL)
					{
						if (strcmp(dir->d_name, ".") != 0
						    && strcmp(dir->d_name, "..") != 0)
						{
							closedir(dp);
							return RD_STATUS_DIRECTORY_NOT_EMPTY;
						}
					}
					closedir(dp);
				}

				pfinfo->delete_on_close = True;
			}

			break;

		case FileAllocationInformation:
			/* Fall through to FileEndOfFileInformation,
			   which uses ftrunc. This is like Samba with
			   "strict allocation = false", and means that
			   we won't detect out-of-quota errors, for
			   example. */

		case FileEndOfFileInformation:
			in_uint8s(in, 28);	/* unknown */
			in_uint32_le(in, length);	/* file size */

			/* prevents start of writing if not enough space left on device */
			if (STATFS_FN(pfinfo->path, &stat_fs) == 0)
				if (stat_fs.f_bfree * stat_fs.f_bsize < length)
					return RD_STATUS_DISK_FULL;

			if (ftruncate_growable(handle, length) != 0)
			{
				return RD_STATUS_DISK_FULL;
			}

			break;
		default:

			unimpl("IRP Set File Information class: 0x%x\n", info_class);
			return RD_STATUS_INVALID_PARAMETER;
	}
	return RD_STATUS_SUCCESS;
}


#if defined(HAVE_INOTIFY_H) || defined(MAKE_PROTO)
/* NOTIFY with INOTIFY */
#define NOTIFY_MAX 100

static int notify_fd=-1;

struct {
  RD_NTHANDLE handle;
  uint32 device_id;
  uint32 msgid;
  uint32 info_class;
  int first_ev;
  int last_ev;
  int overrun;
} notify_wd[NOTIFY_MAX];

struct {
  int next;
  struct inotify_event* pevent;
} notify_event[NOTIFY_MAX];

int active_wd[NOTIFY_MAX];

RD_NTSTATUS
disk_create_inotify(uint32 msgid,RD_NTHANDLE handle, uint32 info_class)
{

        struct fileinfo *pfinfo;

        pfinfo = &(g_fileinfo[handle]);
        printf("start disk_create_notify %s info_class %X\n", pfinfo->path,info_class);
        pfinfo->info_class = info_class;
#if 0
	uint32_t inotify_mask = IN_MASK_ADD |
	  (info_class & FILE_NOTIFY_CHANGE_NAME)? IN_CREATE|IN_DELETE|IN_MOVED_FROM|IN_MOVED_TO : 0 |
	  (info_class & FILE_NOTIFY_CHANGE_ATTRIBUTES)? IN_ATTRIB|IN_MOVED_TO|IN_MOVED_FROM|IN_MODIFY : 0 |
	  (info_class & FILE_NOTIFY_CHANGE_SIZE)? IN_MODIFY : 0 |
	  (info_class & FILE_NOTIFY_CHANGE_LAST_WRITE)? IN_CLOSE_WRITE | IN_ATTRIB : 0 |
	  (info_class & FILE_NOTIFY_CHANGE_LAST_ACCESS)? IN_CLOSE | IN_ATTRIB : 0 |
	  (info_class & FILE_NOTIFY_CHANGE_CREATION)? IN_CREATE | IN_ATTRIB : 0 |
	  (info_class & FILE_NOTIFY_CHANGE_EA)? IN_ATTRIB : 0 |
	  (info_class & FILE_NOTIFY_CHANGE_SECURITY)? IN_ATTRIB : 0 /*|
	  (info_class & FILE_NOTIFY_CHANGE_STREAM_NAME)? IN_MODIFY : 0 |
	  (info_class & FILE_NOTIFY_CHANGE_STREAM_SIZE)? IN_MODIFY : 0 |
	  (info_class & FILE_NOTIFY_CHANGE_STREAM_WRITE)? IN_MODIFY : 0*/ ;
#else
	uint32_t inotify_mask = IN_CREATE | IN_DELETE | IN_MOVE |
				IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE;
#endif
        int wd = inotify_add_watch (notify_fd, pfinfo->path, inotify_mask );

        if (wd < AUTOMOUNT_MAX) {
          notify_wd[wd].handle = handle;
          notify_wd[wd].msgid = msgid;
          notify_wd[wd].info_class = info_class;
          notify_wd[wd].first_ev = 0;
          notify_wd[wd].device_id = pfinfo->device_id;
          notify_wd[wd].overrun = 0;
	  pfinfo->wd = wd;
        } else {
          inotify_rm_watch(notify_fd,wd);
        }

        return RD_STATUS_PENDING;
}

void
disk_cancel_inotify(RD_NTHANDLE handle)
{
        struct fileinfo *pfinfo;
        pfinfo = &(g_fileinfo[handle]);

        printf("start disk_cancel_notify %d %s\n", handle, pfinfo->path);
        int wd = pfinfo->wd;
        if (wd) {
          inotify_rm_watch (notify_fd, wd );
          pfinfo->wd = 0;
          notify_wd[wd].handle = 0;
        }
}

void inotify_add_fds(int *n, fd_set * rfds)
{
  if (notify_fd == -1) {
    notify_fd = inotify_init();
  }
  if (notify_fd>0) {
    FD_SET(notify_fd, rfds);
    *n = MAX(*n, notify_fd);
  }
}

uint8 smb2_buffer[((12+FILENAME_MAX*2)*INOTIFY_MSG_NUM)];

void inotify_check_fds(fd_set * rfds,uint32 *maxid)
{
  int ev_max=1,ev_idx;
  int active_max=0,active_idx;

  if (notify_fd == -1 ) return;

  if (FD_ISSET(notify_fd, rfds)) {
    ssize_t len, i = 0;
    len = read (notify_fd, inotify_buff, INOTIFY_BUFF_SIZE);
    while (i < len) {
      struct inotify_event *pevent = (struct inotify_event *)&inotify_buff[i];
      int wd = pevent->wd;
      RD_NTHANDLE handle = 0;
      printf ("notify_check_fds wd=%d %s 0x%04X %d %d\n",wd,pevent->name,pevent->mask,i,len);
       if ( wd < NOTIFY_MAX ) {
        handle = notify_wd[wd].handle;
        printf ("handle=%d\n",handle);
      }

      if (handle != 0) {
	/* dispatch */
	uint32 msgid = notify_wd[wd].msgid;
	if (msgid != 0) {
          ev_idx =ev_max++;
	  notify_event[ev_idx].next=0;

	  notify_event[ev_idx].pevent = pevent;

	  if (notify_wd[wd].first_ev == 0) {
            printf ("F %d-%d EVT=0x%04x,FILE=%s\n",wd,ev_idx,pevent->mask,pevent->name);
	    notify_wd[wd].first_ev = notify_wd[wd].last_ev = ev_idx;
	    active_wd[active_max++]=wd;
	  } else {
            printf ("N %d-%d EVT=0x%04x,FILE=%s\n",wd,ev_idx,pevent->mask,pevent->name);
	    notify_event[notify_wd[wd].last_ev].next = ev_idx;
	    notify_wd[wd].last_ev = ev_idx;
          }
        }
      }
      i += sizeof(struct inotify_event) + pevent->len;
    }

    
    for (active_idx=0;active_idx<active_max;active_idx++) {

      int wd = active_wd[active_idx];

      struct stream out;
      out.data = out.p = smb2_buffer;
      out.size = sizeof(smb2_buffer);
      
      uint32 action = 0;
      uint32 status = RD_STATUS_SUCCESS;
      if (notify_wd[wd].overrun != 0) {
	status = RD_STATUS_NOTIFY_ENUM_DIR;
	notify_wd[wd].overrun = 0;
      }

      for (ev_idx = notify_wd[wd].first_ev;ev_idx;ev_idx=notify_event[ev_idx].next) {
	struct inotify_event *pevent = notify_event[ev_idx].pevent;
	if (pevent->mask & IN_IGNORED) {
	  break;
	} else if (pevent->mask & IN_Q_OVERFLOW) {
          status = RD_STATUS_NOTIFY_ENUM_DIR;
	  break;
	} else if (pevent->mask & IN_CREATE) {
	  action=NOTIFY_ACTION_ADDED;
	} else if (pevent->mask & IN_DELETE) {
	  action=NOTIFY_ACTION_REMOVED;
	} else if (pevent->mask & IN_MOVED_FROM) {
	  action=NOTIFY_ACTION_OLD_NAME;
	} else if (pevent->mask & IN_MOVED_TO) {
	  action=NOTIFY_ACTION_NEW_NAME;
	} else {
	  action=NOTIFY_ACTION_MODIFIED;
	}

	printf ("%d-%d ACTION=0x%04x,FILE=%s\n",active_idx,ev_idx,action,pevent->name);

	uint32 namelen = (strlen(pevent->name) + 1) * 2; //JLB
	uint32 offset = namelen + 12;

	out_uint32_le(&out,offset); 			// NextEntryOffset
 	out_uint32_le(&out,action); 			// Action
 	out_uint32_le(&out,namelen);			// FileNameLength
	rdp_out_unistr(&out, pevent->name , namelen - 2); 	//NotifyChange
      }

      if (notify_wd[wd].msgid != 0 ) {
        uint32 length = (int)(out.p - out.data);
        rdpdr_send_completion(notify_wd[wd].device_id,notify_wd[wd].msgid,status, length, out.data, length);
        notify_wd[wd].msgid=0;
      } else {
        printf ("Message lost %d\n",notify_wd[wd].overrun);
	notify_wd[wd].overrun ++;
      }

      notify_wd[wd].first_ev = 0;

/* notify_wd[wd].handle = 0; */

    }
  }
}

#endif /* HAVE_INOTIFY_H */

#if !defined(HAVE_INOTIFY_H) || defined(MAKE_PROTO)

RD_NTSTATUS
disk_check_notify(RD_NTHANDLE handle)
{
	struct fileinfo *pfinfo;
	RD_NTSTATUS status = RD_STATUS_PENDING;

	NOTIFY notify;

	pfinfo = &(g_fileinfo[handle]);
	if (!pfinfo->pdir)
		return RD_STATUS_INVALID_DEVICE_REQUEST;



	status = NotifyInfo(handle, pfinfo->info_class, &notify);

	if (status != RD_STATUS_PENDING)
		return status;

	if (memcmp(&pfinfo->notify, &notify, sizeof(NOTIFY)))
	{
		/*printf("disk_check_notify found changed event\n"); */
		memcpy(&pfinfo->notify, &notify, sizeof(NOTIFY));
		status = RD_STATUS_NOTIFY_ENUM_DIR;
	}

	return status;


}

RD_NTSTATUS
disk_create_notify(RD_NTHANDLE handle, uint32 info_class)
{

	struct fileinfo *pfinfo;
	RD_NTSTATUS ret = RD_STATUS_PENDING;

	/* printf("start disk_create_notify info_class %X\n", info_class); */

	pfinfo = &(g_fileinfo[handle]);
	pfinfo->info_class = info_class;

	ret = NotifyInfo(handle, info_class, &pfinfo->notify);

	if (info_class & 0x1000)
	{			/* ???? */
		if (ret == RD_STATUS_PENDING)
			return RD_STATUS_SUCCESS;
	}

	/* printf("disk_create_notify: num_entries %d\n", pfinfo->notify.num_entries); */


	return ret;

}

static RD_NTSTATUS
NotifyInfo(RD_NTHANDLE handle, uint32 info_class, NOTIFY * p)
{
	struct fileinfo *pfinfo;
	struct stat filestat;
	struct dirent *dp;
	char *fullname;
	DIR *dpr;

	pfinfo = &(g_fileinfo[handle]);
	if (fstat(handle, &filestat) < 0)
	{
		perror("NotifyInfo");
		return RD_STATUS_ACCESS_DENIED;
	}
	p->modify_time = filestat.st_mtime;
	p->status_time = filestat.st_ctime;
	p->num_entries = 0;
	p->total_time = 0;


	dpr = opendir(pfinfo->path);
	if (!dpr)
	{
		perror("NotifyInfo");
		return RD_STATUS_ACCESS_DENIED;
	}


	while ((dp = readdir(dpr)))
	{
		if (!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, ".."))
			continue;
		p->num_entries++;
		fullname = (char *) xmalloc(strlen(pfinfo->path) + strlen(dp->d_name) + 2);
		sprintf(fullname, "%s/%s", pfinfo->path, dp->d_name);

		if (!stat(fullname, &filestat))
		{
			p->total_time += (filestat.st_mtime + filestat.st_ctime);
		}

		xfree(fullname);
	}
	closedir(dpr);

	return RD_STATUS_PENDING;
}
#endif /* HAVE_INOTIFY_H */

static FsInfoType *
FsVolumeInfo(char *fpath)
{

	static FsInfoType info;
#ifdef USE_SETMNTENT
	FILE *fdfs;
	struct mntent *e;
#endif

	/* initialize */
	memset(&info, 0, sizeof(info));
	strcpy(info.label, "RDESKTOP");
	strcpy(info.type, "RDPFS");

#ifdef USE_SETMNTENT
	fdfs = setmntent(MNTENT_PATH, "r");
	if (!fdfs)
		return &info;

	while ((e = getmntent(fdfs)))
	{
		if (str_startswith(e->mnt_dir, fpath))
		{
			strcpy(info.type, e->mnt_type);
			strcpy(info.name, e->mnt_fsname);
			if (strstr(e->mnt_opts, "vfat") || strstr(e->mnt_opts, "iso9660"))
			{
				int fd = open(e->mnt_fsname, O_RDONLY);
				if (fd >= 0)
				{
					unsigned char buf[512];
					memset(buf, 0, sizeof(buf));
					if (strstr(e->mnt_opts, "vfat"))
						 /*FAT*/
					{
						strcpy(info.type, "vfat");
						read(fd, buf, sizeof(buf));
						info.serial =
							(buf[42] << 24) + (buf[41] << 16) +
							(buf[40] << 8) + buf[39];
						strncpy(info.label, (char *) buf + 43, 10);
						info.label[10] = '\0';
					}
					else if (lseek(fd, 32767, SEEK_SET) >= 0)	/* ISO9660 */
					{
						read(fd, buf, sizeof(buf));
						strncpy(info.label, (char *) buf + 41, 32);
						info.label[32] = '\0';
						/* info.Serial = (buf[128]<<24)+(buf[127]<<16)+(buf[126]<<8)+buf[125]; */
					}
					close(fd);
				}
			}
		}
	}
	endmntent(fdfs);
#else
	/* initialize */
	memset(&info, 0, sizeof(info));
	strcpy(info.label, "RDESKTOP");
	strcpy(info.type, "RDPFS");

#endif
	return &info;
}


RD_NTSTATUS
disk_query_volume_information(RD_NTHANDLE handle, uint32 info_class, STREAM out)
{
	struct STATFS_T stat_fs;
	struct fileinfo *pfinfo;
	FsInfoType *fsinfo;

	pfinfo = &(g_fileinfo[handle]);

	if (STATFS_FN(pfinfo->path, &stat_fs) != 0)
	{
		perror("statfs");
		return RD_STATUS_ACCESS_DENIED;
	}

	fsinfo = FsVolumeInfo(pfinfo->path);

	switch (info_class)
	{
		case FileFsVolumeInformation:

			out_uint32_le(out, 0);	/* volume creation time low */
			out_uint32_le(out, 0);	/* volume creation time high */
			out_uint32_le(out, fsinfo->serial);	/* serial */

			out_uint32_le(out, 2 * strlen(fsinfo->label));	/* length of string */

			out_uint8(out, 0);	/* support objects? */
			rdp_out_unistr(out, fsinfo->label, 2 * strlen(fsinfo->label) - 2);
			break;

		case FileFsSizeInformation:

			out_uint32_le(out, stat_fs.f_blocks);	/* Total allocation units low */
			out_uint32_le(out, 0);	/* Total allocation high units */
			out_uint32_le(out, stat_fs.f_bfree);	/* Available allocation units */
			out_uint32_le(out, 0);	/* Available allowcation units */
			out_uint32_le(out, stat_fs.f_bsize / 0x200);	/* Sectors per allocation unit */
			out_uint32_le(out, 0x200);	/* Bytes per sector */
			break;

		case FileFsFullSizeInformation:

			out_uint32_le(out, stat_fs.f_blocks);	/* Total allocation units low */
			out_uint32_le(out, 0);	/* Total allocation units high */
			out_uint32_le(out, stat_fs.f_bavail);	/* Caller allocation units low */
			out_uint32_le(out, 0);	/* Caller allocation units high */
			out_uint32_le(out, stat_fs.f_bfree);	/* Available allocation units */
			out_uint32_le(out, 0);	/* Available allowcation units */
			out_uint32_le(out, stat_fs.f_bsize / 0x200);	/* Sectors per allocation unit */
			out_uint32_le(out, 0x200);	/* Bytes per sector */
			break;

		case FileFsAttributeInformation:

			out_uint32_le(out, FS_CASE_SENSITIVE | FS_CASE_IS_PRESERVED);	/* fs attributes */
			out_uint32_le(out, F_NAMELEN(stat_fs));	/* max length of filename */

			out_uint32_le(out, 2 * strlen(fsinfo->type));	/* length of fs_type */
			rdp_out_unistr(out, fsinfo->type, 2 * strlen(fsinfo->type) - 2);
			break;

		case FileFsLabelInformation:
		case FileFsDeviceInformation:
		case FileFsControlInformation:
		case FileFsObjectIdInformation:
		case FileFsMaximumInformation:

		default:

			unimpl("IRP Query Volume Information class: 0x%x\n", info_class);
			return RD_STATUS_INVALID_PARAMETER;
	}
	return RD_STATUS_SUCCESS;
}

RD_NTSTATUS
disk_query_directory(RD_NTHANDLE handle, uint32 info_class, char *pattern, STREAM out)
{
	uint32 file_attributes, ft_low, ft_high;
	char *dirname, fullpath[PATH_MAX];
	DIR *pdir;
	struct dirent *pdirent;
	struct stat filestat;
	struct fileinfo *pfinfo;

	pfinfo = &(g_fileinfo[handle]);
	pdir = pfinfo->pdir;
	dirname = pfinfo->path;
	file_attributes = 0;


	switch (info_class)
	{
		case FileBothDirectoryInformation:
		case FileDirectoryInformation:
		case FileFullDirectoryInformation:
		case FileNamesInformation:

			/* If a search pattern is received, remember this pattern, and restart search */
			if (pattern != NULL && pattern[0] != 0)
			{
				strncpy(pfinfo->pattern, 1 + strrchr(pattern, '/'), PATH_MAX - 1);
				rewinddir(pdir);
			}

			/* find next dirent matching pattern */
			pdirent = readdir(pdir);
			while (pdirent && fnmatch(pfinfo->pattern, pdirent->d_name, 0) != 0)
				pdirent = readdir(pdir);

			if (pdirent == NULL)
				return RD_STATUS_NO_MORE_FILES;

			/* Get information for directory entry */
			sprintf(fullpath, "%s/%s", dirname, pdirent->d_name);

			if (stat(fullpath, &filestat))
			{
				switch (errno)
				{
					case ENOENT:
					case ELOOP:
					case EACCES:
						/* These are non-fatal errors. */
						memset(&filestat, 0, sizeof(filestat));
						break;
					default:
						/* Fatal error. By returning STATUS_NO_SUCH_FILE, 
						   the directory list operation will be aborted */
						perror(fullpath);
						out_uint8(out, 0);
						return RD_STATUS_NO_SUCH_FILE;
				}
			}

			if (S_ISDIR(filestat.st_mode))
				file_attributes |= FILE_ATTRIBUTE_DIRECTORY;
			if (pdirent->d_name[0] == '.')
				file_attributes |= FILE_ATTRIBUTE_HIDDEN;
			if (!file_attributes)
				file_attributes |= FILE_ATTRIBUTE_NORMAL;
			if (!(filestat.st_mode & S_IWUSR))
				file_attributes |= FILE_ATTRIBUTE_READONLY;

			/* Return requested information */
			out_uint32_le(out, 0);	/* NextEntryOffset */
			out_uint32_le(out, 0);	/* FileIndex zero */
			break;

		default:
			unimpl("IRP Query Directory sub: 0x%x\n", info_class);
			return RD_STATUS_INVALID_PARAMETER;
	}

	switch (info_class)
	{
		case FileBothDirectoryInformation:

			seconds_since_1970_to_filetime(get_create_time(&filestat), &ft_high,
						       &ft_low);
			out_uint32_le(out, ft_low);	/* create time */
			out_uint32_le(out, ft_high);

			seconds_since_1970_to_filetime(filestat.st_atime, &ft_high, &ft_low);
			out_uint32_le(out, ft_low);	/* last_access_time */
			out_uint32_le(out, ft_high);

			seconds_since_1970_to_filetime(filestat.st_mtime, &ft_high, &ft_low);
			out_uint32_le(out, ft_low);	/* last_write_time */
			out_uint32_le(out, ft_high);

			seconds_since_1970_to_filetime(filestat.st_ctime, &ft_high, &ft_low);
			out_uint32_le(out, ft_low);	/* change_write_time */
			out_uint32_le(out, ft_high);

			out_uint32_le(out, filestat.st_size);	/* filesize low */
			out_uint32_le(out, 0);	/* filesize high */
			out_uint32_le(out, filestat.st_size);	/* filesize low */
			out_uint32_le(out, 0);	/* filesize high */
			out_uint32_le(out, file_attributes);	/* FileAttributes */
			out_uint32_le(out, 2 * strlen(pdirent->d_name) + 2);	/* unicode length */
			out_uint32_le(out, 0);	/* EaSize */
			out_uint8(out, 0);	/* ShortNameLength */
			/* this should be correct according to MS-FSCC specification
			   but it only works when commented out... */
			/* out_uint8(out, 0); *//* Reserved/Padding */
			out_uint8s(out, 2 * 12);	/* ShortName (8.3 name) */
			rdp_out_unistr(out, pdirent->d_name, 2 * strlen(pdirent->d_name));
			break;


		case FileDirectoryInformation:

			seconds_since_1970_to_filetime(get_create_time(&filestat), &ft_high,
						       &ft_low);
			out_uint32_le(out, ft_low);	/* create time */
			out_uint32_le(out, ft_high);

			seconds_since_1970_to_filetime(filestat.st_atime, &ft_high, &ft_low);
			out_uint32_le(out, ft_low);	/* last_access_time */
			out_uint32_le(out, ft_high);

			seconds_since_1970_to_filetime(filestat.st_mtime, &ft_high, &ft_low);
			out_uint32_le(out, ft_low);	/* last_write_time */
			out_uint32_le(out, ft_high);

			seconds_since_1970_to_filetime(filestat.st_ctime, &ft_high, &ft_low);
			out_uint32_le(out, ft_low);	/* change_write_time */
			out_uint32_le(out, ft_high);

			out_uint32_le(out, filestat.st_size);	/* filesize low */
			out_uint32_le(out, 0);	/* filesize high */
			out_uint32_le(out, filestat.st_size);	/* filesize low */
			out_uint32_le(out, 0);	/* filesize high */
			out_uint32_le(out, file_attributes);
			out_uint32_le(out, 2 * strlen(pdirent->d_name) + 2);	/* unicode length */
			rdp_out_unistr(out, pdirent->d_name, 2 * strlen(pdirent->d_name));
			break;


		case FileFullDirectoryInformation:

			seconds_since_1970_to_filetime(get_create_time(&filestat), &ft_high,
						       &ft_low);
			out_uint32_le(out, ft_low);	/* create time */
			out_uint32_le(out, ft_high);

			seconds_since_1970_to_filetime(filestat.st_atime, &ft_high, &ft_low);
			out_uint32_le(out, ft_low);	/* last_access_time */
			out_uint32_le(out, ft_high);

			seconds_since_1970_to_filetime(filestat.st_mtime, &ft_high, &ft_low);
			out_uint32_le(out, ft_low);	/* last_write_time */
			out_uint32_le(out, ft_high);

			seconds_since_1970_to_filetime(filestat.st_ctime, &ft_high, &ft_low);
			out_uint32_le(out, ft_low);	/* change_write_time */
			out_uint32_le(out, ft_high);

			out_uint32_le(out, filestat.st_size);	/* filesize low */
			out_uint32_le(out, 0);	/* filesize high */
			out_uint32_le(out, filestat.st_size);	/* filesize low */
			out_uint32_le(out, 0);	/* filesize high */
			out_uint32_le(out, file_attributes);
			out_uint32_le(out, 2 * strlen(pdirent->d_name) + 2);	/* unicode length */
			out_uint32_le(out, 0);	/* EaSize */
			rdp_out_unistr(out, pdirent->d_name, 2 * strlen(pdirent->d_name));
			break;


		case FileNamesInformation:

			out_uint32_le(out, 2 * strlen(pdirent->d_name) + 2);	/* unicode length */
			rdp_out_unistr(out, pdirent->d_name, 2 * strlen(pdirent->d_name));
			break;


		default:

			unimpl("IRP Query Directory sub: 0x%x\n", info_class);
			return RD_STATUS_INVALID_PARAMETER;
	}

	return RD_STATUS_SUCCESS;
}



static RD_NTSTATUS
disk_device_control(RD_NTHANDLE handle, uint32 request, STREAM in, STREAM out)
{
	if (((request >> 16) != 20) || ((request >> 16) != 9))
		return RD_STATUS_INVALID_PARAMETER;

	/* extract operation */
	request >>= 2;
	request &= 0xfff;

	printf("DISK IOCTL %d\n", request);

	switch (request)
	{
		case 25:	/* ? */
		case 42:	/* ? */
		default:
			unimpl("DISK IOCTL %d\n", request);
			return RD_STATUS_INVALID_PARAMETER;
	}

	return RD_STATUS_SUCCESS;
}

DEVICE_FNS disk_fns = {
	disk_create,
	disk_close,
	disk_read,
	disk_write,
	disk_device_control	/* device_control */
};
