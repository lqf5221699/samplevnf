/*
// Copyright (c) 2017 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#include <fcntl.h>
#include <unistd.h>

#include <rte_common.h>
#include <rte_malloc.h>

#include <cmdline_parse.h>
#include <cmdline_parse_string.h>
#include <cmdline_socket.h>
#include <cmdline.h>

#include "app.h"
#include "pipeline_master_be.h"

struct pipeline_master {
	struct app_params *app;
	struct cmdline *cl;
	int script_file_done;
} __rte_cache_aligned;

static void*
pipeline_init(__rte_unused struct pipeline_params *params, void *arg)
{
	struct app_params *app = (struct app_params *) arg;
	struct pipeline_master *p;
	uint32_t size;

	/* Check input arguments */
	if (app == NULL)
		return NULL;

	/* Memory allocation */
	size = RTE_CACHE_LINE_ROUNDUP(sizeof(struct pipeline_master));
	p = rte_zmalloc(NULL, size, RTE_CACHE_LINE_SIZE);
	if (p == NULL)
		return NULL;

	/* Initialization */
	p->app = app;

	p->cl = cmdline_stdin_new(app->cmds, "pipeline> ");
	if (p->cl == NULL) {
		rte_free(p);
		return NULL;
	}

	p->script_file_done = 0;
	if (app->script_file == NULL)
		p->script_file_done = 1;

	return (void *) p;
}

static int
pipeline_free(void *pipeline)
{
	struct pipeline_master *p = (struct pipeline_master *) pipeline;

	if (p == NULL)
		return -EINVAL;

	cmdline_stdin_exit(p->cl);
	rte_free(p);

	return 0;
}

static int
pipeline_run(void *pipeline)
{
	struct pipeline_master *p = (struct pipeline_master *) pipeline;
	int status;

	if (p->script_file_done == 0) {
		struct app_params *app = p->app;
		int fd = open(app->script_file, O_RDONLY);

		if (fd < 0)
			printf("Cannot open CLI script file \"%s\"\n",
				app->script_file);
		else {
			struct cmdline *file_cl;

			printf("Running CLI script file \"%s\" ...\n",
				app->script_file);
			file_cl = cmdline_new(p->cl->ctx, "", fd, 1);
			cmdline_interact(file_cl);
			close(fd);
		}

		p->script_file_done = 1;
	}

	status = cmdline_poll(p->cl);
	if (status < 0)
		rte_panic("CLI poll error (%" PRId32 ")\n", status);
	else if (status == RDLINE_EXITED) {
		cmdline_stdin_exit(p->cl);
		rte_exit(0, "Bye!\n");
	}

	return 0;
}

static int
pipeline_timer(__rte_unused void *pipeline)
{
	rte_timer_manage();
	return 0;
}

struct pipeline_be_ops pipeline_master_be_ops = {
		.f_init = pipeline_init,
		.f_free = pipeline_free,
		.f_run = pipeline_run,
		.f_timer = pipeline_timer,
		.f_track = NULL,
};
