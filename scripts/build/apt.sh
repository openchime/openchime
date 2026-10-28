#!/usr/bin/env bash
# Install Ubuntu packages on a GitHub runner without hanging the job: every
# apt step in the workflows runs through here.
#
#   scripts/build/apt.sh <package>...
#
# Two things have hung these steps. azure.archive.ubuntu.com, the Azure-local
# mirror the hosted runners point at, stalls: apt gives up on it, falls back to
# archive.ubuntu.com for InRelease, then wedges on the Packages fetch (every
# noble entry Ign:, 20 minutes, ubuntu-24.04), so the mirror is rewritten away
# before anything is fetched. And with that in place, a packages job has
# still sat in `apt-get update -qq` for the job's whole two hours, printing nothing: apt's own timeouts cover a connection that fails,
# not one that stops answering. So each apt-get runs under `timeout`, is tried
# three times, over IPv4 only (a v6 route that black-holes is one way a
# transfer stops without an error), waits for dpkg's lock rather than
# failing on it (unattended-upgrades holds it on a fresh runner), and prints
# what it fetches, so a stall is visible in the log and bounded in time.
set -euo pipefail
[ "$#" -gt 0 ] || { echo "usage: apt.sh <package>..." >&2; exit 2; }
sudo sed -i 's|http://azure.archive.ubuntu.com/ubuntu|https://archive.ubuntu.com/ubuntu|g' \
  /etc/apt/sources.list /etc/apt/sources.list.d/*.sources /etc/apt/sources.list.d/*.list 2>/dev/null || true
# The runner image's sources do not name the mirror: they point at a mirror
# LIST (`URIs: mirror+file:/etc/apt/apt-mirrors.txt`), whose first entry is
# the Azure mirror, so the rewrite above alone changes nothing and a job
# still fetches from azure. The list is rewritten too.
if [ -f /etc/apt/apt-mirrors.txt ] && grep -q azure /etc/apt/apt-mirrors.txt; then
  printf 'https://archive.ubuntu.com/ubuntu/\tpriority:1\nhttp://security.ubuntu.com/ubuntu/\tpriority:2\n' \
    | sudo tee /etc/apt/apt-mirrors.txt >/dev/null
fi
printf 'Acquire::Retries "3";\nAcquire::http::Timeout "15";\nAcquire::https::Timeout "15";\nAcquire::ForceIPv4 "true";\nDpkg::Lock::Timeout "120";\n' \
  | sudo tee /etc/apt/apt.conf.d/99-ci-timeouts >/dev/null
for try in 1 2 3; do
  if sudo timeout 300 apt-get update -q &&
     sudo DEBIAN_FRONTEND=noninteractive timeout 600 apt-get install -y -q --no-install-recommends "$@"; then
    echo "apt.sh: installed $* (try ${try})"
    exit 0
  fi
  echo "apt.sh: try ${try} failed or timed out" >&2
  sleep 10
done
echo "apt.sh: could not install $* in three tries" >&2
exit 1
