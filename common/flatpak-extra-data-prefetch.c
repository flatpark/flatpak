/*
 * Copyright 2025 FlatPark contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <glib/gstdio.h>

#include "flatpak-dir-private.h"
#include "flatpak-extra-data-prefetch-private.h"
#include "flatpak-repo-utils-private.h"
#include "flatpak-utils-http-private.h"
#include "flatpak-utils-private.h"

#define DEFAULT_PARALLEL_DOWNLOADS 4
#define MAX_DOWNLOADS_PER_HOST 4
#define MAX_DOWNLOAD_ATTEMPTS 3
#define MAX_SIGNAL_JOBS 256
#define PREFETCH_MARKER ".flatpak-prefetch"

typedef struct
{
  FlatpakExtraDataPrefetcher *prefetcher;
  char                       *name;
  char                       *uri;
  char                       *sha256;
  char                       *host;
  /* Precomputed so the signal handler can clean up without allocating */
  char                       *path;
  char                       *part_path;
  char                       *dir;
  char                       *marker;
  char                       *root;
  guint64                     size;
  guint64                     downloaded;
  guint                       users;
  gboolean                    done;
  gboolean                    created;
  gboolean                    marked;
  GCond                       cond;
} PrefetchJob;

/* Jobs of live prefetchers, for flatpak_extra_data_prefetch_cleanup_on_signal() */
static PrefetchJob *signal_jobs[MAX_SIGNAL_JOBS];

struct _FlatpakExtraDataPrefetcher
{
  gint          ref_count;
  guint         parallel;
  GMutex        mutex;
  GCond         host_cond;
  GHashTable   *jobs;
  GPtrArray    *queue;
  GHashTable   *operations;
  GHashTable   *active_hosts;
  GThreadPool  *pool;
  GCancellable *cancellable;
  gboolean      started;
};

static void
prefetch_job_free (PrefetchJob *job)
{
  g_cond_clear (&job->cond);
  g_free (job->name);
  g_free (job->uri);
  g_free (job->sha256);
  g_free (job->host);
  g_free (job->path);
  g_free (job->part_path);
  g_free (job->dir);
  g_free (job->marker);
  g_free (job->root);
  g_free (job);
}

static void
set_job_paths (PrefetchJob *job)
{
  g_autoptr(GFile) base = flatpak_get_user_base_dir_location ();
  g_autoptr(GFile) root = g_file_get_child (base, "extra-data");
  g_autoptr(GFile) dir = g_file_get_child (root, job->sha256);
  g_autofree char *part_name = g_strconcat (job->name, ".part", NULL);

  job->root = g_file_get_path (root);
  job->dir = g_file_get_path (dir);
  job->path = g_build_filename (job->dir, job->name, NULL);
  job->part_path = g_build_filename (job->dir, part_name, NULL);
  job->marker = g_build_filename (job->dir, PREFETCH_MARKER, NULL);
}

static void
register_signal_job (PrefetchJob *job)
{
  for (guint i = 0; i < MAX_SIGNAL_JOBS; i++)
    if (g_atomic_pointer_compare_and_exchange (&signal_jobs[i], NULL, job))
      return;
}

static void
unregister_signal_job (PrefetchJob *job)
{
  for (guint i = 0; i < MAX_SIGNAL_JOBS; i++)
    g_atomic_pointer_compare_and_exchange (&signal_jobs[i], job, NULL);
}

/* Called from a SIGINT/SIGTERM handler right before _exit(): drop whatever
 * the running prefetchers downloaded. Only async-signal-safe calls. */
void
flatpak_extra_data_prefetch_cleanup_on_signal (void)
{
  for (guint i = 0; i < MAX_SIGNAL_JOBS; i++)
    {
      PrefetchJob *job = g_atomic_pointer_get (&signal_jobs[i]);

      if (job == NULL)
        continue;
      unlink (job->part_path);
      if (job->created)
        unlink (job->path);
      if (job->marked)
        {
          unlink (job->marker);
          if (rmdir (job->dir) != 0)
            {
              /* Something else is in there, keep it marked as ours */
              int fd = open (job->marker, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
              if (fd >= 0)
                close (fd);
            }
        }
      rmdir (job->root);
    }
}

static void
remove_job_directory_if_empty (const char *dir)
{
  g_autoptr(GDir) d = NULL;
  g_autofree char *marker = g_build_filename (dir, PREFETCH_MARKER, NULL);
  const char *name;
  gboolean have_marker = FALSE;

  d = g_dir_open (dir, 0, NULL);
  if (d == NULL)
    return;

  while ((name = g_dir_read_name (d)) != NULL)
    {
      if (strcmp (name, PREFETCH_MARKER) == 0)
        have_marker = TRUE;
      else
        return;
    }

  if (have_marker && g_unlink (marker) != 0 && errno != ENOENT)
    {
      g_debug ("Unable to remove extra-data prefetch marker %s: %s", marker, g_strerror (errno));
      return;
    }
  if (g_rmdir (dir) != 0 && errno != ENOENT && errno != ENOTEMPTY)
    g_debug ("Unable to remove extra-data prefetch directory %s: %s", dir, g_strerror (errno));
}

static void
remove_job_file (PrefetchJob *job,
                 gboolean     partial)
{
  const char *path = partial ? job->part_path : job->path;

  if (g_unlink (path) != 0 && errno != ENOENT)
    g_debug ("Unable to remove extra-data prefetch file %s: %s", path, g_strerror (errno));
  remove_job_directory_if_empty (job->dir);
  g_rmdir (job->root);
}

static gboolean
ensure_job_directory (const char  *dir,
                      gboolean    *out_marked,
                      GError     **error)
{
  g_autofree char *parent = g_path_get_dirname (dir);
  g_autofree char *marker = g_build_filename (dir, PREFETCH_MARKER, NULL);

  if (g_mkdir_with_parents (parent, 0755) != 0)
    return glnx_throw_errno_prefix (error, "Unable to create extra-data directory %s", parent);

  if (g_mkdir (dir, 0755) == 0)
    {
      if (!g_file_set_contents (marker, "", 0, error))
        {
          g_rmdir (dir);
          return FALSE;
        }
      *out_marked = TRUE;
    }
  else if (errno == EEXIST)
    *out_marked = g_file_test (marker, G_FILE_TEST_IS_REGULAR);
  else
    return glnx_throw_errno_prefix (error, "Unable to create extra-data prefetch directory %s", dir);

  return TRUE;
}

static void
download_progress (guint64  downloaded,
                   gpointer user_data)
{
  PrefetchJob *job = user_data;

  g_mutex_lock (&job->prefetcher->mutex);
  job->downloaded = downloaded;
  g_mutex_unlock (&job->prefetcher->mutex);
}

static gboolean
validate_download (const char  *path,
                   PrefetchJob *job,
                   GError     **error)
{
  g_autoptr(GFile) file = g_file_new_for_path (path);
  g_autoptr(GFileInputStream) in = NULL;
  g_autoptr(GFileInfo) info = NULL;
  g_autoptr(GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);
  guchar buffer[64 * 1024];
  gssize n_read;

  info = g_file_query_info (file, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                            G_FILE_QUERY_INFO_NONE, NULL, error);
  if (info == NULL)
    return FALSE;
  if ((guint64) g_file_info_get_size (info) != job->size)
    return flatpak_fail (error, "Wrong size for prefetched extra data %s", job->uri);

  in = g_file_read (file, NULL, error);
  if (in == NULL)
    return FALSE;
  while ((n_read = g_input_stream_read (G_INPUT_STREAM (in), buffer, sizeof buffer,
                                        job->prefetcher->cancellable, error)) > 0)
    g_checksum_update (checksum, buffer, n_read);
  if (n_read < 0)
    return FALSE;
  if (strcmp (g_checksum_get_string (checksum), job->sha256) != 0)
    return flatpak_fail (error, "Invalid checksum for prefetched extra data %s", job->uri);

  return TRUE;
}

static gboolean
acquire_host_slot (PrefetchJob *job)
{
  FlatpakExtraDataPrefetcher *self = job->prefetcher;
  guint active;

  g_mutex_lock (&self->mutex);
  while (!g_cancellable_is_cancelled (self->cancellable) &&
         GPOINTER_TO_UINT (g_hash_table_lookup (self->active_hosts, job->host)) >= MAX_DOWNLOADS_PER_HOST)
    g_cond_wait (&self->host_cond, &self->mutex);

  if (g_cancellable_is_cancelled (self->cancellable))
    {
      g_mutex_unlock (&self->mutex);
      return FALSE;
    }

  active = GPOINTER_TO_UINT (g_hash_table_lookup (self->active_hosts, job->host));
  g_hash_table_replace (self->active_hosts, g_strdup (job->host), GUINT_TO_POINTER (active + 1));
  g_mutex_unlock (&self->mutex);
  return TRUE;
}

static void
release_host_slot (PrefetchJob *job)
{
  FlatpakExtraDataPrefetcher *self = job->prefetcher;
  guint active;

  g_mutex_lock (&self->mutex);
  active = GPOINTER_TO_UINT (g_hash_table_lookup (self->active_hosts, job->host));
  if (active <= 1)
    g_hash_table_remove (self->active_hosts, job->host);
  else
    g_hash_table_replace (self->active_hosts, g_strdup (job->host), GUINT_TO_POINTER (active - 1));
  g_cond_broadcast (&self->host_cond);
  g_mutex_unlock (&self->mutex);
}

static void
prefetch_worker (gpointer data,
                 gpointer user_data)
{
  PrefetchJob *job = data;
  FlatpakExtraDataPrefetcher *self = user_data;
  const char *path = job->path;
  const char *part_path = job->part_path;
  gboolean success = FALSE;
  gboolean marked = FALSE;
  gboolean have_host_slot = FALSE;
  g_autoptr(GError) setup_error = NULL;

  if (!ensure_job_directory (job->dir, &marked, &setup_error))
    g_debug ("Unable to prepare extra-data prefetch for %s: %s", job->uri, setup_error->message);
  else
    {
      job->marked = marked;
      if (marked)
        {
          g_autoptr(GError) validation_error = NULL;

          if (g_unlink (part_path) != 0 && errno != ENOENT)
            g_debug ("Unable to remove partial extra-data prefetch file %s: %s",
                     part_path, g_strerror (errno));
          if (g_file_test (path, G_FILE_TEST_EXISTS))
            {
              if (validate_download (path, job, &validation_error))
                {
                  job->created = TRUE;
                  success = TRUE;
                }
              else
                {
                  g_debug ("Discarding invalid extra-data prefetch file %s: %s",
                           path, validation_error->message);
                  if (g_unlink (path) != 0 && errno != ENOENT)
                    g_debug ("Unable to remove invalid extra-data prefetch file %s: %s",
                             path, g_strerror (errno));
                }
            }
        }
      else if (g_file_test (path, G_FILE_TEST_EXISTS))
        success = TRUE;

      if (!success)
        have_host_slot = acquire_host_slot (job);
    }

  for (guint attempt = 0; have_host_slot && !success && attempt < MAX_DOWNLOAD_ATTEMPTS; attempt++)
    {
      g_autoptr(FlatpakHttpSession) session = flatpak_create_http_session (PACKAGE_STRING);
      g_autoptr(GFile) part_file = g_file_new_for_path (part_path);
      g_autoptr(GFileOutputStream) out = NULL;
      g_autoptr(GError) local_error = NULL;

      if (g_cancellable_is_cancelled (self->cancellable))
        break;

      out = g_file_replace (part_file, NULL, FALSE, G_FILE_CREATE_PRIVATE,
                            self->cancellable, &local_error);
      if (out != NULL &&
          flatpak_download_http_uri_with_retries (session, job->uri, NULL,
                                                  FLATPAK_HTTP_FLAGS_NONE,
                                                  G_OUTPUT_STREAM (out), NULL,
                                                  download_progress, job, 0,
                                                  self->cancellable, &local_error) &&
          g_output_stream_close (G_OUTPUT_STREAM (out), self->cancellable, &local_error) &&
          validate_download (part_path, job, &local_error))
        {
          g_autoptr(GFile) target = g_file_new_for_path (path);

          if (g_file_move (part_file, target, G_FILE_COPY_NONE, self->cancellable,
                           NULL, NULL, &local_error))
            {
              job->created = TRUE;
              success = TRUE;
            }
          else if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_EXISTS))
            {
              g_clear_error (&local_error);
              g_unlink (part_path);
              success = TRUE;
            }
        }

      if (!success)
        {
          g_debug ("Extra-data prefetch attempt %u failed for %s: %s",
                   attempt + 1, job->uri, local_error ? local_error->message : "unknown error");
          g_unlink (part_path);
        }
    }

  if (have_host_slot)
    release_host_slot (job);

  g_mutex_lock (&self->mutex);
  job->done = TRUE;
  g_cond_broadcast (&job->cond);
  g_mutex_unlock (&self->mutex);
}

static guint
get_parallel_downloads (FlatpakDir *dir)
{
  const char *env = g_getenv ("FLATPAK_EXTRA_DATA_PARALLEL");
  g_autofree char *configured = NULL;
  const char *value = env;
  char *end = NULL;
  guint64 parsed;

  if (value == NULL || *value == '\0')
    {
      configured = flatpak_dir_get_config (dir, "extra-data-parallel", NULL);
      value = configured;
    }
  if (value == NULL)
    return DEFAULT_PARALLEL_DOWNLOADS;

  errno = 0;
  parsed = g_ascii_strtoull (value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || parsed > G_MAXINT)
    {
      g_info ("Ignoring invalid extra-data parallelism value '%s'", value);
      return DEFAULT_PARALLEL_DOWNLOADS;
    }

  return parsed;
}

FlatpakExtraDataPrefetcher *
flatpak_extra_data_prefetcher_new (FlatpakDir *dir)
{
  FlatpakExtraDataPrefetcher *self = g_new0 (FlatpakExtraDataPrefetcher, 1);

  self->ref_count = 1;
  self->parallel = get_parallel_downloads (dir);
  g_mutex_init (&self->mutex);
  g_cond_init (&self->host_cond);
  self->jobs = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify) prefetch_job_free);
  self->queue = g_ptr_array_new ();
  self->operations = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) g_ptr_array_unref);
  self->active_hosts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  self->cancellable = g_cancellable_new ();
  return self;
}

FlatpakExtraDataPrefetcher *
flatpak_extra_data_prefetcher_ref (FlatpakExtraDataPrefetcher *self)
{
  g_atomic_int_inc (&self->ref_count);
  return self;
}

void
flatpak_extra_data_prefetcher_unref (FlatpakExtraDataPrefetcher *self)
{
  GHashTableIter iter;
  gpointer value;

  if (!g_atomic_int_dec_and_test (&self->ref_count))
    return;

  g_cancellable_cancel (self->cancellable);
  g_mutex_lock (&self->mutex);
  g_cond_broadcast (&self->host_cond);
  g_mutex_unlock (&self->mutex);
  if (self->pool != NULL)
    g_thread_pool_free (self->pool, FALSE, TRUE);

  g_hash_table_iter_init (&iter, self->jobs);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      PrefetchJob *job = value;
      unregister_signal_job (job);
      remove_job_file (job, TRUE);
      if (job->created)
        remove_job_file (job, FALSE);
    }

  g_hash_table_unref (self->operations);
  g_ptr_array_unref (self->queue);
  g_hash_table_unref (self->jobs);
  g_hash_table_unref (self->active_hosts);
  g_object_unref (self->cancellable);
  g_cond_clear (&self->host_cond);
  g_mutex_clear (&self->mutex);
  g_free (self);
}

gboolean
flatpak_extra_data_prefetcher_is_enabled (FlatpakExtraDataPrefetcher *self)
{
  return self->parallel > 1;
}

void
flatpak_extra_data_prefetcher_add (FlatpakExtraDataPrefetcher *self,
                                   gpointer                    operation,
                                   GVariant                   *sources)
{
  GPtrArray *operation_jobs;

  if (!flatpak_extra_data_prefetcher_is_enabled (self))
    return;

  operation_jobs = g_hash_table_lookup (self->operations, operation);
  if (operation_jobs == NULL)
    {
      operation_jobs = g_ptr_array_new ();
      g_hash_table_insert (self->operations, operation, operation_jobs);
    }

  for (gsize i = 0; i < g_variant_n_children (sources); i++)
    {
      const char *name = NULL;
      const char *uri = NULL;
      const guchar *sha_bytes = NULL;
      guint64 size;
      g_autofree char *sha256 = NULL;
      g_autoptr(GUri) parsed_uri = NULL;
      PrefetchJob *job;

      flatpak_repo_parse_extra_data_sources (sources, i, &name, &size, NULL, &sha_bytes, &uri);
      if (sha_bytes == NULL || name == NULL || *name == '\0' ||
          strcmp (name, ".") == 0 || strcmp (name, "..") == 0 || strchr (name, '/') != NULL ||
          (!g_str_has_prefix (uri, "http:") && !g_str_has_prefix (uri, "https:")))
        continue;

      sha256 = ostree_checksum_from_bytes (sha_bytes);
      job = g_hash_table_lookup (self->jobs, sha256);
      if (job == NULL)
        {
          parsed_uri = g_uri_parse (uri, G_URI_FLAGS_PARSE_RELAXED, NULL);
          if (parsed_uri == NULL || g_uri_get_host (parsed_uri) == NULL)
            continue;

          job = g_new0 (PrefetchJob, 1);
          job->prefetcher = self;
          job->name = g_strdup (name);
          job->uri = g_strdup (uri);
          job->sha256 = g_steal_pointer (&sha256);
          job->host = g_ascii_strdown (g_uri_get_host (parsed_uri), -1);
          job->size = size;
          set_job_paths (job);
          g_cond_init (&job->cond);
          register_signal_job (job);
          g_hash_table_insert (self->jobs, job->sha256, job);
          g_ptr_array_add (self->queue, job);
        }
      job->users++;
      g_ptr_array_add (operation_jobs, job);
    }
}

void
flatpak_extra_data_prefetcher_start (FlatpakExtraDataPrefetcher *self)
{
  g_autoptr(GError) error = NULL;

  if (!flatpak_extra_data_prefetcher_is_enabled (self) || self->started)
    return;
  self->started = TRUE;
  self->pool = g_thread_pool_new (prefetch_worker, self, self->parallel, FALSE, &error);
  if (self->pool == NULL)
    {
      g_info ("Unable to create extra-data prefetch pool: %s", error->message);
      return;
    }

  for (guint i = 0; i < self->queue->len; i++)
    g_thread_pool_push (self->pool, g_ptr_array_index (self->queue, i), NULL);
}

gboolean
flatpak_extra_data_prefetcher_wait (FlatpakExtraDataPrefetcher *self,
                                    const char                 *sha256,
                                    FlatpakProgress            *progress,
                                    GCancellable               *cancellable)
{
  PrefetchJob *job = g_hash_table_lookup (self->jobs, sha256);

  if (job == NULL || self->pool == NULL)
    return FALSE;

  g_mutex_lock (&self->mutex);
  while (!job->done &&
         (cancellable == NULL || !g_cancellable_is_cancelled (cancellable)))
    {
      gint64 deadline = g_get_monotonic_time () + 100 * G_TIME_SPAN_MILLISECOND;
      guint64 downloaded;

      g_cond_wait_until (&job->cond, &self->mutex, deadline);
      downloaded = job->downloaded;
      g_mutex_unlock (&self->mutex);
      flatpak_progress_update_extra_data (progress, downloaded);
      g_mutex_lock (&self->mutex);
    }
  g_mutex_unlock (&self->mutex);
  return TRUE;
}

void
flatpak_extra_data_prefetcher_operation_done (FlatpakExtraDataPrefetcher *self,
                                              gpointer                    operation)
{
  GPtrArray *jobs = g_hash_table_lookup (self->operations, operation);

  if (jobs == NULL)
    return;
  for (guint i = 0; i < jobs->len; i++)
    {
      PrefetchJob *job = g_ptr_array_index (jobs, i);

      g_assert (job->users > 0);
      job->users--;
      if (job->users == 0 && job->created)
        {
          remove_job_file (job, FALSE);
          job->created = FALSE;
        }
    }
  g_hash_table_remove (self->operations, operation);
}
