/* vi:set et sw=2 sts=2 cin cino=t0,f0,(0,{s,>2s,n-s,^-s,e-s:
 * Copyright 2025 FlatPark contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <errno.h>
#include <sys/file.h>
#include <unistd.h>

#include <glib/gi18n.h>

#include "flatpak-builtins.h"
#include "flatpak-builtins-utils.h"
#include "flatpak-dir-utils-private.h"
#include "flatpak-extra-data-private.h"
#include "flatpak-utils-private.h"

#define PREFETCH_MARKER ".flatpak-prefetch"

static gboolean opt_dry_run;
static gboolean opt_delete_data;

static GOptionEntry options[] = {
  { "dry-run", 0, 0, G_OPTION_ARG_NONE, &opt_dry_run, N_("Don't make any changes"), NULL },
  { "delete-data", 0, 0, G_OPTION_ARG_NONE, &opt_delete_data, N_("Also delete the data of apps that are no longer installed"), NULL },
  { NULL }
};

static gpointer
cancel_lock_wait (gpointer data)
{
  GCancellable *cancellable = data;

  g_usleep (100 * G_TIME_SPAN_MILLISECOND);
  g_cancellable_cancel (cancellable);
  return NULL;
}

static void
add_ref_to_commit_map (gpointer key,
                       gpointer value,
                       gpointer user_data)
{
  const char *ref = key;
  const char *checksum = value;
  GHashTable *commit_refs = user_data;
  const char *old_ref = g_hash_table_lookup (commit_refs, checksum);

  if (old_ref == NULL ||
      ((!g_str_has_prefix (old_ref, "app/") && !g_str_has_prefix (old_ref, "runtime/")) &&
       (g_str_has_prefix (ref, "app/") || g_str_has_prefix (ref, "runtime/"))))
    g_hash_table_replace (commit_refs, g_strdup (checksum), g_strdup (ref));
}

static gboolean
clean_commit_metadata (OstreeRepo   *repo,
                       gboolean      dry_run,
                       GCancellable *cancellable,
                       guint        *objects,
                       guint64      *freed_bytes,
                       GError      **error)
{
  g_autoptr(GHashTable) repo_objects = NULL;
  g_autoptr(GHashTable) refs = NULL;
  g_autoptr(GHashTable) commit_refs = NULL;
  g_autoptr(GPtrArray) commits = NULL;
  GHashTableIter iter;
  gpointer key;

  if (!ostree_repo_list_objects (repo,
                                 OSTREE_REPO_LIST_OBJECTS_ALL |
                                 OSTREE_REPO_LIST_OBJECTS_NO_PARENTS,
                                 &repo_objects, cancellable, error) ||
      !ostree_repo_list_refs (repo, NULL, &refs, cancellable, error))
    return FALSE;

  commit_refs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  g_hash_table_foreach (refs, add_ref_to_commit_map, commit_refs);
  commits = g_ptr_array_new_with_free_func (g_free);

  g_hash_table_iter_init (&iter, repo_objects);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      const char *checksum;
      OstreeObjectType object_type;

      ostree_object_name_deserialize (key, &checksum, &object_type);
      if (object_type == OSTREE_OBJECT_TYPE_COMMIT)
        g_ptr_array_add (commits, g_strdup (checksum));
    }
  g_ptr_array_sort (commits, flatpak_strcmp0_ptr);

  for (guint i = 0; i < commits->len; i++)
    {
      const char *checksum = g_ptr_array_index (commits, i);
      const char *ref = g_hash_table_lookup (commit_refs, checksum);
      g_autofree char *short_checksum = NULL;
      gboolean removed;
      guint64 freed;

      if (!flatpak_repo_remove_extra_data (repo, checksum, dry_run,
                                           &removed, &freed, cancellable, error))
        return FALSE;
      if (!removed)
        continue;

      if (ref == NULL)
        {
          short_checksum = g_strndup (checksum, 12);
          ref = short_checksum;
        }
      g_print ("%.1f MB  %s\n", freed / (1024.0 * 1024.0), ref);
      (*objects)++;
      *freed_bytes += freed;
    }

  return TRUE;
}

static gboolean
clean_extra_data_dir (GFile        *dir,
                      gboolean      dry_run,
                      GCancellable *cancellable,
                      guint        *objects,
                      guint64      *freed_bytes,
                      GError      **error)
{
  g_autoptr(GFileEnumerator) enumerator = NULL;
  g_autoptr(GFile) marker = g_file_get_child (dir, PREFETCH_MARKER);
  g_autoptr(GFileInfo) marker_info = NULL;
  g_autoptr(GError) local_error = NULL;

  marker_info = g_file_query_info (marker, G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                   G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                   cancellable, &local_error);
  if (marker_info != NULL && g_file_info_get_file_type (marker_info) == G_FILE_TYPE_REGULAR)
    {
      g_autofree char *path = g_file_get_path (dir);
      guint64 size;

      if (!g_file_measure_disk_usage (dir, G_FILE_MEASURE_NONE, cancellable, NULL, NULL,
                                      &size, NULL, NULL, error))
        return FALSE;

      g_print ("%.1f MB  %s\n", size / (1024.0 * 1024.0), path);
      if (!dry_run && !flatpak_rm_rf (dir, cancellable, error))
        return FALSE;
      (*objects)++;
      *freed_bytes += size;
      return TRUE;
    }
  if (marker_info == NULL && !g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }
  g_clear_error (&local_error);

  enumerator = g_file_enumerate_children (dir,
                                          G_FILE_ATTRIBUTE_STANDARD_NAME ","
                                          G_FILE_ATTRIBUTE_STANDARD_TYPE ","
                                          G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          cancellable, &local_error);
  if (enumerator == NULL)
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        return TRUE;
      g_propagate_error (error, g_steal_pointer (&local_error));
      return FALSE;
    }

  while (TRUE)
    {
      GFileInfo *info;
      GFile *child;
      const char *name;
      GFileType type;

      if (!g_file_enumerator_iterate (enumerator, &info, &child,
                                      cancellable, error))
        return FALSE;
      if (info == NULL)
        break;

      name = g_file_info_get_name (info);
      type = g_file_info_get_file_type (info);
      if (type == G_FILE_TYPE_DIRECTORY)
        {
          if (!clean_extra_data_dir (child, dry_run, cancellable, objects, freed_bytes, error))
            return FALSE;
          if (!dry_run && !g_file_delete (child, cancellable, &local_error) &&
              !g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_EMPTY) &&
              !g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            {
              g_propagate_error (error, g_steal_pointer (&local_error));
              return FALSE;
            }
          g_clear_error (&local_error);
        }
      else if (type == G_FILE_TYPE_REGULAR && g_str_has_suffix (name, ".part"))
        {
          g_autofree char *path = g_file_get_path (child);
          guint64 size = g_file_info_get_size (info);

          g_print ("%.1f MB  %s\n", size / (1024.0 * 1024.0), path);
          if (!dry_run && !g_file_delete (child, cancellable, error))
            return FALSE;
          (*objects)++;
          *freed_bytes += size;
        }
      else
        {
          g_autofree char *path = g_file_get_path (child);
          g_print (_("Keeping: %s\n"), path);
        }
    }

  return TRUE;
}

static void
report_item (guint64     size,
             const char *what,
             guint      *objects,
             guint64    *freed_bytes)
{
  g_print ("%.1f MB  %s\n", size / (1024.0 * 1024.0), what);
  (*objects)++;
  *freed_bytes += size;
}

static guint64
disk_usage (GFile        *file,
            GCancellable *cancellable)
{
  guint64 size = 0;

  g_file_measure_disk_usage (file, G_FILE_MEASURE_NONE, cancellable, NULL, NULL,
                             &size, NULL, NULL, NULL);
  return size;
}

static gboolean
is_deploy_tmpdir (const char *name)
{
  /* flatpak_dir_deploy() checks out into ".<checksum>-XXXXXX" and renames
   * it into place when done */
  if (name[0] != '.' || strlen (name) < 1 + 64 + 7)
    return FALSE;
  for (int i = 1; i <= 64; i++)
    if (!g_ascii_isxdigit (name[i]) || g_ascii_isupper (name[i]))
      return FALSE;
  return name[65] == '-';
}

/* Removes the directories below @dir that hold nothing but other empty
 * directories, and @dir itself if @remove_self. ostree and flatpak delete
 * refs and deployments but leave their parent directories behind.
 *
 * Only @max_depth levels below @dir are descended into (-1 for no limit);
 * a directory at the limit counts as empty only if it has no entries at
 * all, except deploy checkouts that @dry_run is about to remove. This
 * must never reach into a deployment: its empty directories are mount
 * points the sandbox needs.
 *
 * Returns whether @dir is (or, with @dry_run, would be) empty afterwards. */
static gboolean
clean_empty_dirs (GFile        *dir,
                  gboolean      remove_self,
                  int           max_depth,
                  gboolean      dry_run,
                  GCancellable *cancellable,
                  guint        *removed)
{
  g_autoptr(GFileEnumerator) enumerator = NULL;
  gboolean empty = TRUE;

  enumerator = g_file_enumerate_children (dir,
                                          G_FILE_ATTRIBUTE_STANDARD_NAME ","
                                          G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          cancellable, NULL);
  if (enumerator == NULL)
    return FALSE;

  while (TRUE)
    {
      GFileInfo *info;
      GFile *child;

      if (!g_file_enumerator_iterate (enumerator, &info, &child, cancellable, NULL))
        {
          empty = FALSE;
          break;
        }
      if (info == NULL)
        break;

      if (g_file_info_get_file_type (info) != G_FILE_TYPE_DIRECTORY)
        empty = FALSE;
      else if (max_depth == 0)
        {
          if (!(dry_run && is_deploy_tmpdir (g_file_info_get_name (info))))
            empty = FALSE;
        }
      else if (!clean_empty_dirs (child, TRUE, max_depth < 0 ? -1 : max_depth - 1,
                                  dry_run, cancellable, removed))
        empty = FALSE;
    }

  if (empty && remove_self)
    {
      if (!dry_run && !g_file_delete (dir, cancellable, NULL))
        return FALSE;
      (*removed)++;
    }

  return empty;
}

static gboolean
clean_interrupted_deploys (FlatpakDir   *dir,
                           gboolean      dry_run,
                           GCancellable *cancellable,
                           guint        *objects,
                           guint64      *freed_bytes,
                           GError      **error)
{
  const char *kinds[] = { "app", "runtime" };

  for (guint k = 0; k < G_N_ELEMENTS (kinds); k++)
    {
      g_autoptr(GFile) kind_dir = g_file_get_child (flatpak_dir_get_path (dir), kinds[k]);
      g_autoptr(GFileEnumerator) ids = NULL;
      guint empty_dirs = 0;

      ids = g_file_enumerate_children (kind_dir, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                       G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                       cancellable, NULL);
      if (ids == NULL)
        continue;

      while (TRUE)
        {
          GFileInfo *id_info;
          GFile *id_dir;
          g_autoptr(GFileEnumerator) arches = NULL;

          if (!g_file_enumerator_iterate (ids, &id_info, &id_dir, cancellable, error))
            return FALSE;
          if (id_info == NULL)
            break;

          arches = g_file_enumerate_children (id_dir, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                              G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                              cancellable, NULL);
          while (arches != NULL)
            {
              GFileInfo *arch_info;
              GFile *arch_dir;
              g_autoptr(GFileEnumerator) branches = NULL;

              if (!g_file_enumerator_iterate (arches, &arch_info, &arch_dir, cancellable, error))
                return FALSE;
              if (arch_info == NULL)
                break;

              branches = g_file_enumerate_children (arch_dir, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                                    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                    cancellable, NULL);
              while (branches != NULL)
                {
                  GFileInfo *branch_info;
                  GFile *branch_dir;
                  g_autoptr(GFileEnumerator) entries = NULL;

                  if (!g_file_enumerator_iterate (branches, &branch_info, &branch_dir, cancellable, error))
                    return FALSE;
                  if (branch_info == NULL)
                    break;

                  entries = g_file_enumerate_children (branch_dir,
                                                       G_FILE_ATTRIBUTE_STANDARD_NAME ","
                                                       G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                                       G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                       cancellable, NULL);
                  while (entries != NULL)
                    {
                      GFileInfo *info;
                      GFile *child;
                      g_autofree char *path = NULL;

                      if (!g_file_enumerator_iterate (entries, &info, &child, cancellable, error))
                        return FALSE;
                      if (info == NULL)
                        break;
                      if (g_file_info_get_file_type (info) != G_FILE_TYPE_DIRECTORY ||
                          !is_deploy_tmpdir (g_file_info_get_name (info)))
                        continue;

                      path = g_file_get_path (child);
                      report_item (disk_usage (child, cancellable), path, objects, freed_bytes);
                      if (!dry_run && !flatpak_rm_rf (child, cancellable, error))
                        return FALSE;
                    }
                }
            }

          /* An install interrupted before its first deployment leaves
           * app/<id>/<arch>/<branch> with nothing in it. Only those three
           * levels: never descend into the deployments themselves. */
          if (clean_empty_dirs (id_dir, TRUE, 2, dry_run, cancellable, &empty_dirs))
            {
              g_autofree char *path = g_file_get_path (id_dir);
              report_item (0, path, objects, freed_bytes);
            }
        }
    }

  if (!dry_run && !flatpak_dir_cleanup_removed (dir, cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean
clean_repo_leftovers (FlatpakDir   *dir,
                      OstreeRepo   *repo,
                      gboolean      dry_run,
                      GCancellable *cancellable,
                      guint        *objects,
                      guint64      *freed_bytes,
                      GError      **error)
{
  GFile *repo_path = ostree_repo_get_path (repo);
  const char *ref_dirs[] = { "heads", "remotes", "mirrors" };
  g_autoptr(GFile) state_dir = g_file_get_child (repo_path, "state");
  g_autoptr(GFileEnumerator) states = NULL;
  g_auto(GStrv) remotes = NULL;
  guint empty_ref_dirs = 0;

  /* Refs of uninstalled apps and of deleted remotes */
  for (guint i = 0; i < G_N_ELEMENTS (ref_dirs); i++)
    {
      g_autoptr(GFile) refs = flatpak_build_file (repo_path, "refs", ref_dirs[i], NULL);
      clean_empty_dirs (refs, FALSE, -1, dry_run, cancellable, &empty_ref_dirs);
    }
  if (empty_ref_dirs > 0)
    {
      g_autofree char *refs_path = g_build_filename (flatpak_file_get_path_cached (repo_path), "refs", NULL);
      g_autofree char *what = g_strdup_printf (ngettext ("%s: %u empty directory",
                                                         "%s: %u empty directories",
                                                         empty_ref_dirs),
                                               refs_path, empty_ref_dirs);
      report_item (0, what, objects, freed_bytes);
    }

  /* Partial-commit markers of pulls whose commit is gone */
  states = g_file_enumerate_children (state_dir, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                      cancellable, NULL);
  while (states != NULL)
    {
      GFileInfo *info;
      GFile *child;
      const char *name;
      g_autofree char *checksum = NULL;
      gboolean have_commit = FALSE;

      if (!g_file_enumerator_iterate (states, &info, &child, cancellable, error))
        return FALSE;
      if (info == NULL)
        break;

      name = g_file_info_get_name (info);
      if (!g_str_has_suffix (name, ".commitpartial"))
        continue;
      checksum = g_strndup (name, strlen (name) - strlen (".commitpartial"));
      if (!ostree_validate_checksum_string (checksum, NULL) ||
          !ostree_repo_has_object (repo, OSTREE_OBJECT_TYPE_COMMIT, checksum,
                                   &have_commit, cancellable, error))
        continue;
      if (have_commit)
        continue;

      {
        g_autofree char *path = g_file_get_path (child);
        report_item (0, path, objects, freed_bytes);
      }
      if (!dry_run && !g_file_delete (child, cancellable, error))
        return FALSE;
    }

  /* Remotes created for bundles and .flatpakref files whose apps are gone.
   * flatpak removes these on uninstall, but not in every path. */
  remotes = flatpak_dir_list_remotes (dir, cancellable, NULL);
  for (guint i = 0; remotes != NULL && remotes[i] != NULL; i++)
    {
      const char *remote = remotes[i];
      g_autofree char *what = NULL;

      if (!g_str_has_suffix (remote, "-origin") ||
          !flatpak_dir_get_remote_noenumerate (dir, remote) ||
          flatpak_dir_remote_has_deploys (dir, remote))
        continue;

      what = g_strdup_printf (_("remote %s (no longer used)"), remote);
      report_item (0, what, objects, freed_bytes);
      if (!dry_run && !flatpak_dir_remove_remote (dir, FALSE, remote, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

static gboolean
add_uninstalled_app (GHashTable   *apps,
                     const char   *app_id)
{
  g_autoptr(FlatpakDecomposed) ref = NULL;

  if (!g_dbus_is_name (app_id) || g_hash_table_contains (apps, app_id))
    return FALSE;
  ref = flatpak_find_current_ref (app_id, NULL, NULL);
  if (ref != NULL)
    return FALSE;
  g_hash_table_add (apps, g_strdup (app_id));
  return TRUE;
}

static void
collect_names (GFile      *dir,
               GHashTable *apps)
{
  g_autoptr(GFileEnumerator) enumerator = NULL;

  enumerator = g_file_enumerate_children (dir, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          NULL, NULL);
  while (enumerator != NULL)
    {
      GFileInfo *info;

      if (!g_file_enumerator_iterate (enumerator, &info, NULL, NULL, NULL) || info == NULL)
        break;
      add_uninstalled_app (apps, g_file_info_get_name (info));
    }
}

/* ~/.var/app/<id>, overrides and portal permissions of apps that are no
 * longer installed anywhere. This is the user's data: it is only listed
 * unless --delete-data is given. */
static gboolean
clean_app_data (gboolean      dry_run,
                GCancellable *cancellable,
                guint        *objects,
                guint64      *freed_bytes,
                GError      **error)
{
  g_autofree char *var_app_path = g_build_filename (g_get_home_dir (), ".var", "app", NULL);
  g_autoptr(GFile) var_app = g_file_new_for_path (var_app_path);
  g_autoptr(GFile) overrides = flatpak_build_file (flatpak_get_user_base_dir_location (),
                                                   "overrides", NULL);
  g_autoptr(GHashTable) apps = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr(GPtrArray) sorted = NULL;
  guint64 total = 0;

  collect_names (var_app, apps);
  collect_names (overrides, apps);
  if (g_hash_table_size (apps) == 0)
    return TRUE;

  sorted = g_hash_table_get_keys_as_ptr_array (apps);
  g_ptr_array_sort (sorted, flatpak_strcmp0_ptr);

  g_print (_("Data of apps that are no longer installed:\n"));
  for (guint i = 0; i < sorted->len; i++)
    {
      const char *app_id = g_ptr_array_index (sorted, i);
      g_autoptr(GFile) data_dir = g_file_get_child (var_app, app_id);
      g_autoptr(GFile) override = g_file_get_child (overrides, app_id);
      gboolean have_data = g_file_query_exists (data_dir, cancellable);
      gboolean have_override = g_file_query_exists (override, cancellable);
      guint64 size = have_data ? disk_usage (data_dir, cancellable) : 0;
      g_autofree char *what = g_strdup_printf ("%s%s%s", app_id,
                                               have_data ? "  ~/.var/app" : "",
                                               have_override ? _("  overrides") : "");

      g_print ("%.1f MB  %s\n", size / (1024.0 * 1024.0), what);
      total += size;
      if (!opt_delete_data)
        continue;

      (*objects)++;
      *freed_bytes += size;
      if (dry_run)
        continue;

      if (have_data && !flatpak_rm_rf (data_dir, cancellable, error))
        return FALSE;
      if (have_override && !flatpak_remove_override_keyfile (app_id, TRUE, error))
        return FALSE;
      {
        g_autoptr(GError) local_error = NULL;

        if (!reset_permissions_for_app (app_id, &local_error))
          g_printerr (_("Unable to reset permissions of %s: %s\n"), app_id, local_error->message);
      }
    }

  if (!opt_delete_data)
    {
      g_autofree char *formatted = g_format_size (total);
      g_print (_("Kept %s; run \"flatpak clean --delete-data\" to delete it\n"), formatted);
    }

  return TRUE;
}

static gboolean
has_explicit_installation_selection (int    argc,
                               char **argv)
{
  for (int i = 1; i < argc; i++)
    {
      if (strcmp (argv[i], "--user") == 0 ||
          strcmp (argv[i], "-u") == 0 ||
          strcmp (argv[i], "--system") == 0 ||
          strcmp (argv[i], "--installation") == 0 ||
          g_str_has_prefix (argv[i], "--installation="))
        return TRUE;
    }

  return FALSE;
}

static char *
get_installation_title (FlatpakDir *dir)
{
  if (flatpak_dir_is_user (dir))
    return g_strdup (_("User installation"));
  if (g_strcmp0 (flatpak_dir_get_id (dir), SYSTEM_DIR_DEFAULT_ID) == 0)
    return g_strdup (_("System installation (default)"));
  return g_strdup_printf (_("Installation %s"), flatpak_dir_get_name_cached (dir));
}

static gboolean
clean_repo (FlatpakDir    *dir,
            gboolean       explicit_selection,
            GCancellable  *cancellable,
            gboolean      *out_read_only,
            guint         *out_objects,
            guint64       *out_freed_bytes,
            GError       **error)
{
  g_autoptr(GCancellable) lock_cancellable = NULL;
  g_autoptr(GError) lock_error = NULL;
  g_autoptr(OstreeRepoAutoLock) repo_lock = NULL;
  g_auto(GLnxLockFile) deploy_lock = { 0, };
  g_autofree char *repo_path = NULL;
  OstreeRepo *repo = flatpak_dir_get_repo (dir);
  gboolean writable;
  gboolean dry_run;
  GThread *lock_timeout_thread;

  *out_read_only = FALSE;
  *out_objects = 0;
  *out_freed_bytes = 0;

  if (repo == NULL)
    return TRUE;

  flatpak_dir_set_no_system_helper (dir, TRUE);
  repo_path = g_file_get_path (ostree_repo_get_path (repo));
  writable = access (repo_path, W_OK) == 0;
  if (!writable && explicit_selection)
    return flatpak_fail (error,
                         _("The repository at %s is not writable; run this command as root"),
                         repo_path);

  dry_run = opt_dry_run || !writable;
  *out_read_only = !writable;

  if (writable)
    {
      lock_cancellable = g_cancellable_new ();
      lock_timeout_thread = g_thread_new ("clean-lock-timeout", cancel_lock_wait,
                                          lock_cancellable);
      repo_lock = ostree_repo_auto_lock_push (repo, OSTREE_REPO_LOCK_EXCLUSIVE,
                                              lock_cancellable, &lock_error);
      g_thread_join (lock_timeout_thread);
      if (repo_lock == NULL)
        {
          if (g_cancellable_set_error_if_cancelled (cancellable, error))
            return FALSE;
          if (g_error_matches (lock_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            return flatpak_fail (error, _("The repository is busy with another transaction"));
          g_propagate_error (error, g_steal_pointer (&lock_error));
          g_prefix_error (error, _("Unable to acquire exclusive repository lock: "));
          return FALSE;
        }

      /* Pulls and deploys hold this shared; prune holds it exclusively */
      if (!flatpak_dir_repo_lock (dir, &deploy_lock, LOCK_EX | LOCK_NB, cancellable, &lock_error))
        {
          if (g_error_matches (lock_error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
            return flatpak_fail (error, _("The repository is busy with another transaction"));
          g_propagate_error (error, g_steal_pointer (&lock_error));
          return FALSE;
        }
    }

  return clean_commit_metadata (repo, dry_run, cancellable,
                                out_objects, out_freed_bytes, error) &&
         clean_repo_leftovers (dir, repo, dry_run, cancellable,
                               out_objects, out_freed_bytes, error) &&
         clean_interrupted_deploys (dir, dry_run, cancellable,
                                    out_objects, out_freed_bytes, error);
}

gboolean
flatpak_builtin_clean (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  g_autoptr(GFile) extra_data_dir = NULL;
  gboolean explicit_selection = has_explicit_installation_selection (argc, argv);
  gboolean show_titles;
  guint total_objects = 0;
  guint64 total_freed_bytes = 0;

  context = g_option_context_new (_("- Remove cached extra-data and leftovers from installations"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_ALL_DIRS |
                                     FLATPAK_BUILTIN_FLAG_OPTIONAL_REPO,
                                     &dirs, cancellable, error))
    return FALSE;
  if (argc != 1)
    return usage_error (context, _("Too many arguments"), error);

  show_titles = dirs->len > 1;
  for (guint i = 0; i < dirs->len; i++)
    {
      FlatpakDir *dir = g_ptr_array_index (dirs, i);
      gboolean is_user = flatpak_dir_is_user (dir);
      gboolean have_repo = flatpak_dir_get_repo (dir) != NULL;
      gboolean have_extra_data = FALSE;
      gboolean read_only = FALSE;
      guint objects = 0;
      guint64 freed_bytes = 0;

      if (is_user)
        {
          extra_data_dir = flatpak_build_file (flatpak_get_user_base_dir_location (),
                                               "extra-data", NULL);
          have_extra_data = g_file_query_exists (extra_data_dir, cancellable);
        }
      if (!have_repo && !is_user)
        continue;

      if (show_titles && (have_repo || have_extra_data))
        {
          g_autofree char *title = get_installation_title (dir);
          g_print ("%s:\n", title);
        }

      if (have_repo &&
          !clean_repo (dir, explicit_selection, cancellable,
                       &read_only, &objects, &freed_bytes, error))
        return FALSE;

      if (read_only)
        {
          g_autofree char *title = get_installation_title (dir);
          g_autofree char *formatted = g_format_size (freed_bytes);

          if (is_user)
            g_print (_("%s: would free %s; the repository is not writable\n"),
                     title, formatted);
          else if (g_strcmp0 (flatpak_dir_get_id (dir), SYSTEM_DIR_DEFAULT_ID) == 0)
            g_print (_("%s: would free %s; run \"sudo flatpak clean --system\"\n"),
                     title, formatted);
          else
            g_print (_("%s: would free %s; run \"sudo flatpak clean --installation=%s\"\n"),
                     title, formatted, flatpak_dir_get_id (dir));
        }

      if (opt_dry_run || !read_only)
        {
          total_objects += objects;
          total_freed_bytes += freed_bytes;
        }

      if (is_user)
        {
          objects = 0;
          freed_bytes = 0;
          if (!clean_extra_data_dir (extra_data_dir, opt_dry_run, cancellable,
                                     &objects, &freed_bytes, error))
            return FALSE;
          if (!opt_dry_run)
            {
              g_autoptr(GError) local_error = NULL;

              if (!g_file_delete (extra_data_dir, cancellable, &local_error) &&
                  !g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_EMPTY) &&
                  !g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
                {
                  g_propagate_error (error, g_steal_pointer (&local_error));
                  return FALSE;
                }
            }
          total_objects += objects;
          total_freed_bytes += freed_bytes;
        }
    }

  for (guint i = 0; i < dirs->len; i++)
    if (flatpak_dir_is_user (g_ptr_array_index (dirs, i)))
      {
        guint objects = 0;
        guint64 freed_bytes = 0;

        if (!clean_app_data (opt_dry_run, cancellable, &objects, &freed_bytes, error))
          return FALSE;
        total_objects += objects;
        total_freed_bytes += freed_bytes;
        break;
      }

  if (opt_dry_run)
    g_print (_("Would free: %u objects, %.2f GB\n"),
             total_objects, total_freed_bytes / (1024.0 * 1024.0 * 1024.0));
  else
    g_print (_("Freed: %u objects, %.2f GB\n"),
             total_objects, total_freed_bytes / (1024.0 * 1024.0 * 1024.0));

  return TRUE;
}

gboolean
flatpak_complete_clean (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;

  context = g_option_context_new ("");
  if (!flatpak_option_context_parse (context, options,
                                     &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_ALL_DIRS |
                                     FLATPAK_BUILTIN_FLAG_OPTIONAL_REPO,
                                     NULL, NULL, NULL))
    return FALSE;

  flatpak_complete_options (completion, global_entries);
  flatpak_complete_options (completion, options);
  flatpak_complete_options (completion, user_entries);
  return TRUE;
}
