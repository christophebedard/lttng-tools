/*
 * Copyright (C) - 2017 Francis Deslauriers <francis.deslauriers@efficios.com>
 * Copyright (C) - 2018 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "utils.h"

#define LTTNG_MODULES_FILE "/proc/lttng-test-filter-event"

/*
 * The process waits for the creation of a file passed as argument from an
 * external processes to execute a syscall and exiting. This is useful for tests
 * in combinaison with LTTng's PID tracker feature where we can trace the kernel
 * events generated by our test process only.
 */
int main(int argc, char **argv)
{
	int fd = -1, ret;
	char *start_file, *nr_events_str;
	ssize_t len;

	if (argc != 3) {
		fprintf(stderr, "Error: Missing argument\n");
		fprintf(stderr, "USAGE: %s PATH_WAIT_FILE NR_EVENTS\n",
				argv[0]);
		ret = -1;
		goto end;
	}

	start_file = argv[1];
	nr_events_str = argv[2];

	/*
	 * Wait for the start_file to be created by an external process
	 * (typically the test script) before executing the syscalls.
	 */
	ret = wait_on_file(start_file);
	if (ret != 0) {
		goto end;
	}

	/*
	 * Start generating events.
	 */
	fd = open(LTTNG_MODULES_FILE, O_RDWR);
	if (fd < 0) {
		perror("open");
		ret = -1;
		goto end;
	}

	len = write(fd, nr_events_str, strlen(nr_events_str) + 1);
	if (len != strlen(nr_events_str) + 1) {
		perror("write");
		ret = -1;
	} else {
		ret = 0;
	}

end:
	if (fd >= 0) {
		int closeret = close(fd);
		if (closeret) {
			perror("close");
		}
	}
	return ret;
}
