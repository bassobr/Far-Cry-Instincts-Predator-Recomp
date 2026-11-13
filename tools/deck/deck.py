#!/usr/bin/env python3
"""Small SSH/SFTP helper for Steam Deck test runs (paramiko).

Connection data only from parameters or the environment, never from this file:
  --host / FCP_DECK_HOST       address of the Deck
  --user / FCP_DECK_USER       login (default: deck)
  FCP_DECK_PASSWORD            password (only if no SSH key works); never logged or written anywhere

The host key must already be in ~/.ssh/known_hosts (RejectPolicy).

Commands:
  run  "<shell command>"              run remotely, stream stdout/stderr, exit with the remote exit code
  put  <local> <remote>               upload a file or a directory tree (SFTP), prints MB/s
  get  <remote> <local>               download one file
  pull <remote_dir> <local_dir>       download a directory tree as tar.gz over the SSH channel
  script <local.sh> [args...]         pipe a local shell script into `bash -s` (nothing is left on the Deck)
"""
import argparse
import os
import posixpath
import shlex
import stat
import sys
import tarfile
import io
import time

import paramiko


def connect(args):
    host = args.host or os.environ.get("FCP_DECK_HOST")
    user = args.user or os.environ.get("FCP_DECK_USER") or "deck"
    if not host:
        sys.exit("deck.py: no host (--host or FCP_DECK_HOST)")
    pw = os.environ.get("FCP_DECK_PASSWORD") or None
    c = paramiko.SSHClient()
    c.load_system_host_keys()
    c.set_missing_host_key_policy(paramiko.RejectPolicy())
    c.connect(host, username=user, password=pw, timeout=15, allow_agent=False,
              look_for_keys=pw is None, banner_timeout=30, auth_timeout=30)
    t = c.get_transport()
    t.set_keepalive(20)
    t.default_window_size = 64 * 1024 * 1024
    return c


def cmd_run(c, command, stdin_data=None):
    chan = c.get_transport().open_session()
    chan.set_combine_stderr(False)
    chan.exec_command(command)
    if stdin_data is not None:
        chan.sendall(stdin_data)
        chan.shutdown_write()
    out = sys.stdout.buffer
    err = sys.stderr.buffer
    while True:
        did = False
        if chan.recv_ready():
            out.write(chan.recv(65536)); out.flush(); did = True
        if chan.recv_stderr_ready():
            err.write(chan.recv_stderr(65536)); err.flush(); did = True
        if chan.exit_status_ready() and not chan.recv_ready() and not chan.recv_stderr_ready():
            break
        if not did:
            time.sleep(0.02)
    return chan.recv_exit_status()


def _put_file(sftp, local, remote, totals):
    size = os.path.getsize(local)
    try:
        st = sftp.stat(remote)
        if st.st_size == size and int(st.st_mtime) == int(os.path.getmtime(local)):
            totals["skipped"] += 1
            return
    except IOError:
        pass
    if local.endswith(".sh"):  # bash on the Deck: LF only, whatever the checkout has
        with open(local, "rb") as f:
            data = f.read().replace(b"\r\n", b"\n")
        with sftp.open(remote, "wb") as out:
            out.write(data)
        sftp.chmod(remote, 0o755)
    else:
        sftp.put(local, remote, confirm=True)
    m = os.path.getmtime(local)
    sftp.utime(remote, (m, m))
    totals["bytes"] += size
    totals["files"] += 1


def _mkdirs(sftp, path):
    parts = []
    while path not in ("", "/"):
        try:
            sftp.stat(path)
            break
        except IOError:
            parts.append(path)
            path = posixpath.dirname(path)
    for p in reversed(parts):
        sftp.mkdir(p)


def cmd_put(c, local, remote):
    sftp = c.open_sftp()
    totals = {"bytes": 0, "files": 0, "skipped": 0}
    t0 = time.time()
    if os.path.isdir(local):
        for root, _dirs, files in os.walk(local):
            rel = os.path.relpath(root, local).replace("\\", "/")
            rdir = remote if rel == "." else posixpath.join(remote, rel)
            _mkdirs(sftp, rdir)
            for f in files:
                _put_file(sftp, os.path.join(root, f), posixpath.join(rdir, f), totals)
    else:
        _mkdirs(sftp, posixpath.dirname(remote))
        _put_file(sftp, local, remote, totals)
    dt = time.time() - t0
    mb = totals["bytes"] / 1e6
    print(f"put: {totals['files']} files ({totals['skipped']} unchanged skipped), {mb:.1f} MB in {dt:.1f} s"
          f" = {mb / dt if dt > 0 else 0:.1f} MB/s")
    return 0


def cmd_get(c, remote, local):
    sftp = c.open_sftp()
    os.makedirs(os.path.dirname(os.path.abspath(local)), exist_ok=True)
    sftp.get(remote, local)
    print(f"get: {remote} -> {local} ({os.path.getsize(local)} bytes)")
    return 0


def cmd_pull(c, remote_dir, local_dir):
    chan = c.get_transport().open_session()
    # a leading ~/ must stay outside the quotes, or the remote shell does not expand it
    rd = remote_dir[2:] if remote_dir.startswith("~/") else remote_dir
    where = ('"$HOME"/' if rd is not remote_dir else "") + shlex.quote(rd)
    chan.exec_command(f"tar -C {where} -czf - .")
    buf = io.BytesIO()
    while True:
        d = chan.recv(1 << 20)
        if not d:
            break
        buf.write(d)
    rc = chan.recv_exit_status()
    buf.seek(0)
    os.makedirs(local_dir, exist_ok=True)
    with tarfile.open(fileobj=buf, mode="r:gz") as tf:
        tf.extractall(local_dir, filter="data")
    print(f"pull: {remote_dir} -> {local_dir} ({buf.getbuffer().nbytes} bytes tar.gz, rc {rc})")
    return rc


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host")
    ap.add_argument("--user")
    ap.add_argument("cmd", choices=["run", "put", "get", "pull", "script"])
    ap.add_argument("args", nargs="*")
    a = ap.parse_args()
    c = connect(a)
    try:
        if a.cmd == "run":
            rc = cmd_run(c, " ".join(a.args))
        elif a.cmd == "put":
            rc = cmd_put(c, a.args[0], a.args[1])
        elif a.cmd == "get":
            rc = cmd_get(c, a.args[0], a.args[1])
        elif a.cmd == "pull":
            rc = cmd_pull(c, a.args[0], a.args[1])
        else:
            with open(a.args[0], "rb") as f:
                data = f.read().replace(b"\r\n", b"\n")
            quoted = " ".join("'" + x.replace("'", "'\\''") + "'" for x in a.args[1:])
            rc = cmd_run(c, f"bash -s -- {quoted}", stdin_data=data)
    finally:
        c.close()
    sys.exit(rc)


if __name__ == "__main__":
    main()
