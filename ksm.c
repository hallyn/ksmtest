#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <fcntl.h>

void usage(char *me)
{
	printf("Usage: %s [-f filetomap] [-n ntasks] [-m memory]\n", me);
	printf("   ntasks: number of tasks to spawn\n");
	printf("           defaults to 5\n");
	printf("   mem: memory to map, in Megabytes\n");
	printf("        defaults to 100M\n");
	printf("   filetomap: file to map into the ksm-mergable map\n");
	printf("        defaults to /boot/initrd*\n");
	exit(1);
}

int *pids;
unsigned long mem = 100, ntasks = 5;
void stop_tests(int sig)
{
	int i, p;
	int status;
	for (i = 0;  i < ntasks; i++) {
		kill(pids[i], SIGTERM);
		if ((p = waitpid(pids[i], &status, 0)) != pids[i]) {
			printf("Warning: %d may not have exited properly\n",
				pids[i]);
		}
	}
	exit(1);
}

char *filetomap;
void *filecontents;
size_t filesize;
int ncopies;

void docopy(void *m, size_t half, size_t sz)
{
	int i, fd;
	struct stat st;
	int ret;

	if ((fd = open(filetomap, O_RDONLY)) < 0) {
		perror("open filetomap");
		exit(1);
	}
	ret = fstat(fd, &st);
	if (ret) {
		perror("stat");
		exit(1);
	}
	filesize = st.st_size;
	ncopies = st.st_size / half;

	filecontents = malloc(st.st_size);
	if (!filecontents) {
		perror("malloc");
		exit(1);
	}

	ret = read(fd, filecontents, st.st_size);
	if (ret != st.st_size) {
		perror("read");
		exit(1);
	}

	for (i = 0;  i < ncopies;  i++)
		memcpy(m + (i * st.st_size), filecontents, st.st_size);
	close(fd);
	printf("Child %d: sucessfully read %s\n", getpid(), filetomap);
}

void verifycopy(void *m, size_t half, size_t sz)
{
	int i, fd;
	struct stat st;
	int ret;

	for (i = 0;  i < ncopies;  i++) {
		if (memcmp(m + (i * filesize), filecontents, filesize) != 0) {
			printf("Child %d: file corruption at %p..%p\n",
				getpid(), m + (i * filesize),
				m + ((i + 1) * filesize) - 1);
			exit(1);
		}
	}
	close(fd);
}

/*
 * This is the actual test
 * mmap @mem M of anon private pages;  fill half of it with 0s,
 * and half of it with copies of /boot/initrd.
 * 
 * Then every 60 seconds, verify the memory contents remain correct.
 */
void run_ksm_test(void)
{
	char *m;
	size_t sz, half;
	int ret;

	sz = mem * 1000000;
	half = sz / 2;

	m = mmap(NULL, sz, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANON, -1, 0);
	if (m == MAP_FAILED) {
		printf("Child %d; failed mmap!\n", getpid());
		exit(1);
	}

	/*
	 * Fill in pages
	 * 0..half-1 is filled in with zeros
	 * half .. sz is filled in with copies of filetomap
	 */
	memset(m, 0, half);
	docopy(m, half, sz);

	/* mark them mergable */
	ret = madvise(m, sz, MADV_MERGEABLE);
	if (ret) {
		perror("madvise");
		printf("Child %d: failed to mark pages mergable\n", getpid());
		exit(1);
	}

	/* now loop, occasionally checking validity */
	while (1) {
		sleep(60);
		char *p;
		for (p = m; p < m + half; p++) {
			if (*p) {
				printf("Child %d: Corruption: byte %lu is not 0!\n", 
					getpid(), p-m);
				exit(1);
			}
		}
		verifycopy(m, half, sz);
	}
}

void print_ksmenabled(void)
{
	int ret = 0, v;
	FILE *f = fopen("/sys/kernel/mm/ksm/run", "r");
	if (f) {
		ret = fscanf(f, "%d", &v);
		fclose(f);
	}
	if (ret != 1 || (v != 0 && v != 1)) {
		printf("/sys/kernel/mm/ksm/run seems bogus\n");
		exit(1);
	}
	printf("ksm enabled: %d\n", v);
}

void print_numaenabled(void)
{
	int ret = 0, v;
	FILE *f = fopen("/sys/kernel/mm/ksm/merge_across_nodes", "r");
	if (f) {
		ret = fscanf(f, "%d", &v);
		fclose(f);
	}
	if (ret != 1 || (v != 0 && v != 1)) {
		printf("/sys/kernel/mm/ksm/merge_across_nodes seems bogus\n");
		exit(1);
	}
	printf("ksm merge across numa nodes enabled: %d\n", v);
}

void print_ksm_shared(void)
{
	unsigned long pages_shared = 0, pages_sharing = 0, full_scans = 0, pages_unshared = 0, pages_volatile = 0;
	FILE *f;

	f = fopen("/sys/kernel/mm/ksm/pages_shared", "r");
	if (f) {
		fscanf(f, "%lu", &pages_shared);
		fclose(f);
	}
	f = fopen("/sys/kernel/mm/ksm/pages_sharing", "r");
	if (f) {
		fscanf(f, "%lu", &pages_sharing);
		fclose(f);
	}
	f = fopen("/sys/kernel/mm/ksm/pages_unshared", "r");
	if (f) {
		fscanf(f, "%lu", &pages_unshared);
		fclose(f);
	}
	f = fopen("/sys/kernel/mm/ksm/pages_volatile", "r");
	if (f) {
		fscanf(f, "%lu", &pages_volatile);
		fclose(f);
	}
	f = fopen("/sys/kernel/mm/ksm/full_scans", "r");
	if (f) {
		fscanf(f, "%lu", &full_scans);
		fclose(f);
	}
	printf("KSM status:\n");
	printf("  Full scans: %lu\n", full_scans);
	printf("  Pages shared: %lu\n", pages_shared);
	printf("  Pages unshared: %lu\n", pages_unshared);
	printf("  Pages sharing: %lu\n", pages_sharing);
	printf("  Pages volatile: %lu\n", pages_volatile);
}

void verify_pids_alive(void)
{
	int ret, status;

	ret = waitpid(-1, &status, WNOHANG);
	if (ret == 0)
		return;
	if (ret == -1) {
		printf("XXX WARNING: waitpid returned error: %m\n");
		return;
	}
	printf("Warning: child pid %d exited\n", ret);
}

void get_filetomap(void)
{
	struct dirent *dirent;
	DIR *d = opendir("/boot");

	if (!d)
		return;
	while ((dirent = readdir(d)) != NULL) {
		if (strncmp(dirent->d_name, "initrd", 6) == 0)
			break;
	}
	if (dirent) {
		filetomap = malloc(strlen(dirent->d_name) + 7);
		if (!filetomap)
			exit(1);
		sprintf(filetomap, "/boot/%s", dirent->d_name);
	}
	closedir(d);
}

int main(int argc, char *argv[])
{
	char *me = argv[0];
	int i, flags, opt;

	while ((opt = getopt(argc, argv, "n:m:")) != -1) {
		switch (opt) {
		case 'n':
			ntasks = strtoul(optarg, NULL, 10);
			break;
		case 'm':
			mem = strtoul(optarg, NULL, 10);
			break;
		case 'f':
			filetomap = optarg;
			break;
		default:
			printf("Unknown arg: %c\n", opt);
			usage(me);
		}
	}

	if (!filetomap)
		get_filetomap();
	if (!filetomap) {
		printf("Failed to find a /boot/initrd to map\n");
		printf("Please provide a file using -f\n");
		usage(me);
	}

	print_ksmenabled();
	print_numaenabled();

	if (ntasks > 100) {
		printf("are you sure you wanted %lu tasks?\n", ntasks);
		printf("sleeping 20 seconds so you can ctrl-c\n");
		sleep(20);
	}
	pids = malloc(ntasks * sizeof(int));
	if (!pids)
		exit(1);
	for (i = 0;  i < ntasks; i++) {
		pids[i] = fork();
		if (pids[i] < 0) {
			printf("Error forking\n");
			exit(1);
		}
		if (pids[i] == 0) {
			run_ksm_test();
			exit(1);
		}
	}

	signal(SIGINT, stop_tests);

	while (1) {
		print_ksm_shared();
		verify_pids_alive();
		sleep(60);
	}
}