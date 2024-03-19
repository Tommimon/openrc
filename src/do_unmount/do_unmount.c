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

/* Enumerate all the possible outcomes of the unmount procedure */
enum Outcome {
    SUCCESS,  // unmounting was successful
    THIS,     // the mount point is being used by this process
    UNKNOWN,  // the mount point is being used but processes can't be found
    FUSER,    // fuser command failed
    KILL,     // the mount point is being used by other processes but the kill command failed
    ERROR,    // unknown error
};

/* Provide a different failure message for each possible outcome */
const char *failure_messages[] = {
    "",
    "failed because we are using %s",
    "in use but fuser finds nothing",
    "in use but fuser command failed",
    "in use but failed to kill process",
    "failed",
};

/* Contains all parameters passed to unmount_one() function */
typedef struct t_args_t
{
    int index;                    // index of the path in the list
    RC_STRING *path;              // path to unmount
    pthread_mutex_t unmounting;   // mutex locked until the path is unmounted
    global_args_t *global_args;   // parameters shared among all threads
    enum Outcome retval;          // return value of the unmounting command
} thread_args_t;

/* Pass arguments to a command and open standard output as readable file */
FILE *popen_with_args(const char *command, int argc, char **argv) {
    FILE *fp;   // file pointer to program output
    char *cmd;  // command with all arguments
    int length; // length of the command with all arguments
    int i;      // iterator

    /* Calculate the length of the command */
    length = strlen(command) + 1; /* +1 for the null terminator */
    for (i = 0; i < argc; i++)
        length += strlen(argv[i]) + 3; /* +3 for the quotes and space */
    
    /* Allocate memory for the command */
    cmd = xmalloc(length * sizeof(char));
    sprintf(cmd, "%s", command);

    /* Craft the command with all passed arguments */
    for (i = 0; i < argc; i++) {
        strcat(cmd, " \"");
        strcat(cmd, argv[i]);
        strcat(cmd, "\"");
    }

    /* Open the command, free memory and return output file pointer for reading */
    fp = popen(cmd, "r");;
    free(cmd);
    return fp;
}

/* Populate the list of shared mounts in the system */
void populate_shared_list(RC_STRINGLIST **list) {
    FILE *fp;               // file pointer to the mountinfo file
    size_t len = 0;         // length of the line read
    char *line = NULL;      // line read from the mountinfo file
    char *token = NULL;     // token of the current line, no need to free this since is pointing to the original string
    char *path = NULL;      // path to relative to the current line
    int i;                  // fields iterator

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
                xasprintf(path, "%s", token);
            }
            /* If token contains "shared:", note that if there is no optional field this token will be the separator character '-' */
            else if (i == 6 && strstr(token, "shared:") != NULL)
            {
                rc_stringlist_add(*list, path);
            }
            token = strtok(NULL, " ");
            i++;
        }
    }

    /* Close file and free memory */
    fclose(fp);
    if(line)
        free(line);
    if(path)
        free(path);
    #endif
}

/* Pass arguments to mountinfo and store output in a list of paths to unmount */
int populate_unmount_list(RC_STRINGLIST **list, int argc, char **argv)
{
    int size = 0,       // number of paths to unmount
    FILE *fp;           // file pointer to the output of the command
    char *path = NULL;  // path to add to the list
    size_t len = 0;     // length of the line read


    /* Open the command for reading */
    fp = popen_with_args("mountinfo", argc-2, argv+2);
    if (fp == NULL)
    {
        printf("Failed to run mountinfo command, can't unmount anything!\n");
        exit(1);
    }

    *list = rc_stringlist_new();
    /* Read the output a line at a time */
    while (getline(&path, &len, fp) != -1)
    {
        path[strlen(path) - 1] = '\0'; /* Remove trailing '\n' */
        rc_stringlist_add(*list, path);
        size++;
    }

    pclose(fp);
    if(path)
        free(path);
    return size;
}

/* If unmount command fails terminate the programs using it and retry */
int unmount_with_retries(char *command, char *mount_point){
    int retry = 4;                                  // effectively TERM, sleep 1, TERM, sleep 1, KILL, sleep 1
    size_t len = 0;                                 // length of the line read
    char *pids = NULL;                              // line read from the fuser command
    FILE *fp = NULL;                                // file pointer to the output of the command
    char *fuser_command;                            // command to execute fuser and find the pids of the processes that use the mount point
    char *kill_command;                             // command to kill the processes that use the mount point
    char pidString[8];                              // string to store the pid (max value is 4194304 source https://unix.stackexchange.com/questions/16883/what-is-the-maximum-value-of-the-process-id)
    enum Outcome retVal = SUCCESS;                  // return value of the function
    char *f_opts = "-m -c";                         // fuser options
    char *f_kill = "-s ";                           // fuser kill options
    char* timeout = getenv("rc_fuser_timeout");     // fuser timeout

    #ifdef __linux__
    f_opts = "-m";
    f_kill = "-";
    #endif

    /* Default timeout value if environment variable is not set */
    if(timeout == NULL)
        timeout = "60";

    /* Allocate memory for the commands */
    xasprintf(fuser_command, "timeout -s KILL %s fuser %s %s 2>/dev/null", timeout, f_opts, mount_point);
    xasprintf(kill_command, "fuser %sTERM -k %s \"%s\" >/dev/null 2>&1", f_kill, f_opts, mount_point);

    /* Execute the unmount command, send term/kill signal and retry if it fails */
    while(system(command) != 0){
        fp = popen(fuser_command, "r");
        if(fp == NULL) {
            retVal = FUSER;
            break;
        }
        if(getline(&pids, &len, fp) == -1){
            retVal = UNKNOWN;
            break;
        }
        fclose(fp);
        fp = NULL;
        pid_t pid = getpid();
        sprintf(pidString, "%d", pid);
        if(strstr(pids, pidString) != NULL) {   /* Check if the current process is using the mount point */
            retVal = THIS;
            break;
        }
        retry--;
        if(retry <= 0){     /* After 4 retries, return error */
            retVal = ERROR;
            break;
        }
        if(retry == 1)                                                                                          /* Kill processes if it's the last retry */
            sprintf(kill_command, "fuser %sKILL -k %s \"%s\" >/dev/null 2>&1", f_kill, f_opts, mount_point);    /* No need to reallocate since length is the same */
        if(system(kill_command) != 0 && retry == 1) {   /* If the kill command fails, return error */
            retVal = KILL;
            break;
        }
        sleep(1);
    }

    /* Free memory and close file */
    if(fp)
        fclose(fp);
    if (pids)
        free(pids);
    free(fuser_command);
    free(kill_command);
    
    return retVal;
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
    xasprintf(command, "%s %s 2>/dev/null", args->global_args->command, args->path->value);

    /* If is a shared mount, avoid any other shared mount concurrency */
    if(rc_stringlist_find(args->global_args->shared, args->path->value)){
        /* Allocate memory and compose the check command */
        xasprintf(check_command, "mountinfo --quiet %s", args->path->value);
        pthread_mutex_lock(&args->global_args->shared_lock);
        /* Unmount only if is still mounted */
        if(system(check_command) == 0)
            args->retval = unmount_with_retries(command, args->path->value);
        else
            args->retval = SUCCESS;
        pthread_mutex_unlock(&args->global_args->shared_lock);
    }
    else
    {
        /* Unmount the path */
        args->retval = unmount_with_retries(command, args->path->value);
    }

    /* Free memory */
    free(command);
    if (check_command)
        free(check_command);
}


/*
* Handy program to handle all our unmounting needs
* mountinfo is a C program to actually find our mounts on our supported OS's
* We rely on fuser being present, so if it's not then don't unmount anything.
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
    int exitCode = 0;           // return value of the main function

    /* Check first argument provided */
    if(argc < 2)
    {
        printf("No unmounting command provided!\n");
        exit(1);
    }

    /* Check if fuser is available in PATH */
    if(system("command -v fuser >/dev/null 2>&1"))
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
    global_args.command = argv[1];  /* Assuming argv[1] is the unmounting command */
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

    /* Wait for all threads to finish and long info with the proper failure message */
    for (i = 0; i < size; i++)
    {
        if (strstr(argv[1], "-r") != NULL)
            ebegin("Remounting %s read-only", args_array[i].path->value);
        else
            ebegin("Unmounting %s", args_array[i].path->value);
        pthread_join(threads[i], NULL);
        pthread_mutex_unlock(&args_array[i].unmounting);
        if (strstr(argv[1], "-r") != NULL)
            eend(args_array[i].retval, failure_messages[args_array[i].retval], args_array[i].path->value);
        else
            eend(args_array[i].retval, failure_messages[args_array[i].retval], args_array[i].path->value);
        if (args_array[i].retval == ERROR)
            exitCode = 1;     /* Set exit code to 1 because this should never happen */
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

    return exitCode;
}
