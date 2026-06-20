#include "threading.h"
#include <malloc.h>
#include <stdio.h>
#include <unistd.h>

#define DEBUG_LOG(msg, ...)
#define ERROR_LOG(msg, ...) printf("threading ERROR: " msg "\n", ##__VA_ARGS__)

void *threadfunc(void *thread_param) {

    struct thread_data *thread_func_args = (struct thread_data *)thread_param;

    thread_func_args->thread_complete_success = false;

    if (usleep(thread_func_args->wait_to_obtain_ms * 1000) != 0) {
        ERROR_LOG("usleep failed before obtaining mutex");
        return thread_param;
    }

    int rc = pthread_mutex_lock(thread_func_args->mutex);
    if (rc != 0) {
        ERROR_LOG("pthread_mutex_lock failed with rc=%d", rc);
        return thread_param;
    }

    if (usleep(thread_func_args->wait_to_release_ms * 1000) != 0) {
        ERROR_LOG("usleep failed before releasing mutex");
        // Still try to release the mutex we successfully obtained
        pthread_mutex_unlock(thread_func_args->mutex);
        return thread_param;
    }

    rc = pthread_mutex_unlock(thread_func_args->mutex);
    if (rc != 0) {
        ERROR_LOG("pthread_mutex_unlock failed with rc=%d", rc);
        return thread_param;
    }

    thread_func_args->thread_complete_success = true;
    return thread_param;
}

bool start_thread_obtaining_mutex(pthread_t *thread, pthread_mutex_t *mutex,
                                  int wait_to_obtain_ms,
                                  int wait_to_release_ms) {
    if (thread == NULL || mutex == NULL) {
        ERROR_LOG("NULL thread or mutex pointer passed in");
        return false;
    }

    struct thread_data *thread_func_args =
        (struct thread_data *)malloc(sizeof(struct thread_data));
    if (thread_func_args == NULL) {
        ERROR_LOG("malloc failed for thread_data");
        return false;
    }

    thread_func_args->mutex = mutex;
    thread_func_args->wait_to_obtain_ms = wait_to_obtain_ms;
    thread_func_args->wait_to_release_ms = wait_to_release_ms;
    thread_func_args->thread_complete_success = false;

    int rc = pthread_create(thread, NULL, threadfunc, (void *)thread_func_args);
    if (rc != 0) {
        ERROR_LOG("pthread_create failed with rc=%d", rc);
        free(thread_func_args);
        return false;
    }

    return true;
}
