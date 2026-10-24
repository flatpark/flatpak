/*
 * Copyright © 2018 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#ifndef __FLATPAK_TRANSACTION_PRIVATE_H__
#define __FLATPAK_TRANSACTION_PRIVATE_H__

#include "flatpak-transaction.h"
#include "flatpak-ref-utils-private.h"

FlatpakRemoteState *flatpak_transaction_ensure_remote_state (FlatpakTransaction             *self,
                                                             FlatpakTransactionOperationType kind,
                                                             const char                     *remote,
                                                             const char                     *opt_arch,
                                                             GError                        **error);

FlatpakDecomposed * flatpak_transaction_operation_get_decomposed (FlatpakTransactionOperation *self);

typedef enum
{
  FLATPAK_PREFETCH_STATE_NONE,
  FLATPAK_PREFETCH_STATE_QUEUED,
  FLATPAK_PREFETCH_STATE_DOWNLOADING,
  FLATPAK_PREFETCH_STATE_DONE,
  FLATPAK_PREFETCH_STATE_FAILED,
} FlatpakPrefetchState;

FlatpakPrefetchState flatpak_transaction_get_operation_prefetch_state (FlatpakTransaction          *self,
                                                                       FlatpakTransactionOperation *op,
                                                                       guint64                     *out_downloaded,
                                                                       guint64                     *out_total);

#include "flatpak-dir-private.h"

#endif /* __FLATPAK_TRANSACTION_PRIVATE_H__ */
