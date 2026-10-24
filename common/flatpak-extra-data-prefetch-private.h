/*
 * Copyright 2025 FlatPark contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef __FLATPAK_EXTRA_DATA_PREFETCH_PRIVATE_H__
#define __FLATPAK_EXTRA_DATA_PREFETCH_PRIVATE_H__

#include "flatpak-common-types-private.h"
#include "flatpak-progress-private.h"

typedef struct _FlatpakExtraDataPrefetcher FlatpakExtraDataPrefetcher;

FlatpakExtraDataPrefetcher *flatpak_extra_data_prefetcher_new (FlatpakDir *dir);
FlatpakExtraDataPrefetcher *flatpak_extra_data_prefetcher_ref (FlatpakExtraDataPrefetcher *self);
void flatpak_extra_data_prefetcher_unref (FlatpakExtraDataPrefetcher *self);

gboolean flatpak_extra_data_prefetcher_is_enabled (FlatpakExtraDataPrefetcher *self);
void flatpak_extra_data_prefetcher_add (FlatpakExtraDataPrefetcher *self,
                                        gpointer                    operation,
                                        GVariant                   *sources);
void flatpak_extra_data_prefetcher_start (FlatpakExtraDataPrefetcher *self);
gboolean flatpak_extra_data_prefetcher_wait (FlatpakExtraDataPrefetcher *self,
                                             const char                 *sha256,
                                             FlatpakProgress            *progress,
                                             GCancellable               *cancellable);
void flatpak_extra_data_prefetcher_operation_done (FlatpakExtraDataPrefetcher *self,
                                                   gpointer                    operation);
void flatpak_extra_data_prefetch_cleanup_on_signal (void);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakExtraDataPrefetcher, flatpak_extra_data_prefetcher_unref)

#endif /* __FLATPAK_EXTRA_DATA_PREFETCH_PRIVATE_H__ */
