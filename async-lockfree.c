#include "async.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>

/* suppress compilation warnings */
static inline ssize_t write_wrapper(int fd, const void *buf, size_t count)
{
    ssize_t s;
    if ((s = write(fd, buf, count)) < count) perror("write");
    return s;
}
#undef write
#define write write_wrapper

/* the actual working thread */
static void *worker_thread_cycle(void *async);

/* signaling to finish */
static void async_signal(async_p async);

/* the destructor */
static void async_destroy(async_p queue);

static void *join_thread(pthread_t thr)
{
    void *ret;
    pthread_join(thr, &ret);
    return ret;
}

static int create_thread(pthread_t *thr,
                         void *(*thread_func)(void *),
                         void *async)
{
    return pthread_create(thr, NULL, thread_func, async);
}

/** A task node */
struct AsyncTask {
    void (*task)(void *);
    void *arg;
};

#define WORK_QUEUE_POWER 10
#define WORK_QUEUE_SIZE (1<<WORK_QUEUE_POWER) /**<ring buffer size*/
#define QUEUE_MASK (WORK_QUEUE_SIZE - 1) /**<use mask to replace the mod. operation*/

#define thread_rear_val(thread) (__sync_val_compare_and_swap(&(thread->rear),0,0))
#define thread_front_val(thread) (__sync_val_compare_and_swap(&(thread->front),0,0))
#define thread_queue_len(thread) (thread_front_val(thread) - thread_rear_val(thread))
#define thread_queue_empty(thread) (thread_queue_len(thread) == 0)
#define thread_queue_full(thread) (thread_queue_len(thread) == WORK_QUEUE_SIZE)
#define queue_offset(val) (val & QUEUE_MASK)

#define SELECT_THREAD(async,i) \
    (async->threads + i)
/**threads in pool*/
struct thpool {
    pthread_t thid; /**<pthread id*/
    struct AsyncTask work_queue[WORK_QUEUE_SIZE]; /**<work queue simple ring buffer*/
    /** The pipe used for thread wakeup */
    struct {
        int in;  /**< read incoming data (opaque data), used for wakeup */
        int out; /**< write opaque data (single byte),
                      used for wakeup signaling */
    } pipe;
    volatile unsigned int rear; /**<the place to get the tasks*/
    volatile unsigned int front; /**<the place to put new tasks*/
};

/** The Async struct */
struct Async {
    /** the task queue - MUST be first in the struct */
    pthread_mutex_t lock;              /**< a mutex for data integrity */

    int count; /**< the number of initialized threads */

    unsigned run : 1; /**< the running flag */

    /** the thread pool */
    struct thpool threads[];
};

/* Task Management - add a task and perform all tasks in queue */
static struct thpool* RR(async_p async)
{
    static int cur_thread_index = -1;

    cur_thread_index = __sync_add_and_fetch(&cur_thread_index,1) %async->count; /**< avoid race condition to add cur_thread_index concurrently */
    return SELECT_THREAD(async , cur_thread_index);
}

static int dispatch_task(async_p async, struct thpool *thread, void (*task)(void *), void *arg)
{
    struct AsyncTask *c = NULL;
    while(thread_queue_full(thread)) thread = RR(async);

    c = thread->work_queue + queue_offset(__sync_fetch_and_add(&(thread->front),1)); /**< avoid race condition on put new task in buffer*/
    c->task = task;
    c->arg = arg;
    if(thread_queue_len(thread) >= 1)
        write(thread->pipe.out, c->task, 1);
    return 0;
}

static int async_run(async_p async, void (*task)(void *), void *arg, int flag)
{
    struct thpool *thread;
    if (!async || !task) return -1;

    if(flag == 1) return dispatch_task(async, async->threads+async->count, task, arg) ;
    thread = RR(async);
    return dispatch_task(async, thread, task, arg);
}

/** Performs all the existing tasks in the queue. */
static void perform_tasks(struct thpool *thread)
{
    struct AsyncTask *c = NULL;  /* c == container, will store the task */
    unsigned int tmp;
    do {
        /* grab a task from the queue. */
        if(thread_queue_empty(thread)) break;
        tmp = thread->rear++;
        c = thread->work_queue + queue_offset(tmp);
        /* perform the task */
        if (c->task) c->task(c->arg);
    } while (c);
}

/* The worker threads */

/* The worker cycle */
static void *worker_thread_cycle(void *_async)
{
    /* setup signal and thread's local-storage async variable. */
    struct Async *async = _async;
    char sig_buf;

    int i;
    for( i = 0; i <= async->count; i++)
        if(SELECT_THREAD(async,i)->thid == pthread_self()) break;

    /* pause for signal for as long as we're active. */
    while (async->run && (read(SELECT_THREAD(async,i)->pipe.in, &sig_buf, 1) >= 0)) {
        perform_tasks(SELECT_THREAD(async,i));
        sched_yield();
    }

    perform_tasks(SELECT_THREAD(async,i));
    return 0;
}

/* Signal and finish */

static void async_signal(async_p async)
{
    async->run = 0;
    /* send `async->count` number of wakeup signales.
     * data content is irrelevant. */
    for(int i = 0; i <= async->count; i++)
        write(SELECT_THREAD(async,i)->pipe.out, async, async->count);
}

static void async_wait(async_p async)
{
    if (!async) return;

    /* wake threads (just in case) by sending `async->count`
     * number of wakeups
     */
    for(int i = 0; i <= async->count; i++)
        if (SELECT_THREAD(async,i)->pipe.out)
            write(SELECT_THREAD(async,i)->pipe.out, async, async->count);
    /* join threads */
    for (int i = 0; i <= async->count; i++) {
        join_thread(SELECT_THREAD(async,i)->thid);
        /* perform any pending tasks */
        perform_tasks(SELECT_THREAD(async,i));
    }
    /* release queue memory and resources */
    async_destroy(async);
}

static void async_finish(async_p async)
{
    async_signal(async);
    async_wait(async);
}

/* Object creation and destruction */

/** Destroys the Async object, releasing its memory. */
static void async_destroy(async_p async)
{
    //pthread_mutex_lock(&async->lock);
    struct thpool *thread;
    /*free the thread pool*/
    for(int i = 0 ; i <= async->count ; i++) {
        thread = SELECT_THREAD(async,i);
        /* close pipe */
        if (thread->pipe.in) {
            close(thread->pipe.in);
            thread->pipe.in = 0;
        }
        if (thread->pipe.out) {
            close(thread->pipe.out);
            thread->pipe.out = 0;
        }
    }
    //pthread_mutex_unlock(&async->lock);
    //pthread_mutex_destroy(&async->lock);
    free(async);
}

static async_p async_create(int threads)
{
    async_p async = malloc(sizeof(*async) + (threads+1) * sizeof(struct thpool)); /**< an extra thread is used to be producer */

    /*if (pthread_mutex_init(&(async->lock), NULL)) {
        free(async);
        return NULL;
    };*/

    async->run = 1;
    async->count = threads;
    /* create threads */
    for (int i = 0; i <= threads; i++) {
        /* initialize pipe */
        SELECT_THREAD(async,i)->pipe.in = 0;
        SELECT_THREAD(async,i)->pipe.out = 0;
        if (pipe((int *) &(SELECT_THREAD(async,i)->pipe))) {
            free(async);
            return NULL;
        };
        fcntl(SELECT_THREAD(async,i)->pipe.out, F_SETFL, O_NONBLOCK | O_WRONLY);
        /* initialize work queue */
        memset(SELECT_THREAD(async,i)->work_queue,0,WORK_QUEUE_SIZE*sizeof(struct AsyncTask));
        SELECT_THREAD(async,i)->front = 0;
        SELECT_THREAD(async,i)->rear = 0;
        
        if (create_thread(&(SELECT_THREAD(async,i)->thid),
                          worker_thread_cycle, async)) {
            /* signal */
            async_signal(async);
            /* wait for threads and destroy object */
            async_wait(async);
            /* return error */
            return NULL;
        };
    }

    return async;
}

/* API gateway */
struct __ASYNC_API__ Async = {
    .create = async_create,
    .signal = async_signal,
    .wait = async_wait,
    .finish = async_finish,
    .run = async_run,
};
