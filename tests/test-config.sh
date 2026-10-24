#!/bin/bash
#
# Copyright (C) 2019 Matthias Clasen
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.

set -euo pipefail

. $(dirname $0)/libtest.sh

# This test looks for specific localized strings.
export LC_ALL=C

${FLATPAK} config --list > list_out
assert_file_has_content list_out "^languages:"
assert_file_has_content list_out "^extra-data-parallel: [*]unset[*] [(]default: 4[)]"
assert_file_has_content list_out "^keep-extra-data: [*]unset[*] [(]default: false[)]"

ok "config list"

${FLATPAK} config --set languages "de;fr"
${FLATPAK} config --get languages > get_out
assert_file_has_content get_out "^de;fr"

ok "config set"

${FLATPAK} config --set languages "*"
${FLATPAK} config --get languages > get_out
assert_file_has_content get_out "*"

ok "config languages *"

${FLATPAK} config --set languages "all"
${FLATPAK} config --get languages > get_out
assert_file_has_content get_out "all"

ok "config languages *"

${FLATPAK} config --unset languages
${FLATPAK} config --get languages > get_out
assert_file_has_content get_out "^[*]unset[*]"

ok "config unset"

${FLATPAK} config --set extra-data-parallel 0
${FLATPAK} config --get extra-data-parallel > get_out
assert_file_has_content get_out "^0$"
${FLATPAK} config --set extra-data-parallel 1
if ${FLATPAK} config --set extra-data-parallel invalid >invalid_out 2>&1; then
    fatal "Invalid extra-data parallelism unexpectedly accepted"
fi
assert_file_has_content invalid_out "not a valid non-negative integer"
${FLATPAK} config --unset extra-data-parallel

ok "config extra-data parallelism"

done_testing
