/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/errno.h>
#include <fcntl.h>
#include <string.h>
#include <assert.h>
#include "ioshark.h"
#include "ioshark_bench.h"

extern char *progname;

void *
files_db_create_handle(void)
{
	struct files_db_handle *h;
	int i;

	h = malloc(sizeof(struct files_db_handle));
	for (i = 0 ; i < FILE_DB_HASHSIZE ; i++)
		h->files_db_buckets[i] = NULL;
	return h;
}

void *files_db_lookup_byfileno(void *handle, int fileno)
{
	u_int32_t	hash;
	struct files_db_handle *h = (struct files_db_handle *)handle;
	struct files_db_s *db_node;

	hash = fileno % FILE_DB_HASHSIZE;
	db_node = h->files_db_buckets[hash];
	while (db_node != NULL) {
		if (db_node->fileno == fileno)
			break;
		db_node = db_node->next;
	}
	return db_node;
}

void *files_db_add_byfileno(void *handle, int fileno)
{
	u_int32_t	hash = fileno % FILE_DB_HASHSIZE;
	struct files_db_handle *h = (struct files_db_handle *)handle;
	struct files_db_s *db_node;

	db_node = (struct files_db_s *)
		files_db_lookup_byfileno(handle, fileno);
	if (db_node == NULL) {
		db_node = malloc(sizeof(struct files_db_s));
		db_node->fileno = fileno;
		db_node->filename = NULL;
		db_node->size = 0;
		db_node->fd = -1;
		db_node->next = h->files_db_buckets[hash];
		h->files_db_buckets[hash] = db_node;
	} else {
		fprintf(stderr,
			"%s: Node to be added already exists fileno = %d\n\n",
			__func__, fileno);
		exit(EXIT_FAILURE);
	}
	return db_node;
}

void
files_db_fsync_discard_files(void *handle)
{
	struct files_db_handle *h = (struct files_db_handle *)handle;
	struct files_db_s *db_node;
	int i;

	for (i = 0 ; i < FILE_DB_HASHSIZE ; i++) {
		db_node = h->files_db_buckets[i];
		while (db_node != NULL) {
			int do_close = 0;

			if (db_node->fd == -1) {
				int fd;

				/*
				 * File was closed, let's open it so we can
				 * fsync and fadvise(DONTNEED) it.
				 */
				do_close = 1;
				fd = open(files_db_get_filename(db_node),
					  O_RDWR);
				if (fd < 0) {
					fprintf(stderr,
						"%s: open(%s O_RDWR) error %d\n",
						progname, db_node->filename,
						errno);
					exit(EXIT_FAILURE);
				}
				db_node->fd = fd;
			}
			if (fsync(db_node->fd) < 0) {
				fprintf(stderr, "%s: Cannot fsync %s\n",
					__func__, db_node->filename);
				exit(1);
			}
			if (posix_fadvise(db_node->fd, 0, 0,
					  POSIX_FADV_DONTNEED) < 0) {
				fprintf(stderr,
					"%s: Cannot fadvise(DONTNEED) %s\n",
					__func__, db_node->filename);
				exit(1);
			}
			if (do_close) {
				close(db_node->fd);
				db_node->fd = -1;
			}
			db_node = db_node->next;
		}
	}
}

void
files_db_update_fd(void *node, int fd)
{
	struct files_db_s *db_node = (struct files_db_s *)node;

	db_node->fd = fd;
}

void
files_db_close_fd(void *node)
{
	struct files_db_s *db_node = (struct files_db_s *)node;

	if (db_node->fd != -1)
		close(db_node->fd);
	db_node->fd = -1;
}

void
files_db_close_files(void *handle)
{
	struct files_db_handle *h = (struct files_db_handle *)handle;
	struct files_db_s *db_node;
	int i;

	for (i = 0 ; i < FILE_DB_HASHSIZE ; i++) {
		db_node = h->files_db_buckets[i];
		while (db_node != NULL) {
			if ((db_node->fd != -1) && close(db_node->fd) < 0) {
				fprintf(stderr, "%s: Cannot close %s\n",
					__func__, db_node->filename);
				exit(1);
			}
			db_node->fd = -1;
			db_node = db_node->next;
		}
	}
}

void
files_db_unlink_files(void *handle)
{
	struct files_db_handle *h = (struct files_db_handle *)handle;
	struct files_db_s *db_node;
	int i;

	for (i = 0 ; i < FILE_DB_HASHSIZE ; i++) {
		db_node = h->files_db_buckets[i];
		while (db_node != NULL) {
			if ((db_node->fd != -1) && close(db_node->fd) < 0) {
				fprintf(stderr, "%s: Cannot close %s\n",
					__func__, db_node->filename);
				exit(1);
			}
			db_node->fd = -1;
			if (unlink(db_node->filename) < 0) {
				fprintf(stderr, "%s: Cannot unlink %s:%s\n",
					__func__, db_node->filename, strerror(errno));
				exit(1);
			}
			db_node = db_node->next;
		}
	}
}

void
files_db_free_memory(void *handle)
{
	struct files_db_handle *h = (struct files_db_handle *)handle;
	struct files_db_s *db_node, *tmp;
	int i;

	for (i = 0 ; i < FILE_DB_HASHSIZE ; i++) {
		db_node = h->files_db_buckets[i];
		while (db_node != NULL) {
			tmp = db_node;
			db_node = db_node->next;
			free(tmp->filename);
			free(tmp);
		}
	}
	free(h);
}

char *
get_buf(char **buf, int *buflen, int len, int do_fill __attribute__((unused)))
{
	if (len == 0 && *buf == NULL) {
		/*
		 * If we ever get a zero len
		 * request, start with MINBUFLEN
		 */
		if (*buf == NULL)
			len = MINBUFLEN / 2;
	}
	if (*buflen < len) {
		*buflen = MAX(MINBUFLEN, len * 2);
		if (*buf)
			free(*buf);
		*buf = malloc(*buflen);
		if (do_fill) {
			u_int32_t *s;
			int count;

			s = (u_int32_t *)*buf;
			count = *buflen / sizeof(u_int32_t);
			while (count > 0) {
				*s++ = rand();
				count--;
			}
		}
	}
	assert(*buf != NULL);
	return *buf;
}

void
create_file(char *path, size_t size, struct rw_bytes_s *rw_bytes)
{
	int fd, n;
	char *buf = NULL;
	int buflen = 0;

	fd = open(path, O_WRONLY|O_CREAT|O_TRUNC, 0644);
	if (fd < 0) {
		fprintf(stderr, "%s Cannot create file %s, error = %d\n",
			progname, path, errno);
		exit(EXIT_FAILURE);
	}
	while (size > 0) {
		n = MIN(size, MINBUFLEN);
		buf = get_buf(&buf, &buflen, n, 1);
		if (write(fd, buf, n) < n) {
			fprintf(stderr,
				"%s Cannot write file %s, error = %d\n",
				progname, path, errno);
			exit(EXIT_FAILURE);
		}
		rw_bytes->bytes_written += n;
		size -= n;
	}
	if (fsync(fd) < 0) {
		fprintf(stderr, "%s Cannot fsync file %s, error = %d\n",
			progname, path, errno);
		exit(EXIT_FAILURE);
	}
	if (posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED) < 0) {
		fprintf(stderr,
			"%s Cannot fadvise(DONTNEED) file %s, error = %d\n",
			progname, path, errno);
		exit(EXIT_FAILURE);
	}
	close(fd);
}

void
print_op_stats(u_int64_t *op_counts)
{
	int i;
	extern char *IO_op[];

	printf("IO Operation counts :\n");
	for (i = IOSHARK_LSEEK ; i < IOSHARK_MAX_FILE_OP ; i++) {
		printf("%s: %ju\n",
		       IO_op[i], op_counts[i]);
	}
}

void
print_bytes(char *desc, struct rw_bytes_s *rw_bytes)
{
	printf("%s: Reads = %dMB, Writes = %dMB\n",
	       desc,
	       (int)(rw_bytes->bytes_read / (1024 * 1024)),
	       (int)(rw_bytes->bytes_written / (1024 * 1024)));
}

struct cpu_disk_util_stats {
	/* CPU util */
	u_int64_t user_cpu_ticks;
	u_int64_t nice_cpu_ticks;
	u_int64_t system_cpu_ticks;
	u_int64_t idle_cpu_ticks;
	u_int64_t iowait_cpu_ticks;
	u_int64_t hardirq_cpu_ticks;
	u_int64_t softirq_cpu_ticks;
	/* disk util */
	unsigned long long uptime;
	unsigned int tot_ticks;
	unsigned long rd_ios;
	unsigned long wr_ios;
	unsigned long rd_sec;
	unsigned long wr_sec;
};

static struct cpu_disk_util_stats before;
static struct cpu_disk_util_stats after;

#define BUFSIZE		8192

static int hz;

static void
get_HZ(void)
{
	if ((hz = sysconf(_SC_CLK_TCK)) == -1)
		exit(1);
}

#if 0
static int num_cores;

static void
get_cores(void)
{
	if ((num_cores = sysconf(_SC_NPROCESSORS_ONLN)) == -1)
		exit(1);
}
#endif

static void
read_disk_util_state(struct cpu_disk_util_stats *state)
{
	FILE *fp;
        char line[BUFSIZE], dev_name[BUFSIZE];
        unsigned int major, minor;
	unsigned int ios_pgr;
	unsigned int rq_ticks;
	unsigned int wr_ticks;
	unsigned long rd_ticks;
	unsigned long rd_merges;
	unsigned long wr_merges;
	unsigned long up_sec, up_cent;

	/* Read and parse /proc/uptime */
	fp = fopen("/proc/uptime", "r");
	if (fgets(line, sizeof(line), fp) == NULL) {
		fprintf(stderr, "%s: Cannot read /proc/uptime\n",
			progname);
		exit(1);
	}
	fclose(fp);
	sscanf(line, "%lu.%lu", &up_sec, &up_cent);
	state->uptime = (unsigned long long) up_sec * hz +
		(unsigned long long) up_cent * hz / 100;
	/* Read and parse /proc/diskstats */
	fp = fopen("/proc/diskstats", "r");
	while (fgets(line, sizeof(line), fp)) {
		sscanf(line,
		       "%u %u %s %lu %lu %lu %lu %lu %lu %lu %u %u %u %u",
		       &major, &minor, dev_name,
		       &state->rd_ios, &rd_merges, &state->rd_sec,
		       &rd_ticks, &state->wr_ios, &wr_merges,
		       &state->wr_sec, &wr_ticks,
		       &ios_pgr, &state->tot_ticks, &rq_ticks);
                if (strcmp(dev_name, "sda") == 0) {
			/*
			 * tot_ticks is "number of milliseconds spent
			 * doing I/Os". Look at Documentation/iostats.txt.
			 * Or at genhd.c:diskstats_show(), which calls
			 * jiffies_to_msecs() on this field before printing
			 * it. Convert this to hz, so we can do all our math
			 * in ticks.
			 */
			state->tot_ticks /= 1000; /* to seconds */
			state->tot_ticks *= hz;   /* to hz	*/
			fclose(fp);
			return;
		}
	}
        fprintf(stderr, "%s: Did not find device sda in /proc/diskstats\n",
		progname);
	exit(1);
}

static void
read_cpu_util_state(struct cpu_disk_util_stats *state)
{
	FILE *fp;
	char line[BUFSIZE], cpu[BUFSIZE];

	/* Read and parse /proc/stat */
	fp = fopen("/proc/stat", "r");
	if (fgets(line, sizeof(line), fp) == NULL) {
		fprintf(stderr, "%s: Cannot read /proc/stat\n",
			progname);
		exit(1);
	}
	fclose(fp);
	sscanf(line, "%s %ju %ju %ju %ju %ju %ju %ju",
	       cpu,
	       &state->user_cpu_ticks,
	       &state->nice_cpu_ticks,
	       &state->system_cpu_ticks,
	       &state->idle_cpu_ticks,
	       &state->iowait_cpu_ticks,
	       &state->hardirq_cpu_ticks,
	       &state->softirq_cpu_ticks);
}

void
capture_util_state_before(void)
{
	get_HZ();
	read_disk_util_state(&before);
	read_cpu_util_state(&before);
}

extern int verbose;

void
report_cpu_disk_util(void)
{
        double disk_util, cpu_util;
	u_int64_t tot1, tot2, delta1, delta2;

	read_disk_util_state(&after);
	read_cpu_util_state(&after);
	/* CPU Util */
	tot2 = after.user_cpu_ticks + after.nice_cpu_ticks +
		after.system_cpu_ticks + after.hardirq_cpu_ticks +
		after.softirq_cpu_ticks;
	tot1 = before.user_cpu_ticks + before.nice_cpu_ticks +
		before.system_cpu_ticks + before.hardirq_cpu_ticks +
		before.softirq_cpu_ticks;
	delta1 = tot2 - tot1;
	tot2 += after.iowait_cpu_ticks + after.idle_cpu_ticks;
	tot1 += before.iowait_cpu_ticks + before.idle_cpu_ticks;
	delta2 = tot2 - tot1;
	cpu_util = delta1 * 100.0 / delta2;
	printf("CPU util = %.2f%%\n", cpu_util);
	/* Next compute system (incl irq/softirq) and user cpu util */
	delta1 = (after.user_cpu_ticks + after.nice_cpu_ticks) -
		(before.user_cpu_ticks + before.nice_cpu_ticks);
	cpu_util = delta1 * 100.0 / delta2;
	printf("User CPU util = %.2f%%\n", cpu_util);
	delta1 = (after.system_cpu_ticks + after.hardirq_cpu_ticks +
		  after.softirq_cpu_ticks) -
		(before.system_cpu_ticks + before.hardirq_cpu_ticks +
		 before.softirq_cpu_ticks);
	cpu_util = delta1 * 100.0 / delta2;
	printf("System CPU util = %.2f%%\n", cpu_util);	
	/* Disk Util */
	disk_util = (after.tot_ticks - before.tot_ticks) * 100.0 /
		(after.uptime - before.uptime);
	if (verbose) {
		printf("Reads : nr_ios %lu, MB read %lu\n",
	       (after.rd_ios - before.rd_ios),
	       (after.rd_sec - before.rd_sec) / 2048);
		printf("Writes : nr_ios %lu, MB written %lu\n",
	       (after.wr_ios - before.wr_ios),
		       (after.wr_sec - before.wr_sec) / 2048);
	}
	printf("Disk util = %.2f%%\n", disk_util);
}
