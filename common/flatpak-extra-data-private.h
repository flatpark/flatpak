/*
 * Copyright 2025 FlatPark contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef __FLATPAK_EXTRA_DATA_PRIVATE_H__
#define __FLATPAK_EXTRA_DATA_PRIVATE_H__

#include <ostree.h>

gboolean flatpak_repo_remove_extra_data (OstreeRepo   *repo,
                                         const char   *checksum,
                                         gboolean      dry_run,
                                         gboolean     *out_removed,
                                         guint64      *out_freed_bytes,
                                         GCancellable *cancellable,
                                         GError      **error);

#endif /* __FLATPAK_EXTRA_DATA_PRIVATE_H__ */
