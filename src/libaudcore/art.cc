/*
 * art.c
 * Copyright 2011-2012 John Lindgren
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions, and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions, and the following disclaimer in the documentation
 *    provided with the distribution.
 *
 * This software is provided "as is" and without any warranty, express or
 * implied. In no event shall the authors be liable for any damages arising from
 * the use of this software.
 */

#include "probe.h"
#include "internal.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "audstrings.h"
#include "hook.h"
#include "playlist-internal.h"
#include "scanner.h"
#include "vfs.h"

#define FLAG_DONE 1
#define FLAG_SENT 2

struct ArtItem {
    int refcount;
    int flag;

    /* album art as JPEG or PNG data */
    void * data;
    int64_t len;

    /* album art as (possibly a temporary) file */
    String art_file;
    bool_t is_temp;
};

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static GHashTable * art_items; /* of ArtItem */
static String current_ref;
static int send_source;

static void art_item_free (ArtItem * item)
{
    /* delete temporary file */
    if (item->art_file && item->is_temp)
    {
        String local = uri_to_filename (item->art_file);
        if (local)
            g_unlink (local);
    }

    g_free (item->data);
    delete item;
}

static bool_t send_requests (void * unused)
{
    pthread_mutex_lock (& mutex);

    GQueue queue = G_QUEUE_INIT;

    GHashTableIter iter;
    void * ptr1, * ptr2;

    g_hash_table_iter_init (& iter, art_items);
    while (g_hash_table_iter_next (& iter, & ptr1, & ptr2))
    {
        char * file = (char *) ptr1;
        ArtItem * item = (ArtItem *) ptr2;

        if (item->flag == FLAG_DONE)
        {
            g_queue_push_tail (& queue, str_ref (file));
            item->flag = FLAG_SENT;
        }
    }

    if (send_source)
    {
        g_source_remove (send_source);
        send_source = 0;
    }

    pthread_mutex_unlock (& mutex);

    String current;
    if (! current_ref)
        current = playback_entry_get_filename ();

    char * file;
    while ((file = (char *) g_queue_pop_head (& queue)))
    {
        hook_call ("art ready", file);

        if (current && ! strcmp (file, current))
        {
            hook_call ("current art ready", file);
            current_ref = String::from_c (file);
        }
        else
        {
            aud_art_unref (file); /* release temporary reference */
            str_unref (file);
        }
    }

    return FALSE;
}

static void request_callback (ScanRequest * request)
{
    pthread_mutex_lock (& mutex);

    String file = scan_request_get_filename (request);
    ArtItem * item = (ArtItem *) g_hash_table_lookup (art_items, file);
    assert (item != NULL && ! item->flag);

    scan_request_get_image_data (request, & item->data, & item->len);
    item->art_file = scan_request_get_image_file (request);
    item->flag = FLAG_DONE;

    if (! send_source)
        send_source = g_idle_add (send_requests, NULL);

    pthread_mutex_unlock (& mutex);
}

static ArtItem * art_item_get (const char * file)
{
    ArtItem * item = (ArtItem *) g_hash_table_lookup (art_items, file);

    if (item && item->flag)
    {
        item->refcount ++;
        return item;
    }

    if (! item)
    {
        item = new ArtItem ();
        g_hash_table_insert (art_items, str_get (file), item);
        item->refcount = 1; /* temporary reference */

        scan_request (file, SCAN_IMAGE, NULL, request_callback);
    }

    return NULL;
}

static void art_item_unref (const char * file, ArtItem * item)
{
    if (! -- item->refcount)
        g_hash_table_remove (art_items, file);
}

static void release_current (void)
{
    if (current_ref)
    {
        aud_art_unref (current_ref);
        current_ref = String ();
    }
}

void art_init (void)
{
    art_items = g_hash_table_new_full ((GHashFunc) str_calc_hash, g_str_equal,
     (GDestroyNotify) str_unref, (GDestroyNotify) art_item_free);

    hook_associate ("playlist position", (HookFunction) release_current, NULL);
    hook_associate ("playlist set playing", (HookFunction) release_current, NULL);
}

void art_cleanup (void)
{
    hook_dissociate ("playlist position", (HookFunction) release_current);
    hook_dissociate ("playlist set playing", (HookFunction) release_current);

    if (send_source)
    {
        g_source_remove (send_source);
        send_source = 0;
    }

    release_current ();

    g_hash_table_destroy (art_items);
    art_items = NULL;
}

EXPORT void aud_art_request_data (const char * file, const void * * data, int64_t * len)
{
    * data = NULL;
    * len = 0;

    pthread_mutex_lock (& mutex);

    ArtItem * item = art_item_get (file);
    if (! item)
        goto UNLOCK;

    /* load data from external image file */
    if (! item->data && item->art_file)
        vfs_file_get_contents (item->art_file, & item->data, & item->len);

    if (item->data)
    {
        * data = item->data;
        * len = item->len;
    }
    else
        art_item_unref (file, item);

UNLOCK:
    pthread_mutex_unlock (& mutex);
}

EXPORT const char * aud_art_request_file (const char * file)
{
    const char * art_file = NULL;
    pthread_mutex_lock (& mutex);

    ArtItem * item = art_item_get (file);
    if (! item)
        goto UNLOCK;

    /* save data to temporary file */
    if (item->data && ! item->art_file)
    {
        String local = write_temp_file (item->data, item->len);
        if (local)
        {
            item->art_file = filename_to_uri (local);
            item->is_temp = TRUE;
        }
    }

    if (item->art_file)
        art_file = item->art_file;
    else
        art_item_unref (file, item);

UNLOCK:
    pthread_mutex_unlock (& mutex);
    return art_file;
}

EXPORT void aud_art_unref (const char * file)
{
    pthread_mutex_lock (& mutex);

    ArtItem * item = (ArtItem *) g_hash_table_lookup (art_items, file);
    assert (item != NULL);

    art_item_unref (file, item);

    pthread_mutex_unlock (& mutex);
}
