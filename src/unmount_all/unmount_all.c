#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "queue.h"
#include "rc.h"

int populateList(RC_STRINGLIST **list)
{
    FILE *fp;
    char path[4096]; // https://unix.stackexchange.com/questions/32795/what-is-the-maximum-allowed-filename-and-folder-size-with-ecryptfs
    int size;

    /* Open the command for reading */
    fp = popen("/lib/rc/bin/mountinfo", "r");
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

/*Execute the unmount of the provided path*/
void *unmountOne(void *path)
{
    char command[4096];
    sprintf(command, "umount %s", ((RC_STRING *)path)->value);
    system(command);
}

int main(int argc, char **argv)
{
    RC_STRINGLIST *list;
    RC_STRING *path;
    int size, i;

    printf("Starting Unmount!\n");

    size = populateList(&list);

    pthread_t thread_id[size];

    i = 0;
    TAILQ_FOREACH(path, list, entries)
    {
        printf("%s", path->value);
        pthread_create(thread_id + i, NULL, unmountOne, path);
        i++;
    }

    for (i = 0; i < size; i++)
        pthread_join(thread_id[i], NULL);

    rc_stringlist_free(list);

    printf("Unmounted %d filesystems!\n", size);

    return 0;
}
