/*
 *  Copyright (C) 2019 Cisco Systems, Inc. and/or its affiliates. All rights reserved.
 *
 *  Authors: Mickey Sola
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA 02110-1301, USA.
 */

#if HAVE_CONFIG_H
#include "clamav-config.h"
#endif

#if defined(FANOTIFY)

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <string.h>

#include "../misc/onaccess_others.h"

#include "libclamav/clamav.h"

#include "shared/optparser.h"
#include "shared/output.h"

#include "../c-thread-pool/thpool.h"

#include "onaccess_scth.h"
#include "onaccess_scque.h"

static void onas_scanque_exit(int sig);
static int onas_consume_event(threadpool thpool);
static cl_error_t onas_new_event_queue_node(struct onas_event_queue_node **node);
static void onas_destroy_event_queue_node(struct onas_event_queue_node *node);

static pthread_mutex_t onas_queue_lock = PTHREAD_MUTEX_INITIALIZER;
extern pthread_t scque_pid;

static threadpool g_thpool;

static struct onas_event_queue_node *g_onas_event_queue_head = NULL;
static struct onas_event_queue_node *g_onas_event_queue_tail = NULL;

static struct onas_event_queue g_onas_event_queue;

static cl_error_t onas_new_event_queue_node(struct onas_event_queue_node **node) {

	*node = malloc(sizeof(struct onas_event_queue_node));
	if (NULL == *node) {
		return CL_EMEM;
	}


	**node = (struct onas_event_queue_node) {
		.next = NULL,
		.prev = NULL,

		.data = NULL
	};

	return CL_SUCCESS;
}

static void *onas_init_event_queue() {

	if (CL_EMEM == onas_new_event_queue_node(&g_onas_event_queue_head)) {
		return NULL;
	}

	if (CL_EMEM == onas_new_event_queue_node(&g_onas_event_queue_tail)) {
		return NULL;
	}

	g_onas_event_queue_tail->prev = g_onas_event_queue_head;
	g_onas_event_queue_head->next = g_onas_event_queue_tail;

	g_onas_event_queue = (struct onas_event_queue)  {
		.head = g_onas_event_queue_head,
		.tail = g_onas_event_queue_tail,

		.size = 0
	};

	return &g_onas_event_queue;
}

static void onas_destroy_event_queue_node(struct onas_event_queue_node *node) {

	if (NULL == node) {
		return;
	}

	node->next = NULL;
	node->prev = NULL;
	node->data = NULL;

	free(node);
	node = NULL;

	return;
}

static void onas_destroy_event_queue() {

	struct onas_event_queue_node *curr = g_onas_event_queue_head;
	struct onas_event_queue_node *next = curr->next;

	do {
		onas_destroy_event_queue_node(curr);
		curr = next;
		if (curr) {
			next = curr->next;
		}
	} while (curr);

	return;
}


void *onas_scanque_th(void *arg) {

	/* not a ton of use for context right now, but perhaps in the future we can pass in more options */
	struct onas_context *ctx = (struct onas_context *) arg;
	sigset_t sigset;
	struct sigaction act;
	const struct optstruct *pt;
	int ret, len, idx;

        cl_error_t err;

	/* ignore all signals except SIGUSR1 */
	sigfillset(&sigset);
	sigdelset(&sigset, SIGUSR2);
	/* The behavior of a process is undefined after it ignores a
	 * SIGFPE, SIGILL, SIGSEGV, or SIGBUS signal */
	sigdelset(&sigset, SIGFPE);
	sigdelset(&sigset, SIGILL);
	sigdelset(&sigset, SIGSEGV);
	sigdelset(&sigset, SIGINT);
#ifdef SIGBUS
	sigdelset(&sigset, SIGBUS);
#endif
	pthread_sigmask(SIG_SETMASK, &sigset, NULL);
	memset(&act, 0, sizeof(struct sigaction));
	act.sa_handler = onas_scanque_exit;
	sigfillset(&(act.sa_mask));
	sigaction(SIGUSR2, &act, NULL);
	sigaction(SIGSEGV, &act, NULL);

	logg("*ClamQueue: initializing event queue consumer ... (%d) threads in thread pool\n", ctx->maxthreads);
        onas_init_event_queue();
        threadpool thpool = thpool_init(ctx->maxthreads);
	g_thpool = thpool;

        /* loop w/ onas_consume_event until we die */
	logg("*ClamQueue: waiting to consume events ...\n");
	do {
		/* if there's no event to consume ... */
		if (!onas_consume_event(thpool)) {
			/* sleep for a bit */
			usleep(1000);
		}
	} while(1);

}

static int onas_queue_is_b_empty() {

    if (g_onas_event_queue.head->next == g_onas_event_queue.tail) {
        return 1;
    }

    return 0;
}

static int onas_consume_event(threadpool thpool) {

    pthread_mutex_lock(&onas_queue_lock);

    struct onas_event_queue_node *popped_node = g_onas_event_queue_head->next;

    if (onas_queue_is_b_empty()) {
        pthread_mutex_unlock(&onas_queue_lock);
        return 1;
    }

#ifdef ONAS_DEBUG
    logg("*ClamonQueue: consuming event!\n");
#endif

    thpool_add_work(thpool, (void *) onas_scan_worker, (void *) popped_node->data);

    g_onas_event_queue_head->next = g_onas_event_queue_head->next->next;
    g_onas_event_queue_head->next->prev = g_onas_event_queue_head;

    onas_destroy_event_queue_node(popped_node);

    g_onas_event_queue.size--;

    pthread_mutex_unlock(&onas_queue_lock);
    return 0;
}

cl_error_t onas_queue_event(struct onas_scan_event *event_data) {


    pthread_mutex_lock(&onas_queue_lock);

    struct onas_event_queue_node *node = NULL;

#ifdef ONAS_DEBUG
    logg("*ClamonQueue: queueing event!\n");
#endif

    if (CL_EMEM == onas_new_event_queue_node(&node)) {
	    return CL_EMEM;
    }

    node->next = g_onas_event_queue_tail;
    node->prev = g_onas_event_queue_tail->prev;

    node->data = event_data;

    /* tail will always have a .prev */
    ((struct onas_event_queue_node *) g_onas_event_queue_tail->prev)->next = node;
    g_onas_event_queue_tail->prev = node;

    g_onas_event_queue.size++;

    pthread_mutex_unlock(&onas_queue_lock);

    return CL_SUCCESS;
}

cl_error_t onas_scanque_start(struct onas_context **ctx) {

	pthread_attr_t scque_attr;
	int32_t thread_started = 1;

	if (!ctx || !*ctx) {
		logg("*ClamQueue: unable to start clamonacc. (bad context)\n");
		return CL_EARG;
	}

        if(pthread_attr_init(&scque_attr)) {
            return CL_BREAK;
        }
        pthread_attr_setdetachstate(&scque_attr, PTHREAD_CREATE_JOINABLE);
	thread_started = pthread_create(&scque_pid, &scque_attr, onas_scanque_th, *ctx);

	if (0 != thread_started) {
		/* Failed to create thread */
		logg("*ClamQueue: Unable to start event consumer queue thread ... \n");
		return CL_ECREAT;
	}

	return CL_SUCCESS;
}

static void onas_scanque_exit(int sig) {

	logg("*ClamScanque: onas_scanque_exit(), signal %d\n", sig);

	onas_destroy_event_queue();
        if (g_thpool) {
            thpool_destroy(g_thpool);
        }
        g_thpool = NULL;

	logg("ClamScanque: stopped\n");
	pthread_exit(NULL);
}

#endif
