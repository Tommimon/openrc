/*
 * Copyright (c) 2007-2015 The OpenRC Authors.
 * See the Authors file at the top-level directory of this distribution and
 * https://github.com/OpenRC/openrc/blob/HEAD/AUTHORS
 *
 * This file is part of OpenRC. It is subject to the license terms in
 * the LICENSE file found in the top-level directory of this
 * distribution and at https://github.com/OpenRC/openrc/blob/HEAD/LICENSE
 * This file may not be copied, modified, propagated, or distributed
 * except according to the terms contained in the LICENSE file.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "helpers.h"
#include "queue.h"
#include "rc.h"

/* Contains all parameters passed to unmount_one() function */
typedef struct t_args_t
{
    int index;                   // index of the path in the list
    RC_STRING *path;             // path to unmount
    pthread_mutex_t unmounting;  // mutex locked until the path is unmounted
    char* command;               // unmounting command to execute
    struct t_args_t *args_array; // array of all parameters of all threads
} thread_args_t;

/* Pass arguments to a command and open standard output as readable file */
FILE *popen_with_args(const char *command, int argc, char **argv) {
    char *cmd;  // command with all arguments
    int length; // length of the command with all arguments
    int i;      // iterator
    FILE *fp;   // file pointer to the output of the command

    /* Calculate the length of the command */
    length = strlen(command) + 1; // +1 for the null terminator
    for (i = 2; i < argc; i++)
        length += strlen(argv[i]) + 3; // +3 for the quotes and space
    
    /* Allocate memory for the command */
    cmd = xmalloc(length * sizeof(char));
    sprintf(cmd, "%s", command);

    /* Craft the command with all passed arguments */
    for (i = 2; i < argc; i++) {
        strcat(cmd, " \"");
        strcat(cmd, argv[i]);
        strcat(cmd, "\"");
    }

    /* Open the command and return output file descriptor for reading */
    return popen(cmd, "r");;
}

/* Pass arguments to mountinfo and store output in a list of paths to unmount */
int populate_list(RC_STRINGLIST **list, int argc, char **argv)
{
    int size;        // number of paths to unmount
    FILE *fp;        // file pointer to the output of the command
    char path[4096]; // https://unix.stackexchange.com/questions/32795/what-is-the-maximum-allowed-filename-and-folder-size-with-ecryptfs

    /* Open the command for reading */
    fp = popen_with_args("mountinfo", argc, argv);
    if (fp == NULL)
    {
        printf("Failed to run mountinfo command, can't unmount anything!\n");
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

/* Execute the unmount of the provided path */
void *unmount_one(void *input)
{
    thread_args_t *args = (thread_args_t *)input;
    RC_STRING *prev;
    char *command;
    int i, j;

    /* Check all previous paths in the list for children */
    prev = args->path;
    for (i = args->index - 1; i >= 0; i--)
    {
        prev = TAILQ_PREV(prev, rc_stringlist, entries);
        /* If the first different char is a '/' then we have a child */
        for (j = 0; args->path->value[j] == prev->value[j]; j++);
        if (prev->value[j] == '/')
        {
            /* Wait for child to unmount (wait for mutex to be available) */
            pthread_mutex_lock(&args->args_array[i].unmounting);
            pthread_mutex_unlock(&args->args_array[i].unmounting);
        }
    }

    /* Allocate memory for the command */
    command = xmalloc((strlen(args->command) + strlen(args->path->value) + 2) * sizeof(char));

    /* Unmount the path */
    sprintf(command, "%s %s", args->command, args->path->value);
    system(command);
    printf("%s\n", command);  //TODO: remove this debug line and the fflush(stdout) below
    fflush(stdout);
}

/*
* Handy function to handle all our unmounting needs
* mountinfo is a C program to actually find our mounts on our supported OS's
* We rely on fuser being present, so if it's not then don't unmount anything. TODO: do we need to check for fuser?
* This isn't a real issue for the BSD's, but it is for Linux.
*/
int main(int argc, char **argv)
{
    RC_STRINGLIST *list;        // list of paths to unmount
    RC_STRING *path;            // path to unmount
    int size, i;                // size of the list and iterator
    pthread_t *threads;         // array of threads
    thread_args_t *args_array;  // array of arguments for each thread

    if (strstr(argv[1], "-r") != NULL)
        printf("Remounting filesystems readonly!\n");
    else
        printf("Unmounting filesystems!\n");

    /* Get list of paths to unmount */
    size = populate_list(&list, argc, argv);

    /* Create array of threads and array of arguments for each path in the list */
    threads = xmalloc(size * sizeof(pthread_t));
    args_array = xmalloc(size * sizeof(thread_args_t));

    /* Unmount each path in a different thread */
    i = 0;
    TAILQ_FOREACH(path, list, entries)
    {
        args_array[i].index = i;
        args_array[i].path = path;
        pthread_mutex_init(&args_array[i].unmounting, NULL);
        pthread_mutex_lock(&args_array[i].unmounting);
        args_array[i].command = argv[1];  // Assuming argv[1] is the unmounting command
        args_array[i].args_array = args_array;
        pthread_create(threads + i, NULL, unmount_one, args_array + i);
        i++;
    }

    /* Wait for all threads to finish */
    for (i = 0; i < size; i++)
    {
        pthread_join(threads[i], NULL);
        pthread_mutex_unlock(&args_array[i].unmounting);
    }

    /* Destroy mutexes */
    for (i = 0; i < size; i++)
        pthread_mutex_destroy(&args_array[i].unmounting);

    /* Free memory */
    free(threads);
    free(args_array);
    rc_stringlist_free(list);

    printf("Unmounted/Remounted %d filesystems!\n", size);  //TODO: remove this debug line

    return 0;
}
