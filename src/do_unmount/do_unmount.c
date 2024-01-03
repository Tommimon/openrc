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
#include "einfo.h"
#include "queue.h"
#include "rc.h"

/* Forward declaration to solve circular dependency */
struct t_args_t;

/* Contains parameters shared among all threads */
typedef struct
{
    char* command;                // unmounting command to execute
    RC_STRINGLIST *shared;        // list of shared paths in the system
    pthread_mutex_t shared_lock;  // mutex to lock shared mount operations
    struct t_args_t *args_array;  // array of all parameters of all threads
} global_args_t;

/* Contains all parameters passed to unmount_one() function */
typedef struct t_args_t
{
    int index;                    // index of the path in the list
    RC_STRING *path;              // path to unmount
    pthread_mutex_t unmounting;   // mutex locked until the path is unmounted
    global_args_t *global_args;   // parameters shared among all threads
    int retval;                   // return value of the unmounting command
} thread_args_t;

/* Pass arguments to a command and open standard output as readable file */
FILE *popen_with_args(const char *command, int argc, char **argv) {
    char *cmd;  // command with all arguments
    int length; // length of the command with all arguments
    int i;      // iterator
    FILE *fp;   // file pointer to the output of the command

    /* Calculate the length of the command */
    length = strlen(command) + 1; // +1 for the null terminator
    for (i = 0; i < argc; i++)
        length += strlen(argv[i]) + 3; // +3 for the quotes and space
    
    /* Allocate memory for the command */
    cmd = xmalloc(length * sizeof(char));
    sprintf(cmd, "%s", command);

    /* Craft the command with all passed arguments */
    for (i = 0; i < argc; i++) {
        strcat(cmd, " \"");
        strcat(cmd, argv[i]);
        strcat(cmd, "\"");
    }

    /* Open the command and return output file descriptor for reading */
    return popen(cmd, "r");;
}

void populate_shared_list(RC_STRINGLIST **list) {
    FILE *fp;               // file pointer to the mountinfo file
    size_t len = 0;         // length of the line read
    char *line = NULL;      // line read from the mountinfo file
    char *token;            // token of the current line
    char *path[4096];       // path to relative to the current line
    int i;                  // iterator

    /* Initialize list */
    *list = rc_stringlist_new();

    /* Only linux supports shared mounts, so leave the list empty for eny other OS */
    #ifdef __linux__
    /* Open mountinfo file for reading */
    fp = fopen("/proc/1/mountinfo", "r");
    if (fp == NULL)
        exit(EXIT_FAILURE);

    /* Read the output a line at a time */
    while (getline(&line, &len, fp) != -1) {
        /* Split line by space */
        token = strtok(line, " ");
        i = 0;
        while (token != NULL) {
            if (i == 4)
            {
                /* Copy token into path */
                strcpy(path, token);
            }
            // if token contains "shared:" TODO: check if this is the correct way to check for shared mounts
            if (strstr(token, "shared:") != NULL)
            {
                rc_stringlist_add(*list, path);
            }
            token = strtok(NULL, " ");
            i++;
        }
    }

    /* Close file and free memory */
    fclose(fp);
    if (line)
        free(line);
    #endif
}

/* Pass arguments to mountinfo and store output in a list of paths to unmount */
int populate_unmount_list(RC_STRINGLIST **list, int argc, char **argv)
{
    int size;        // number of paths to unmount
    FILE *fp;        // file pointer to the output of the command
    char path[4096]; // https://unix.stackexchange.com/questions/32795/what-is-the-maximum-allowed-filename-and-folder-size-with-ecryptfs

    /* Open the command for reading */
    fp = popen_with_args("mountinfo", argc-2, argv+2);
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
    thread_args_t *args = (thread_args_t *)input;  // arguments passed to the thread
    RC_STRING *prev;                               // backwards iterator in the paths list
    char *command;                                 // command to execute
    char *check_command;                           // command to check mountpoint existence
    int i, j;                                      // iterators

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
            pthread_mutex_lock(&args->global_args->args_array[i].unmounting);
            pthread_mutex_unlock(&args->global_args->args_array[i].unmounting);
        }
    }

    /* Allocate memory and compose the unmount/remount command */
    command = xmalloc((strlen(args->global_args->command) + strlen(args->path->value) + 2) * sizeof(char));
    sprintf(command, "%s %s", args->global_args->command, args->path->value);

    /* If is a shared mount, avoid any other shared mount concurrency TODO: more granular control */
    if(rc_stringlist_find(args->global_args->shared, args->path->value)){
        /* Allocate memory and compose the check command */
        check_command = xmalloc((19 + strlen(args->path->value)) * sizeof(char));
        sprintf(check_command, "mountinfo --quiet %s", args->path->value);
        pthread_mutex_lock(&args->global_args->shared_lock);
        /* Unmount only if is still mounted */
        if(system(check_command) == 0)
            args->retval = exec_unmount(command);
        else
            args->retval = 0;            
        pthread_mutex_unlock(&args->global_args->shared_lock);
    }
    else
    {
        /* Unmount the path */
        args->retval = exec_unmount(command);
    }

    /* Free memory */
    free(command);
    if (check_command)
        free(check_command);    
}

int exec_unmount(char *command){
    int retry = 4;          // Effectively TERM, sleep 1, TERM, sleep 1, KILL, sleep 1
    while(system(command) != 0){
        if(retry <= 0)
            return 1;
        int signal = (retry == 1) ? SIGKILL : SIGTERM;
        kill(atoi(pid), signal);    //TODO: find a way to get the pid of the process
        retry--;
        sleep(1);
        if(retry <= 0)
            return 1;
    }
    return 0;
}

/*
* Handy program to handle all our unmounting needs
* mountinfo is a C program to actually find our mounts on our supported OS's
* We rely on fuser being present, so if it's not then don't unmount anything. TODO: do we need to check for fuser?
* This isn't a real issue for the BSD's, but it is for Linux.
*/
int main(int argc, char **argv)
{
    RC_STRINGLIST *to_unmount;  // list of paths to unmount
    RC_STRING *path;            // path to unmount
    int size, i;                // size of the list and iterator
    pthread_t *threads;         // array of threads
    global_args_t global_args;  // arguments shared among all threads
    thread_args_t *args_array;  // array of arguments for each thread

    /* Check if fuser is available in PATH */
    if(system("command -v fuser"))
    {
        printf("fuser is not installed, can't unmount anything!\n");
        exit(1);
    }

    /* Get list of paths to unmount */
    size = populate_unmount_list(&to_unmount, argc, argv);

    /* Get list of shared paths in the system */
    populate_shared_list(&global_args.shared);

    /* Create array of threads and array of arguments for each path in the list */
    threads = xmalloc(size * sizeof(pthread_t));
    args_array = xmalloc(size * sizeof(thread_args_t));

    /* Initialize global arguments */
    global_args.command = argv[1];// Assuming argv[1] is the unmounting command
    pthread_mutex_init(&global_args.shared_lock, NULL);
    global_args.args_array = args_array;

    /* Unmount each path in a different thread */
    i = 0;
    TAILQ_FOREACH(path, to_unmount, entries)
    {
        args_array[i].index = i;
        args_array[i].path = path;
        pthread_mutex_init(&args_array[i].unmounting, NULL);
        pthread_mutex_lock(&args_array[i].unmounting);
        args_array[i].global_args = &global_args;
        pthread_create(threads + i, NULL, unmount_one, args_array + i);
        i++;
    }

    /* Wait for all threads to finish and long info */
    for (i = 0; i < size; i++)
    {
        if (strstr(argv[1], "-r") != NULL)
            ebegin("Remounting %s read-only", args_array[i].path->value);
        else
            ebegin("Unmounting %s", args_array[i].path->value);
        pthread_join(threads[i], NULL);
        pthread_mutex_unlock(&args_array[i].unmounting);
        if (strstr(argv[1], "-r") != NULL)
            eend(args_array[i].retval, "Failed to remount %s", args_array[i].path->value);
        else
            eend(args_array[i].retval, "Failed to unmount %s", args_array[i].path->value);
    }

    /* Destroy mutexes */
    pthread_mutex_destroy(&global_args.shared_lock);
    for (i = 0; i < size; i++)
        pthread_mutex_destroy(&args_array[i].unmounting);

    /* Free memory */
    free(threads);
    free(args_array);
    rc_stringlist_free(to_unmount);
    rc_stringlist_free(global_args.shared);

    return 0;
}
