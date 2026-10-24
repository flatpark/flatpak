/*
 * Copyright 2025 FlatPark contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "flatpak-extra-data-private.h"
#include "flatpak-utils-private.h"

static guint64
get_commitmeta_size (OstreeRepo *repo,
                     const char *checksum)
{
  g_autoptr(GFile) file = NULL;
  g_autoptr(GFileInfo) info = NULL;
  g_autofree char *relative_path = NULL;

  relative_path = g_strdup_printf ("objects/%c%c/%s.commitmeta",
                                   checksum[0], checksum[1], checksum + 2);
  file = g_file_resolve_relative_path (ostree_repo_get_path (repo), relative_path);
  info = g_file_query_info (file, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                            G_FILE_QUERY_INFO_NONE, NULL, NULL);
  return info != NULL ? g_file_info_get_size (info) : 0;
}

gboolean
flatpak_repo_remove_extra_data (OstreeRepo   *repo,
                                const char   *checksum,
                                gboolean      dry_run,
                                gboolean     *out_removed,
                                guint64      *out_freed_bytes,
                                GCancellable *cancellable,
                                GError      **error)
{
  g_autoptr(GVariant) metadata = NULL;
  g_autoptr(GVariant) extra_data = NULL;
  g_autoptr(GVariant) new_metadata = NULL;
  g_auto(GVariantDict) metadata_dict = FLATPAK_VARIANT_DICT_INITIALIZER;
  guint64 old_size;
  guint64 new_size;

  if (out_removed != NULL)
    *out_removed = FALSE;
  if (out_freed_bytes != NULL)
    *out_freed_bytes = 0;

  if (!ostree_validate_checksum_string (checksum, error))
    return FALSE;
  if (!ostree_repo_read_commit_detached_metadata (repo, checksum, &metadata,
                                                  cancellable, error))
    return FALSE;
  extra_data = metadata != NULL ?
               g_variant_lookup_value (metadata, "xa.extra-data", NULL) : NULL;
  if (extra_data == NULL)
    return TRUE;

  old_size = get_commitmeta_size (repo, checksum);
  g_variant_dict_init (&metadata_dict, metadata);
  g_variant_dict_remove (&metadata_dict, "xa.extra-data");
  new_metadata = g_variant_ref_sink (g_variant_dict_end (&metadata_dict));

  if (!dry_run &&
      !ostree_repo_write_commit_detached_metadata (repo, checksum, new_metadata,
                                                   cancellable, error))
    return FALSE;

  new_size = dry_run ? g_variant_get_size (new_metadata) :
             get_commitmeta_size (repo, checksum);
  if (out_removed != NULL)
    *out_removed = TRUE;
  if (out_freed_bytes != NULL && old_size > new_size)
    *out_freed_bytes = old_size - new_size;
  return TRUE;
}
