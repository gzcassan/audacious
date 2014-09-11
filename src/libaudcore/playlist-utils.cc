/*
 * playlist-utils.c
 * Copyright 2009-2011 John Lindgren
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

#include "playlist-internal.h"

#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "audstrings.h"
#include "hook.h"
#include "multihash.h"
#include "runtime.h"
#include "tuple.h"
#include "vfs.h"

static const char * get_basename (const char * filename)
{
    const char * slash = strrchr (filename, '/');

    return (slash == nullptr) ? filename : slash + 1;
}

static int filename_compare_basename (const char * a, const char * b)
{
    return str_compare_encoded (get_basename (a), get_basename (b));
}

static int tuple_compare_string (const Tuple & a, const Tuple & b, int field)
{
    String string_a = a.get_str (field);
    String string_b = b.get_str (field);

    if (! string_a)
        return (! string_b) ? 0 : -1;

    return (! string_b) ? 1 : str_compare (string_a, string_b);
}

static int tuple_compare_int (const Tuple & a, const Tuple & b, int field)
{
    if (a.get_value_type (field) != TUPLE_INT)
        return (b.get_value_type (field) != TUPLE_INT) ? 0 : -1;
    if (b.get_value_type (field) != TUPLE_INT)
        return 1;

    int int_a = a.get_int (field);
    int int_b = b.get_int (field);

    return (int_a < int_b) ? -1 : (int_a > int_b);
}

static int tuple_compare_title (const Tuple & a, const Tuple & b)
{
    return tuple_compare_string (a, b, FIELD_TITLE);
}

static int tuple_compare_album (const Tuple & a, const Tuple & b)
{
    return tuple_compare_string (a, b, FIELD_ALBUM);
}

static int tuple_compare_artist (const Tuple & a, const Tuple & b)
{
    return tuple_compare_string (a, b, FIELD_ARTIST);
}

static int tuple_compare_date (const Tuple & a, const Tuple & b)
{
    return tuple_compare_int (a, b, FIELD_YEAR);
}

static int tuple_compare_track (const Tuple & a, const Tuple & b)
{
    return tuple_compare_int (a, b, FIELD_TRACK_NUMBER);
}

static int tuple_compare_length (const Tuple & a, const Tuple & b)
{
    return tuple_compare_int (a, b, FIELD_LENGTH);
}

static const PlaylistStringCompareFunc filename_comparisons[] = {
    str_compare_encoded,  // path
    filename_compare_basename,  // filename
    nullptr,  // title
    nullptr,  // album
    nullptr,  // artist
    nullptr,  // date
    nullptr,  // track
    nullptr,  // formatted title
    nullptr  // length
};

static const PlaylistTupleCompareFunc tuple_comparisons[] = {
    nullptr,  // path
    nullptr,  // filename
    tuple_compare_title,  // title
    tuple_compare_album,  // album
    tuple_compare_artist,  // artist
    tuple_compare_date,  // date
    tuple_compare_track,  // track
    nullptr,  // formatted title
    tuple_compare_length  // length
};

static const PlaylistStringCompareFunc title_comparisons[] = {
    nullptr,  // path
    nullptr,  // filename
    nullptr,  // title
    nullptr,  // album
    nullptr,  // artist
    nullptr,  // date
    nullptr,  // track
    str_compare,  // formatted title
    nullptr  // length
};

EXPORT void aud_playlist_sort_by_scheme (int playlist, int scheme)
{
    if (filename_comparisons[scheme] != nullptr)
        aud_playlist_sort_by_filename (playlist, filename_comparisons[scheme]);
    else if (tuple_comparisons[scheme] != nullptr)
        aud_playlist_sort_by_tuple (playlist, tuple_comparisons[scheme]);
    else if (title_comparisons[scheme] != nullptr)
        aud_playlist_sort_by_title (playlist, title_comparisons[scheme]);
}

EXPORT void aud_playlist_sort_selected_by_scheme (int playlist, int scheme)
{
    if (filename_comparisons[scheme] != nullptr)
        aud_playlist_sort_selected_by_filename (playlist, filename_comparisons[scheme]);
    else if (tuple_comparisons[scheme] != nullptr)
        aud_playlist_sort_selected_by_tuple (playlist, tuple_comparisons[scheme]);
    else if (title_comparisons[scheme] != nullptr)
        aud_playlist_sort_selected_by_title (playlist, title_comparisons[scheme]);
}

/* FIXME: this considers empty fields as duplicates */
EXPORT void aud_playlist_remove_duplicates_by_scheme (int playlist, int scheme)
{
    int entries = aud_playlist_entry_count (playlist);
    if (entries < 1)
        return;

    aud_playlist_select_all (playlist, false);

    if (filename_comparisons[scheme] != nullptr)
    {
        PlaylistStringCompareFunc compare = filename_comparisons[scheme];

        aud_playlist_sort_by_filename (playlist, compare);
        String last = aud_playlist_entry_get_filename (playlist, 0);

        for (int count = 1; count < entries; count ++)
        {
            String current = aud_playlist_entry_get_filename (playlist, count);

            if (compare (last, current) == 0)
                aud_playlist_entry_set_selected (playlist, count, true);

            last = current;
        }
    }
    else if (tuple_comparisons[scheme] != nullptr)
    {
        PlaylistTupleCompareFunc compare = tuple_comparisons[scheme];

        aud_playlist_sort_by_tuple (playlist, compare);
        Tuple last = aud_playlist_entry_get_tuple (playlist, 0, false);

        for (int count = 1; count < entries; count ++)
        {
            Tuple current = aud_playlist_entry_get_tuple (playlist, count, false);

            if (last && current && compare (last, current) == 0)
                aud_playlist_entry_set_selected (playlist, count, true);

            last = std::move (current);
        }
    }

    aud_playlist_delete_selected (playlist);
}

EXPORT void aud_playlist_remove_failed (int playlist)
{
    int entries = aud_playlist_entry_count (playlist);
    int count;

    aud_playlist_select_all (playlist, false);

    for (count = 0; count < entries; count ++)
    {
        String filename = aud_playlist_entry_get_filename (playlist, count);

        /* vfs_file_test() only works for file:// URIs currently */
        if (! strncmp (filename, "file://", 7) && ! vfs_file_test (filename, VFS_EXISTS))
            aud_playlist_entry_set_selected (playlist, count, true);
    }

    aud_playlist_delete_selected (playlist);
}

EXPORT void aud_playlist_select_by_patterns (int playlist, const Tuple & patterns)
{
    int entries = aud_playlist_entry_count (playlist);

    aud_playlist_select_all (playlist, true);

    for (int field : {FIELD_TITLE, FIELD_ALBUM, FIELD_ARTIST, FIELD_FILE_NAME})
    {
        String pattern = patterns.get_str (field);
        GRegex * regex;

        if (! pattern || ! pattern[0] || ! (regex = g_regex_new (pattern,
         G_REGEX_CASELESS, (GRegexMatchFlags) 0, nullptr)))
            continue;

        for (int entry = 0; entry < entries; entry ++)
        {
            if (! aud_playlist_entry_get_selected (playlist, entry))
                continue;

            Tuple tuple = aud_playlist_entry_get_tuple (playlist, entry, false);
            String string = tuple.get_str (field);

            if (! string || ! g_regex_match (regex, string, (GRegexMatchFlags) 0, nullptr))
                aud_playlist_entry_set_selected (playlist, entry, false);
        }

        g_regex_unref (regex);
    }
}

static StringBuf make_playlist_path (int playlist)
{
    if (! playlist)
        return filename_build ({aud_get_path (AudPath::UserDir), "playlist.xspf"});

    StringBuf name = str_printf ("playlist_%02d.xspf", 1 + playlist);
    name.steal (filename_build ({aud_get_path (AudPath::UserDir), name}));
    return name;
}

static void load_playlists_real (void)
{
    const char * folder = aud_get_path (AudPath::PlaylistDir);

    /* old (v3.1 and earlier) naming scheme */

    int count;
    for (count = 0; ; count ++)
    {
        StringBuf path = make_playlist_path (count);
        if (! g_file_test (path, G_FILE_TEST_EXISTS))
            break;

        aud_playlist_insert (count);
        playlist_insert_playlist_raw (count, 0, filename_to_uri (path));
        playlist_set_modified (count, true);
    }

    /* unique ID-based naming scheme */

    StringBuf order_path = filename_build ({folder, "order"});
    char * order_string;
    Index<String> order;

    g_file_get_contents (order_path, & order_string, nullptr, nullptr);
    if (! order_string)
        goto DONE;

    order = str_list_to_index (order_string, " ");
    g_free (order_string);

    for (int i = 0; i < order.len (); i ++)
    {
        const String & number = order[i];

        StringBuf name1 = str_concat ({number, ".audpl"});
        StringBuf name2 = str_concat ({number, ".xspf"});

        StringBuf path = filename_build ({folder, name1});
        if (! g_file_test (path, G_FILE_TEST_EXISTS))
            path.steal (filename_build ({folder, name2}));

        playlist_insert_with_id (count + i, atoi (number));
        playlist_insert_playlist_raw (count + i, 0, filename_to_uri (path));
        playlist_set_modified (count + i, false);

        if (g_str_has_suffix (path, ".xspf"))
            playlist_set_modified (count + i, true);
    }

DONE:
    if (! aud_playlist_count ())
        aud_playlist_insert (0);

    aud_playlist_set_active (0);
}

static void save_playlists_real (void)
{
    int lists = aud_playlist_count ();
    const char * folder = aud_get_path (AudPath::PlaylistDir);

    /* save playlists */

    Index<String> order;
    SimpleHash<String, bool> saved;

    for (int i = 0; i < lists; i ++)
    {
        int id = aud_playlist_get_unique_id (i);
        StringBuf number = int_to_str (id);
        StringBuf name = str_concat ({number, ".audpl"});

        if (playlist_get_modified (i))
        {
            StringBuf path = filename_build ({folder, name});
            aud_playlist_save (i, filename_to_uri (path));
            playlist_set_modified (i, false);
        }

        order.append (String (number));
        saved.add (String (name), true);
    }

    StringBuf order_string = index_to_str_list (order, " ");
    StringBuf order_path = filename_build ({folder, "order"});

    char * old_order_string;
    g_file_get_contents (order_path, & old_order_string, nullptr, nullptr);

    if (! old_order_string || strcmp (old_order_string, order_string))
    {
        GError * error = nullptr;
        if (! g_file_set_contents (order_path, order_string, -1, & error))
        {
            AUDERR ("Cannot write to %s: %s\n", (const char *) order_path, error->message);
            g_error_free (error);
        }
    }

    g_free (old_order_string);

    /* clean up deleted playlists and files from old naming scheme */

    g_unlink (make_playlist_path (0));

    GDir * dir = g_dir_open (folder, 0, nullptr);
    if (! dir)
        return;

    const char * name;
    while ((name = g_dir_read_name (dir)))
    {
        if (! g_str_has_suffix (name, ".audpl") && ! g_str_has_suffix (name, ".xspf"))
            continue;

        if (! saved.lookup (String (name)))
            g_unlink (filename_build ({folder, name}));
    }

    g_dir_close (dir);
}

static bool hooks_added, state_changed;

static void update_cb (void * data, void * user)
{
    if (GPOINTER_TO_INT (data) < PLAYLIST_UPDATE_METADATA)
        return;

    state_changed = true;
}

static void state_cb (void * data, void * user)
{
    state_changed = true;
}

void load_playlists (void)
{
    load_playlists_real ();
    playlist_load_state ();

    state_changed = false;

    if (! hooks_added)
    {
        hook_associate ("playlist update", update_cb, nullptr);
        hook_associate ("playlist activate", state_cb, nullptr);
        hook_associate ("playlist position", state_cb, nullptr);

        hooks_added = true;
    }
}

void save_playlists (bool exiting)
{
    save_playlists_real ();

    /* on exit, save resume states */
    if (state_changed || exiting)
    {
        playlist_save_state ();
        state_changed = false;
    }

    if (exiting && hooks_added)
    {
        hook_dissociate ("playlist update", update_cb);
        hook_dissociate ("playlist activate", state_cb);
        hook_dissociate ("playlist position", state_cb);

        hooks_added = false;
    }
}
