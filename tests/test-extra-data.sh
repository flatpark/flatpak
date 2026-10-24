#!/bin/bash
#
# Copyright (C) 2025 Red Hat, Inc
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

. "$(dirname $0)/libtest.sh"

skip_without_bwrap

REPONAME="test"
BRANCH="master"
COLLECTION_ID="org.test.Collection.${REPONAME}"

setup_repo ${REPONAME} ${COLLECTION_ID}

# create the extra data
EXTRA_DATA_FILE="extra-data-test"
EXTRA_DATA_DIR="${TEST_DATA_DIR}/extra-data-server/"
mkdir -p "${EXTRA_DATA_DIR}"
echo "extra-data-test-content" > "${EXTRA_DATA_DIR}/${EXTRA_DATA_FILE}"

# serve the extra data
EVENTS_LOG="${TEST_DATA_DIR}/extra-data-events"
touch "${EVENTS_LOG}"
httpd web-server.py "${EXTRA_DATA_DIR}" "" "${EVENTS_LOG}"
EXTRA_DATA_URL="http://127.0.0.1:$(cat httpd-port)/${EXTRA_DATA_FILE}"

# download to get the size and sha256 sum
DOWNLOADED_EXTRA_DATA="${TEST_DATA_DIR}/downloaded-extra-data"
curl "${EXTRA_DATA_URL}" -o "${DOWNLOADED_EXTRA_DATA}"
EXTRA_DATA_SIZE=$(stat --printf="%s" "${DOWNLOADED_EXTRA_DATA}")
EXTRA_DATA_SHA256=$(sha256sum "${DOWNLOADED_EXTRA_DATA}" | cut -f1 -d' ')

# build the app with the extra data
EXTRA_DATA_NAME="test"
EXTRA_DATA="--extra-data=${EXTRA_DATA_NAME}:${EXTRA_DATA_SHA256}:${EXTRA_DATA_SIZE}:${EXTRA_DATA_SIZE}:${EXTRA_DATA_URL}"
BUILD_FINISH_ARGS=${EXTRA_DATA} make_updated_app ${REPONAME} ${COLLECTION_ID} ${BRANCH} UPDATE1

# ensure it installs correctly
install_repo ${REPONAME} ${BRANCH}

# ensure the right extra-data got downloaded
${FLATPAK} run --command=sh org.test.Hello -c "cat /app/extra/test" > out
assert_file_has_content out "extra-data-test-content"
assert_not_has_file "${USERDIR}/extra-data/${EXTRA_DATA_SHA256}/${EXTRA_DATA_NAME}"

EXTRA_DATA_COMMIT=$(${FLATPAK} ${U} info --show-commit org.test.Hello)
ostree --repo="${FL_DIR}/repo" show --list-detached-metadata-keys "${EXTRA_DATA_COMMIT}" > detached-keys
assert_not_file_has_content detached-keys "^xa.extra-data$"
assert_file_has_content detached-keys "^ostree.gpgsigs$"

${FLATPAK} ${U} uninstall -y org.test.Hello >&2

ok "install extra data app with ostree"

${FLATPAK} ${U} config --set keep-extra-data true
${FLATPAK} ${U} install -y ${REPONAME}-repo org.test.Hello ${BRANCH} >&2
ostree --repo="${FL_DIR}/repo" show --list-detached-metadata-keys "${EXTRA_DATA_COMMIT}" > detached-keys
assert_file_has_content detached-keys "^xa.extra-data$"
assert_file_has_content detached-keys "^ostree.gpgsigs$"
ok "keep extra data in detached metadata when configured"

${FLATPAK} clean --dry-run > clean-all-dry-run-out
assert_file_has_content clean-all-dry-run-out "Would free:"
assert_file_has_content clean-all-dry-run-out "org.test.Hello"
if test -n "${U}"; then
    assert_file_has_content clean-all-dry-run-out "User installation:"
else
    assert_file_has_content clean-all-dry-run-out "System installation (default):"
fi
ok "clean defaults to all installations"

mkdir -p "${USERDIR}/extra-data/cleanup"
printf partial > "${USERDIR}/extra-data/cleanup/orphan.part"
printf keep > "${USERDIR}/extra-data/cleanup/keep.txt"
mkdir -p "${USERDIR}/extra-data/prefetch-cleanup"
touch "${USERDIR}/extra-data/prefetch-cleanup/.flatpak-prefetch"
printf prefetched > "${USERDIR}/extra-data/prefetch-cleanup/complete"
${FLATPAK} ${U} clean --dry-run > clean-dry-run-out
assert_file_has_content clean-dry-run-out "Would free:"
assert_file_has_content clean-dry-run-out "prefetch-cleanup"
ostree --repo="${FL_DIR}/repo" show --list-detached-metadata-keys "${EXTRA_DATA_COMMIT}" > detached-keys
assert_file_has_content detached-keys "^xa.extra-data$"
assert_has_file "${USERDIR}/extra-data/cleanup/orphan.part"
assert_has_file "${USERDIR}/extra-data/prefetch-cleanup/complete"

# Empty directories inside a deployment are mount points; keep them.
HELLO_DEPLOY="$(${FLATPAK} ${U} info --show-location org.test.Hello)"
mkdir -p "${HELLO_DEPLOY}/files/lib/GL/empty"
${FLATPAK} ${U} clean > clean-out
assert_file_has_content clean-out "Freed:"
assert_has_dir "${HELLO_DEPLOY}/files/lib/GL/empty"
ostree --repo="${FL_DIR}/repo" show --list-detached-metadata-keys "${EXTRA_DATA_COMMIT}" > detached-keys
assert_not_file_has_content detached-keys "^xa.extra-data$"
assert_file_has_content detached-keys "^ostree.gpgsigs$"
assert_not_has_file "${USERDIR}/extra-data/cleanup/orphan.part"
assert_has_file "${USERDIR}/extra-data/cleanup/keep.txt"
assert_not_has_dir "${USERDIR}/extra-data/prefetch-cleanup"
${FLATPAK} run --command=sh org.test.Hello -c "cat /app/extra/test" > out
assert_file_has_content out "extra-data-test-content"
${FLATPAK} ${U} config --unset keep-extra-data
${FLATPAK} ${U} repair >&2
${FLATPAK} ${U} uninstall -y org.test.Hello >&2

# Leftovers of uninstalled apps, deleted remotes and interrupted installs.
mkdir -p "${FL_DIR}/repo/refs/remotes/deleted-remote/app/org.test.Gone/x86_64"
mkdir -p "${FL_DIR}/repo/refs/heads/deploy/app/org.test.Gone"
GONE_DEPLOY="${FL_DIR}/app/org.test.Gone/x86_64/stable/.$(printf %064d 0)-AbC123"
mkdir -p "${GONE_DEPLOY}/files"
printf leftover > "${GONE_DEPLOY}/files/leftover"
touch "${FL_DIR}/repo/state/$(printf %064d 1).commitpartial"
mkdir -p "${HOME}/.var/app/org.test.Gone"
printf data > "${HOME}/.var/app/org.test.Gone/file"
mkdir -p "${USERDIR}/overrides"
printf '[Context]\nshared=network;\n' > "${USERDIR}/overrides/org.test.Gone"

${FLATPAK} ${U} clean --dry-run > clean-leftovers-out
assert_file_has_content clean-leftovers-out "org.test.Gone"
assert_has_dir "${FL_DIR}/repo/refs/remotes/deleted-remote"
assert_has_dir "${GONE_DEPLOY}"

${FLATPAK} ${U} install -y ${REPONAME}-repo org.test.Hello ${BRANCH} >&2
HELLO_DEPLOY="$(${FLATPAK} ${U} info --show-location org.test.Hello)"
mkdir -p "${HELLO_DEPLOY}/files/lib/GL"
${FLATPAK} ${U} clean > clean-leftovers-out
assert_has_dir "${HELLO_DEPLOY}/files/lib/GL"
${FLATPAK} run --command=sh org.test.Hello -c "cat /app/extra/test" > out
assert_file_has_content out "extra-data-test-content"
${FLATPAK} ${U} uninstall -y org.test.Hello >&2
assert_not_has_dir "${FL_DIR}/repo/refs/remotes/deleted-remote"
assert_not_has_dir "${FL_DIR}/repo/refs/heads/deploy/app/org.test.Gone"
assert_has_dir "${FL_DIR}/repo/refs/heads"
assert_not_has_dir "${FL_DIR}/app/org.test.Gone"
assert_not_has_file "${FL_DIR}/repo/state/$(printf %064d 1).commitpartial"
# The data of uninstalled apps is only listed by default
assert_file_has_content clean-leftovers-out "flatpak clean --delete-data"
assert_has_file "${HOME}/.var/app/org.test.Gone/file"
assert_has_file "${USERDIR}/overrides/org.test.Gone"

${FLATPAK} ${U} clean --delete-data > clean-leftovers-out
assert_not_has_dir "${HOME}/.var/app/org.test.Gone"
assert_not_has_file "${USERDIR}/overrides/org.test.Gone"
ok "clean leftovers of uninstalled apps"
: > "${EVENTS_LOG}"
${FLATPAK} ${U} install -y ${REPONAME}-repo org.test.Hello ${BRANCH} >&2
assert_file_has_content "${EVENTS_LOG}" "/${EXTRA_DATA_FILE}"
${FLATPAK} ${U} uninstall -y org.test.Hello >&2
rm -rf "${USERDIR}/extra-data/cleanup"
ok "clean cached extra data"

LOCK_READY="${TEST_DATA_DIR}/clean-lock-ready"
python3 - "${FL_DIR}/repo/.lock" "${LOCK_READY}" <<'PY' &
import fcntl
import sys
import time

with open(sys.argv[1], 'w') as lock_file:
    fcntl.lockf(lock_file, fcntl.LOCK_EX)
    open(sys.argv[2], 'w').close()
    time.sleep(3)
PY
LOCK_PID=$!
while ! test -e "${LOCK_READY}"; do sleep 0.05; done
if ${FLATPAK} ${U} clean > clean-locked-out 2>&1; then
    assert_not_reached
fi
assert_file_has_content clean-locked-out "repository is busy with another transaction"
wait "${LOCK_PID}"
rm "${LOCK_READY}"
ok "reject clean while repository is locked"

# A complete file left by an interrupted prefetch is reused and reclaimed.
mkdir -p "${USERDIR}/extra-data/${EXTRA_DATA_SHA256}"
touch "${USERDIR}/extra-data/${EXTRA_DATA_SHA256}/.flatpak-prefetch"
cp "${DOWNLOADED_EXTRA_DATA}" "${USERDIR}/extra-data/${EXTRA_DATA_SHA256}/${EXTRA_DATA_NAME}"
printf partial > "${USERDIR}/extra-data/${EXTRA_DATA_SHA256}/${EXTRA_DATA_NAME}.part"
: > "${EVENTS_LOG}"
${FLATPAK} ${U} install -y ${REPONAME}-repo org.test.Hello ${BRANCH} >&2
assert_not_file_has_content "${EVENTS_LOG}" "/${EXTRA_DATA_FILE}"
assert_not_has_dir "${USERDIR}/extra-data/${EXTRA_DATA_SHA256}"
${FLATPAK} ${U} uninstall -y org.test.Hello >&2
ok "reuse and reclaim interrupted extra data prefetch"

# An invalid complete file left by an interrupted prefetch is downloaded again.
mkdir -p "${USERDIR}/extra-data/${EXTRA_DATA_SHA256}"
touch "${USERDIR}/extra-data/${EXTRA_DATA_SHA256}/.flatpak-prefetch"
printf invalid > "${USERDIR}/extra-data/${EXTRA_DATA_SHA256}/${EXTRA_DATA_NAME}"
: > "${EVENTS_LOG}"
${FLATPAK} ${U} install -y ${REPONAME}-repo org.test.Hello ${BRANCH} >&2
assert_file_has_content "${EVENTS_LOG}" "/${EXTRA_DATA_FILE}"
assert_not_has_dir "${USERDIR}/extra-data/${EXTRA_DATA_SHA256}"
${FLATPAK} ${U} uninstall -y org.test.Hello >&2
ok "replace invalid interrupted extra data prefetch"

# A file supplied by the user must never be removed by the prefetcher.
mkdir -p "${USERDIR}/extra-data/${EXTRA_DATA_SHA256}"
cp "${DOWNLOADED_EXTRA_DATA}" "${USERDIR}/extra-data/${EXTRA_DATA_SHA256}/${EXTRA_DATA_NAME}"
: > "${EVENTS_LOG}"
${FLATPAK} ${U} install -y ${REPONAME}-repo org.test.Hello ${BRANCH} >&2
assert_not_file_has_content "${EVENTS_LOG}" "/${EXTRA_DATA_FILE}"
assert_has_file "${USERDIR}/extra-data/${EXTRA_DATA_SHA256}/${EXTRA_DATA_NAME}"
${FLATPAK} ${U} uninstall -y org.test.Hello >&2
rm -rf "${USERDIR}/extra-data/${EXTRA_DATA_SHA256}"
ok "preserve user-provided extra data"

make_parallel_extra_data_app () {
    local number="$1"
    local app_id="org.test.ExtraData${number}"
    local file="parallel-${number}"
    local size sha url extra

    printf 'parallel extra data %s\n' "${number}" > "${EXTRA_DATA_DIR}/${file}"
    echo 0.4 > "${EXTRA_DATA_DIR}/${file}.delay"
    size=$(stat --printf="%s" "${EXTRA_DATA_DIR}/${file}")
    sha=$(sha256sum "${EXTRA_DATA_DIR}/${file}" | cut -f1 -d' ')
    url="http://127.0.0.1:$(cat httpd-port)/${file}"
    extra="--extra-data=test:${sha}:${size}:${size}:${url}"
    BUILD_FINISH_ARGS=${extra} make_updated_app ${REPONAME} ${COLLECTION_ID} ${BRANCH} UPDATE "${app_id}"
}

assert_parallel_requests () {
    local expected="$1"
    python3 - "${EVENTS_LOG}" "${expected}" <<'PY'
import sys

log, expected = sys.argv[1:]
active = 0
overlap = False
with open(log) as source:
    for line in source:
        _, event, path = line.split()
        if not path.startswith('/parallel-'):
            continue
        if event == 'start':
            if active:
                overlap = True
            active += 1
        else:
            active -= 1
if overlap != (expected == 'yes'):
    raise SystemExit('request overlap was %s, expected %s' % (overlap, expected))
PY
}

for number in 1 2 3; do
    make_parallel_extra_data_app "${number}"
done

if test -n "${FLATPAK_CLI_PROGRESS_CAPTURE:-}"; then
    : > "${EVENTS_LOG}"
    progress_command="env -u G_MESSAGES_DEBUG -u FLATPAK_FANCY_OUTPUT ${FLATPAK} ${U} install -y ${REPONAME}-repo org.test.ExtraData1 org.test.ExtraData2 org.test.ExtraData3"
    if test -n "${FLATPAK_CLI_PTY_HELPER:-}"; then
        python3 "${FLATPAK_CLI_PTY_HELPER}" "${FLATPAK_CLI_PROGRESS_CAPTURE}" "${progress_command}"
    else
        script -qec "${progress_command}" "${FLATPAK_CLI_PROGRESS_CAPTURE}"
    fi
    ${FLATPAK} ${U} uninstall -y \
        org.test.ExtraData1 org.test.ExtraData2 org.test.ExtraData3 >&2
fi

: > "${EVENTS_LOG}"
${FLATPAK} ${U} install -y ${REPONAME}-repo \
    org.test.ExtraData1 org.test.ExtraData2 org.test.ExtraData3 >&2
assert_parallel_requests yes
${FLATPAK} ${U} uninstall -y \
    org.test.ExtraData1 org.test.ExtraData2 org.test.ExtraData3 >&2
ok "prefetch extra data in parallel"

: > "${EVENTS_LOG}"
FLATPAK_EXTRA_DATA_PARALLEL=1 ${FLATPAK} ${U} install -y ${REPONAME}-repo \
    org.test.ExtraData1 org.test.ExtraData2 org.test.ExtraData3 >&2
assert_parallel_requests no
${FLATPAK} ${U} uninstall -y \
    org.test.ExtraData1 org.test.ExtraData2 org.test.ExtraData3 >&2
ok "disable parallel extra data prefetch"

# Transient prefetch failures are retried and do not fail the operation.
echo "2 500" > "${EXTRA_DATA_DIR}/parallel-1.fail"
${FLATPAK} ${U} install -y ${REPONAME}-repo org.test.ExtraData1 >&2
${FLATPAK} ${U} uninstall -y org.test.ExtraData1 >&2
rm "${EXTRA_DATA_DIR}/parallel-1.fail"
ok "retry failed extra data prefetch"

# Interrupting a transaction drops what the prefetcher downloaded. The
# prefetcher runs in the client either way; a system install would leave
# the system helper's revokefs mount behind until it notices.
if [ x${USE_SYSTEMDIR-} != xyes ] ; then
    for number in 1 2 3; do
        echo 30 > "${EXTRA_DATA_DIR}/parallel-${number}.delay"
    done
    rm -rf "${USERDIR}/extra-data"
    ${FLATPAK} ${U} install -y ${REPONAME}-repo \
        org.test.ExtraData1 org.test.ExtraData2 org.test.ExtraData3 >&2 &
    install_pid=$!
    for _ in $(seq 100); do
        test "$(find "${USERDIR}/extra-data" -name .flatpak-prefetch 2>/dev/null | wc -l)" -ge 2 && break
        sleep 0.1
    done
    kill -INT "${install_pid}"
    wait "${install_pid}" || true
    assert_not_has_dir "${USERDIR}/extra-data"
    for number in 1 2 3; do
        echo 0.4 > "${EXTRA_DATA_DIR}/parallel-${number}.delay"
    done
fi
ok "drop prefetched extra data on interrupt"

# A permanent error still comes from the original pull path.
MISSING_CONTENT="missing extra data"
MISSING_SIZE=${#MISSING_CONTENT}
MISSING_SHA=$(printf %s "${MISSING_CONTENT}" | sha256sum | cut -f1 -d' ')
MISSING_URL="http://127.0.0.1:$(cat httpd-port)/permanently-missing"
BUILD_FINISH_ARGS="--extra-data=test:${MISSING_SHA}:${MISSING_SIZE}:${MISSING_SIZE}:${MISSING_URL}" \
    make_updated_app ${REPONAME} ${COLLECTION_ID} ${BRANCH} UPDATE org.test.ExtraDataMissing
if ${FLATPAK} ${U} install -y ${REPONAME}-repo org.test.ExtraDataMissing >missing-out 2>&1; then
    fatal "Installing permanently missing extra data unexpectedly succeeded"
fi
assert_file_has_content missing-out "While downloading.*permanently-missing"
ok "report permanent extra data failure"

# Start the fake registry server

httpd oci-registry-server.py --dir=.
port=$(cat httpd-port)
scheme=http

client="python3 $test_srcdir/oci-registry-client.py --url=${scheme}://127.0.0.1:${port}"

# Add OCI bundles to it

${FLATPAK} build-bundle --runtime --oci $FL_GPGARGS repos/test oci/platform-image org.test.Platform >&2
$client add platform latest "$(pwd)/oci/platform-image"

${FLATPAK} build-bundle --oci $FL_GPGARGS repos/test oci/app-image org.test.Hello >&2
$client add hello latest "$(pwd)/oci/app-image"

# Add an OCI remote

${FLATPAK} remote-add ${U} oci-registry "oci+${scheme}://127.0.0.1:${port}" >&2

# Check that the images we expect are listed

images=$(${FLATPAK} remote-ls ${U} --columns=app oci-registry | sort | tr '\n' ' ' | sed 's/ $//')
assert_streq "$images" "org.test.Hello org.test.Platform"

${FLATPAK} ${U} install -y oci-registry org.test.Hello >&2

# ensure the right extra-data got downloaded
${FLATPAK} run --command=sh org.test.Hello -c "cat /app/extra/test" > out
assert_file_has_content out "extra-data-test-content"

${FLATPAK} ${U} uninstall -y org.test.Hello >&2

ok "install extra data app with oci"

build_extra_data_noruntime_app() {
    local repo="$1"
    local branch="$2"

    DIR="$(mktemp -d)"
    ARCH="$(flatpak --default-arch)"

    mkdir -p "${DIR}/files/bin"

    cp "${G_TEST_BUILDDIR}/apply-extra-static" "${DIR}/files/bin/apply_extra"
    chmod +x "${DIR}/files/bin/apply_extra"

    cat > "${DIR}/metadata" <<EOF
[Application]
name=org.test.ExtraDataNoRuntime
runtime=org.test.Platform/${ARCH}/${branch}

[Extra Data]
NoRuntime=true
uri=${EXTRA_DATA_URL}
size=${EXTRA_DATA_SIZE}
checksum=${EXTRA_DATA_SHA256}
EOF

    flatpak build-finish "${DIR}" >&2
    flatpak build-export ${FL_GPGARGS} "${repo}" "${DIR}" "${branch}" >&2
    rm -rf "${DIR}"
}

if test -f "${G_TEST_BUILDDIR}/apply-extra-static"; then
    build_extra_data_noruntime_app repos/test ${BRANCH}
    update_repo ${REPONAME} ${COLLECTION_ID}
    ${FLATPAK} ${U} install --or-update -y ${REPONAME}-repo org.test.ExtraDataNoRuntime ${BRANCH} >&2
    DEPLOY_DIR="$(${FLATPAK} ${U} info --show-location org.test.ExtraDataNoRuntime)"
    assert_file_has_content "${DEPLOY_DIR}/files/extra/ran" "^noruntime-extra-data$"
    ${FLATPAK} ${U} uninstall -y org.test.ExtraDataNoRuntime >&2
    ok "install+run extra data app with NoRuntime"
else
    ok "# SKIP install+run extra data app with NoRuntime apply-extra-static not found"
fi

done_testing
