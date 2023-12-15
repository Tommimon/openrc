#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "queue.h"
#include "rc.h"

int populate_list(RC_STRINGLIST **list, int argc, char **argv)
{
    FILE *fp;
    char path[4096]; // https://unix.stackexchange.com/questions/32795/what-is-the-maximum-allowed-filename-and-folder-size-with-ecryptfs
    int size;
    char cmd[4096] = "mountinfo";

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
        rc_stringlist_add(*list, path);
        size++;
    }

    /* close */
    pclose(fp);

    return size;
}

typedef struct {
    int index;
    RC_STRING *path;
    pthread_t *threads;
} thread_args_t;


/*Execute the unmount of the provided path*/
void *unmount_one(void *input)
{
    thread_args_t *args = (thread_args_t *)input;
    RC_STRING *prev;
    int i;

    printf("Unmounting %s\n", args->path->value);
    prev = args->path;
    for(i = args->index-1; i >= 0; i--) {
        prev = TAILQ_PREV(prev, rc_stringlist, entries);
        printf("Previous: %s at index  %d\n", prev->value, i);
        // get index of first different char 
        int j = 0;
        while (args->path->value[j] == prev->value[j]) {
            j++;
        }
        // if the first different char is a '/' then we have a child
        if (prev->value[j] == '/') {
            printf("Child: %s\n", prev->value);
            pthread_join(args->threads[i], NULL);
        }
    }
    char command[4096];
    sprintf(command, "umount %s", args->path->value);
    system(command);
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
        args[i].threads = threads_list;
        pthread_create(threads_list + i, NULL, unmount_one, args + i);
        i++;
    }

    for (i = 0; i < size; i++)
        pthread_join(threads_list[i], NULL);

    rc_stringlist_free(list);

    printf("Unmounted %d filesystems!\n", size);

    return 0;
}
