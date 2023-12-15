#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "queue.h"
#include "rc.h"

typedef struct t_args_t{
    int index;
    RC_STRING *path;
    bool done;
    pthread_cond_t signal;
    pthread_mutex_t lock;
    struct t_args_t *args_array;
} thread_args_t;

int populate_list(RC_STRINGLIST **list, int argc, char **argv)
{
    FILE *fp;
    char path[4096]; // https://unix.stackexchange.com/questions/32795/what-is-the-maximum-allowed-filename-and-folder-size-with-ecryptfs
    int size;
    char cmd[4096] = "/lib/rc/bin/mountinfo";

    /* Craft the command with all passed arguments but the first*/
    for (int i = 2; i < argc; i++) {
        printf("Argument: %s\n", argv[i]);
        strcat(cmd, " \"");
        strcat(cmd, argv[i]);
        strcat(cmd, "\"");
    }

    /* Open the command for reading */
    fp = popen(cmd, "r");
    if (fp == NULL)
    {
        printf("Failed to run command\n");
        exit(1);
    }

    *list = rc_stringlist_new();
    size = 0;
    /* Read the output a line at a time */
    while (fgets(path, sizeof(path), fp) != NULL)
    {
        path[strlen(path) - 1] = '\0'; // remove trailing '\n'
        rc_stringlist_add(*list, path);
        size++;
    }

    /* close */
    pclose(fp);

    return size;
}


/*Execute the unmount of the provided path*/
void *unmount_one(void *input)
{
    thread_args_t *args = (thread_args_t *)input;
    RC_STRING *prev;
    int i;

    //printf("Unmounting %s\n", args->path->value);
    prev = args->path;
    for(i = args->index-1; i >= 0; i--) {
        prev = TAILQ_PREV(prev, rc_stringlist, entries);
        //printf("Previous: %s at index %d ", prev->value, i);
        // get index of first different char 
        int j = 0;
        while (args->path->value[j] == prev->value[j]) {
            j++;
        }
        //printf("First different char at: %d ", j);
        //printf("First different char: %d\n", (int) args->path->value[j]);
        // if the first different char is a '/' then we have a child
        if (prev->value[j] == '/') {
            //printf("Child: %s\n", prev->value);
            /* Wait for child to unmount */
            pthread_mutex_lock(&args->args_array[i].lock);
            while (args->args_array[i].done == false) {
                pthread_cond_wait(&args->args_array[i].signal, &args->args_array[i].lock);
            }
            pthread_mutex_unlock(&args->args_array[i].lock);
        }
    }

    char command[4096];
    sprintf(command, "umount %s", args->path->value);
    //system(command);
    pthread_mutex_lock(&args->lock);
    args->done = true;
    printf("Done %s\n", args->path->value);
    fflush(stdout);
    pthread_cond_signal(&args->signal);
    pthread_mutex_unlock(&args->lock);
}

int main(int argc, char **argv)
{
    RC_STRINGLIST *list;
    RC_STRING *path;
    int size, i;

    printf("Starting Unmount!\n");

    size = populate_list(&list, argc, argv);

    pthread_t threads_list[size];
    thread_args_t args[size];

    i = 0;
    TAILQ_FOREACH(path, list, entries)
    {
        args[i].index = i;
        args[i].path = path;
        args[i].done = false;
        pthread_cond_init(&args[i].signal, NULL);
        pthread_mutex_init(&args[i].lock, NULL);
        args[i].args_array = args;
        pthread_create(threads_list + i, NULL, unmount_one, args + i);
        i++;
    }

    for (i = 0; i < size; i++)
        pthread_join(threads_list[i], NULL);

    rc_stringlist_free(list);

    printf("Unmounted %d filesystems!\n", size);

    return 0;
}
