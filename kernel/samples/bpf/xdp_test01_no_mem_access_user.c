/* Copyright(c) 2017 Jesper Dangaard Brouer, Red Hat, Inc.
 */
static const char *__doc__=
 " XDP test01: Speed when not touching packet memory";

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <locale.h>

#include <sys/resource.h>
#include <getopt.h>

#include <arpa/inet.h>

#include "bpf_load.h"
#include "bpf_util.h"
#include "libbpf.h"

static int ifindex = -1;

/* Exit return codes */
#define EXIT_OK                 0
#define EXIT_FAIL               1
#define EXIT_FAIL_OPTION        2
#define EXIT_FAIL_XDP           3

static void int_exit(int sig)
{
	fprintf(stderr, "Interrupted: Removing XDP program on ifindex:%d\n",
		ifindex);
	if (ifindex > -1)
		set_link_xdp_fd(ifindex, -1);
	exit(EXIT_OK);
}

static const struct option long_options[] = {
	{"help",	no_argument,		NULL, 'h' },
	{"ifindex",	required_argument,	NULL, 'i' },
	{"sec", 	required_argument,	NULL, 's' },
	{"action", 	required_argument,	NULL, 'a' },
	{0, 0, NULL,  0 }
};

static void usage(char *argv[])
{
	int i;
	printf("\nDOCUMENTATION:\n%s\n", __doc__);
	printf("\n");
	printf(" Usage: %s (options-see-below)\n",
	       argv[0]);
	printf(" Listing options:\n");
	for (i = 0; long_options[i].name != 0; i++) {
		printf(" --%-12s", long_options[i].name);
		if (long_options[i].flag != NULL)
			printf(" flag (internal value:%d)",
			       *long_options[i].flag);
		else
			printf(" short-option: -%c",
			       long_options[i].val);
		printf("\n");
	}
	printf("\n");
}

struct stats_record {
	__u64 data[1];
	__u64 action;
};

#define XDP_ACTION_MAX (XDP_TX + 1)
#define XDP_ACTION_MAX_STRLEN 11
static const char *xdp_action_names[XDP_ACTION_MAX] = {
	[XDP_ABORTED]	= "XDP_ABORTED",
	[XDP_DROP]	= "XDP_DROP",
	[XDP_PASS]	= "XDP_PASS",
	[XDP_TX]	= "XDP_TX",
};
static const char *action2str(int action)
{
	if (action < XDP_ACTION_MAX)
		return xdp_action_names[action];
	return NULL;
}

static __u64 get_xdp_action(void)
{
	__u64 value;
	__u32 key = 0;

	/* map_fd[1] == map(xdp_action) */
	if ((bpf_map_lookup_elem(map_fd[1], &key, &value)) != 0) {
		printf("%s(): bpf_map_lookup_elem failed\n", __func__);
		exit(EXIT_FAIL_XDP);
	}
	return value;
}

static bool set_xdp_action(__u64 action)
{
	__u64 value = action;
	__u32 key = 0;

	/* map_fd[1] == map(xdp_action) */
	if ((bpf_map_update_elem(map_fd[1], &key, &value, BPF_ANY)) != 0) {
		printf("%s(): bpf_map_update_elem failed\n", __func__);
		return false;
	}
	return true;
}

static int parse_xdp_action(char *action_str)
{
	size_t maxlen;
	__u64 action = -1;
	int i;

	for (i = 0; i < XDP_ACTION_MAX; i++) {
		maxlen = strnlen(xdp_action_names[i], XDP_ACTION_MAX_STRLEN);
		if (strncmp(xdp_action_names[i], action_str, maxlen) == 0) {
			action = i;
			break;
		}
	}
	return action;
}

static void list_xdp_action(void)
{
	int i;

	printf("Available XDP --action <options>\n");
	for (i = 0; i < XDP_ACTION_MAX; i++) {
		printf("\t%s\n", xdp_action_names[i]);
	}
	printf("\n");
}


static bool stats_collect(struct stats_record *record)
{
	unsigned int nr_cpus = bpf_num_possible_cpus();
	__u64 values[nr_cpus];
	__u32 key = 0;
	__u64 sum = 0;
	int i;

	/* Notice map is percpu: BPF_MAP_TYPE_PERCPU_ARRAY */
	if ((bpf_map_lookup_elem(map_fd[0], &key, values)) != 0) {
		printf("DEBUG: bpf_map_lookup_elem failed\n");
		return false;
	}
	/* Sum values from each CPU */
	for (i = 0; i < nr_cpus; i++) {
		sum += values[i];
	}
	record->data[key] = sum;

	return true;
}

static void stats_poll(int interval)
{
	struct stats_record record;
	__u64 prev = 0, count;
	__u64 pps;

	memset(&record, 0, sizeof(record));

	/* Read current XDP action */
	record.action = get_xdp_action();

	/* Trick to pretty printf with thousands separators use %' */
	setlocale(LC_NUMERIC, "en_US");

	while (1) {
		if (!stats_collect(&record))
			exit(EXIT_FAIL_XDP);

		count = record.data[0];
		pps = (count - prev)/interval;
		printf("XDP action: %s : %llu pps (%'llu pps)\n",
		       action2str(record.action), pps, pps);
		// TODO: add nanosec accuracy measurement

		prev = count;
		sleep(interval);
		// TODO: measure time, don't rely on accuracy of sleep()
	}
}

int main(int argc, char **argv)
{
	struct rlimit r = {RLIM_INFINITY, RLIM_INFINITY};
	char *action_str = NULL;
	int action = XDP_DROP; /* Default action */
	char filename[256];
	int longindex = 0;
	int interval = 1;
	int opt;

	snprintf(filename, sizeof(filename), "%s_kern.o", argv[0]);

	/* Parse commands line args */
	while ((opt = getopt_long(argc, argv, "hi:s:a:",
				  long_options, &longindex)) != -1) {
		switch (opt) {
		case 'i':
			ifindex = atoi(optarg);
			break;
		case 's':
			interval = atoi(optarg);
			break;
		case 'a':
			action_str = optarg;
			break;
		case 'h':
		default:
			usage(argv);
			list_xdp_action();
			return EXIT_FAIL_OPTION;
		}
	}
	/* Required options */
	if (ifindex == -1) {
		printf("**Error**: required option --ifindex missing");
		usage(argv);
		return EXIT_FAIL_OPTION;
	}

	/* Parse action string */
	if (action_str) {
		action = parse_xdp_action(action_str);
		if (action < 0) {
			printf("**Error**: Invalid XDP action\n");
			usage(argv);
			list_xdp_action();
			return EXIT_FAIL_OPTION;
		}
	}

	/* Increase resource limits */
	if (setrlimit(RLIMIT_MEMLOCK, &r)) {
		perror("setrlimit(RLIMIT_MEMLOCK, RLIM_INFINITY)");
		return EXIT_FAIL;
	}

	if (load_bpf_file(filename)) {
		printf("%s", bpf_log_buf);
		return EXIT_FAIL;
	}

	if (!prog_fd[0]) {
		printf("load_bpf_file: %s\n", strerror(errno));
		return EXIT_FAIL;
	}

	set_xdp_action(action);

	/* Remove XDP program when program is interrupted */
	signal(SIGINT, int_exit);

	if (set_link_xdp_fd(ifindex, prog_fd[0]) < 0) {
		printf("link set xdp fd failed\n");
		return EXIT_FAIL_XDP;
	}

	stats_poll(interval);

	return EXIT_OK;
}