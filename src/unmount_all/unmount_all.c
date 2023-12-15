#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "queue.h"
#include "rc.h"

/* Contains all parameters passed to unmount_one() function */
typedef struct t_args_t{
    int index;                    // index of the path in the list
    RC_STRING *path;              // path to unmount
    pthread_mutex_t unmounting;   // mutex locked until the path is unmounted
    struct t_args_t *args_array;  // array of all parameters of all threads
} thread_args_t;

/* Pass arguments to mountinfo and store output in a list of paths to unmount */
int populate_list(RC_STRINGLIST **list, int argc, char **argv)
{
    FILE *fp;
    char path[4096]; // https://unix.stackexchange.com/questions/32795/what-is-the-maximum-allowed-filename-and-folder-size-with-ecryptfs
    int size;
    char cmd[4096] = "/lib/rc/bin/mountinfo";  //TODO: change to mountinfo
                                               //TODO: find proper length for cmd

    /* Craft the command with all passed arguments but the first */
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

    pclose(fp);

    return size;
}


/*Execute the unmount of the provided path*/
void *unmount_one(void *input)
{
    thread_args_t *args = (thread_args_t *)input;
    RC_STRING *prev;
    char command[4096];
    int i, j;

    prev = args->path;
    for(i = args->index-1; i >= 0; i--) {
        prev = TAILQ_PREV(prev, rc_stringlist, entries);
        /* Get index of first different char */ 
        j = 0;
        while (args->path->value[j] == prev->value[j]) {
            j++;
        }
        /* If the first different char is a '/' then we have a child */
        if (prev->value[j] == '/') {
            /* Wait for child to unmount (wait for mutex to be available) */
            pthread_mutex_lock(&args->args_array[i].unmounting);
            pthread_mutex_unlock(&args->args_array[i].unmounting);
        }
    }

    /* Unmount the path */
    sprintf(command, "umount %s", args->path->value);
    //system(command);
    printf("Done %s\n", args->path->value);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    RC_STRINGLIST *list;
    RC_STRING *path;
    int size, i;

    printf("Starting Unmount!\n");

    /* Get list of paths to unmount */
    size = populate_list(&list, argc, argv);

    pthread_t threads[size];
    thread_args_t args_array[size];

    /* Unmount each path in a different thread */
    i = 0;
    TAILQ_FOREACH(path, list, entries)
    {
        args_array[i].index = i;
        args_array[i].path = path;
        pthread_mutex_init(&args_array[i].unmounting, NULL);
        pthread_mutex_lock(&args_array[i].unmounting);
        args_array[i].args_array = args_array;
        pthread_create(threads + i, NULL, unmount_one, args_array + i);
        i++;
    }

    /* Wait for all threads to finish */
    for (i = 0; i < size; i++) {
        pthread_join(threads[i], NULL);
        pthread_mutex_unlock(&args_array[i].unmounting);
    }

    rc_stringlist_free(list);

    printf("Unmounted %d filesystems!\n", size);

    return 0;
}
