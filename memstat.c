/*
 * This software copyright 1997 Joshua M. Yelon.
 * Debian Maintainer and new Features:
 *   copyright 1999 Bernd Eckenfels, Germany, ecki@debian.org 
 *   (see debian/changelog)
 *   copyright 2008-2009 Michael Meskes, meskes@debian.org
 *
 * Distribution subject to the terms of the GPL.
 *
 */

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <dirent.h>
#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>

/* blacklist devices that just map physical memory */
char *blacklist[] = { "/dev/mem",
    "/dev/dri/"
};

int wide = 0;
int only_pid = 0;
int needinode = 0;

typedef int (*qcmp) (const void *, const void *);

typedef struct mapping *mapping;

struct mapping {
    unsigned int fs, inode, pid;
    unsigned long offs, lo, hi;
    char *path;
    int valid;
    int shared;
};

mapping maptab_data;
size_t maptab_fill;
size_t maptab_size;

static void maptab_expand(void)
{
    size_t bytes;

    bytes = (maptab_size * 2 + 100) * sizeof(struct mapping);
    maptab_data = (mapping) realloc(maptab_data, bytes);
    if (maptab_data == NULL) {
	fprintf(stderr, "Error: realloc returned null, possibly out of memory? Exiting.\n");
	exit(1);
    }
    maptab_size = maptab_size * 2 + 100;
}

static void read_proc(void)
{
    unsigned int nread, pid;
    unsigned long inode, lo, hi, offs;
    char *p, dev[80], buff[PATH_MAX + 300], *path, perm[4];
    DIR *d;
    struct dirent *ent;
    FILE *f;

    mapping m;
    d = opendir("/proc");
    if (d == 0) {
	perror("/proc");
	exit(1);
    }
    while (1) {
	ent = readdir(d);
	if (ent == NULL)
	    break;
	pid = strtol(ent->d_name, NULL, 10);
	if (errno != 0) {
		perror("strtol");
		exit(1);
	}
	if (pid == 0 || (only_pid != 0 && pid != only_pid))
	    continue;
	sprintf(buff, "/proc/%d/maps", pid);
	f = fopen(buff, "r");
	if (f == NULL)
	    continue;
	while (fgets(buff, sizeof(buff), f)) {
	    p = strchr(buff, '-');
	    if (p)
		*p = ' ';
	    path = NULL;
	    if ((strlen(buff) == 10) && (strcmp(buff, " (deleted)") == 0))
		continue;
	    nread = sscanf(buff, "%lx %lx %4s %lx %s %lu %as", &lo, &hi, perm, &offs, dev, &inode, &path);
	    if (nread < 6) {
		fprintf(stderr, "I don't recognize format of /proc/%d/maps. (nread=%d)\n", pid, nread);
		exit(1);
	    }
	    if (maptab_fill == maptab_size)
		maptab_expand();
	    m = maptab_data + maptab_fill;
	    m->fs = (dev_t)strtol(dev, NULL, 16);
	    if (errno != 0) {
		perror("strtol");
                exit(1);
            }
	    m->inode = inode;
	    m->offs = offs;
	    m->pid = pid;
	    m->lo = lo;
	    m->hi = hi;
	    m->valid = 1;
	    if ((nread == 7) && path && path[0]) {
		int i;

		m->path = path;
		for (i = 0; i < sizeof(blacklist) / sizeof(blacklist[0]); i++) {
		    if (!strncmp(path, blacklist[i], strlen(blacklist[i])))
			m->valid = 0;
		}
	    } else {
		sprintf(buff, "[%04x]:%lu", m->fs, inode);
		m->path = strdup(buff);
		needinode = 1;
	    }
	    /* mark segment as either shared or private */
	    if (perm[3] == 's' ||	/* declared as shared */
		perm[2] == 'x')		/* executables are shared */
		m->shared = 1;
	    else
		m->shared = 0;
	    maptab_fill++;
	}
	fclose(f);
    }
    closedir(d);
}

static int sort_by_pid(mapping m1, mapping m2)
{
    int delta;

    delta = m1->pid - m2->pid;
    return delta;
}

static int sort_by_inode(mapping m1, mapping m2)
{
    int delta;

    delta = m1->fs - m2->fs;
    if (delta)
	return delta;
    delta = m1->inode - m2->inode;
    if (delta)
	return delta;
    delta = m1->shared - m2->shared;
    if (delta)
	return delta;
    delta = m1->offs - m2->offs;
    return delta;
}

static void register_path(unsigned int fs, unsigned int inode, char *path, int valid)
{
    size_t i;
    mapping m;
    char *copy = NULL;

    for (i = 0; i < maptab_fill; i++) {
	m = maptab_data + i;
	if ((m->path) && (m->path[0] == '[') && (m->fs == fs)
	    && (m->inode == inode)) {
	    if (copy == NULL)
		copy = strdup(path);
	    m->path = copy;
	    m->valid = valid;
	}
    }
}

static void scan_directory(char *dir)
{
    DIR *d;
    struct dirent *ent;
    struct stat info;
    int ok;
    char full[8192];

    d = opendir(dir);
    if (d == NULL)
	return;
    while ((ent = readdir(d)) != NULL) {
	sprintf(full, "%s/%s", dir, ent->d_name);
	ok = stat(full, &info);
	if (ok >= 0)
	    register_path(info.st_dev, info.st_ino, full, S_ISREG(info.st_mode));
    }
    closedir(d);
}

static void scan_fpath(const char *fn)
{
    char *p;
    FILE *f = fopen(fn, "r");
    char buff[1024];

    if (f == NULL) {
	fprintf(stderr, "Cannot open /etc/memstat.conf\n");
	exit(1);
    }
    while (fgets(buff, 1023, f)) {
	if (buff[0] == '#')
	    continue;
	p = strchr(buff, '\n');
	if (p)
	    *p = 0;
	scan_directory(buff);
    }
    fclose(f);
}

static void printline(char *str)
{
    if (!wide && strlen(str) > 79) {
	str[76] = '.';
	str[77] = '.';
	str[78] = '.';
	str[79] = 0;
    }
    printf("%s\n", str);
}

static void summarize_usage(void)
{
    char buffer[8192];
    unsigned int i, fs, inode, pid, scan;
    unsigned long offs, grand, lo = 0, hi = 0, total, sharedtotal, sharedgrand;
    mapping m;
    char *exe;

    grand = sharedgrand = 0;
    qsort(maptab_data, maptab_fill, sizeof(struct mapping), (qcmp) sort_by_pid);
    for (offs = 0; offs < maptab_fill; offs = scan) {
	char linkname[PATH_MAX], filename[PATH_MAX];
	ssize_t len;
	int deleted = 0;

	pid = maptab_data[offs].pid;
	sprintf(filename, "/proc/%d/exe", pid);
	if ((len = readlink(filename, linkname, PATH_MAX)) == -1) {
	    fprintf(stderr, "Cannot read link information for %s\n", filename);
	    deleted = 1;
	}
	linkname[len] = '\0';
	total = 0;
	for (scan = offs; scan < maptab_fill; scan++) {
	    m = maptab_data + scan;
	    if (m->pid != pid)
		break;
	    /* if fs and/or inode are set, memory belongs to the process listed */
	    /* [vsyscall] is in kernel space */
	    if (!deleted && !m->fs && !m->inode && strcmp(m->path, "[vsyscall]"))
		total += (m->hi - m->lo);
	}
	if (!deleted) {
		sprintf(buffer, "%7ldk: PID %5d (%s)", total / 1024, pid, linkname);
		printline(buffer);
		grand += total;
	}
    }

    qsort(maptab_data, maptab_fill, sizeof(struct mapping), (qcmp) sort_by_inode);
    for (offs = 0; offs < maptab_fill; offs = scan) {
	m = maptab_data + offs;
	if (m->valid == 0) {
	    scan = offs + 1;
	    continue;
	}
	fs = m->fs;
	inode = m->inode;
	exe = m->path;
	if ((fs == 0) && (inode == 0)) {
	    scan = offs + 1;
	    continue;
	}
	sharedtotal = total = hi = 0;
	for (scan = offs; scan < maptab_fill; scan++) {
	    m = maptab_data + scan;
	    if (fs != m->fs || inode != m->inode) {	/* new file */
		break;
	    } else if (m->shared) {
#if 0
		if (hi == 0) {	/* first shared segment */
		    lo = m->offs;
		    hi = m->offs + m->hi - m->lo;
		} else if (m->offs != lo) {	/* new unconnected segment */
		    total += (hi - lo);
		    lo = m->offs;
		    hi = m->offs + m->hi - m->lo;
		} else if (m->offs + m->hi - m->lo > hi) {	/* longer than the one we had earlier */
		    hi = m->offs + m->hi - m->lo;
		}
#else
		if (hi == 0) {
		    lo = m->offs;
		    hi = m->offs + m->hi - m->lo;
		} else if (hi != m->offs + m->hi - m->lo) {
		    sharedtotal += (hi - lo);
		    lo = m->offs;
		    hi = m->offs + m->hi - m->lo;
		}
#endif
	    } else
		total += (m->hi - m->lo);
	}
	/* do we have a segment left that hasn't been counted? */
	if (hi != 0)
	    sharedtotal += (hi - lo);
	pid = 0;
	for (i = offs; i < scan; i++) {
	    m = maptab_data + i;
	    if (m->pid != pid) {
		pid = m->pid;
		sprintf(buffer + strlen(buffer), " %d", pid);
	    }
	}
	sprintf(buffer, "%7ldk(%7ldk): %s", (total + sharedtotal) / 1024, sharedtotal / 1024, exe);
	printline(buffer);
	grand += total;
	sharedgrand += sharedtotal;
    }
    printf("--------\n");
    printf("%7ldk (%7ldk)\n", (grand + sharedgrand) / 1024, sharedgrand / 1024);
}

static void usage(char *prog)
{
    fprintf(stderr, "usage: %s [-w] [-p pid]\n", prog);
    exit(1);
}

int main(int argc, char **argv)
{
    char *prog = argv[0];
    int opt;

    while ((opt = getopt(argc, argv, "wp:")) != -1) {
	switch (opt) {
	case 'w':
	    wide = 1;
	    break;
	case 'p':
	    only_pid = atoi(optarg);
	    break;
	default:
	    usage(prog);
	}
    }

    read_proc();
    if (needinode)
	scan_fpath("/etc/memstat.conf");
    summarize_usage();
    return (0);
}
