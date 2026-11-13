#!/usr/bin/env python3
"""Small SSH/SFTP helper for test runs on a Windows handheld (ROG Ally / Ally X with Windows OpenSSH), paramiko.

Connection data only from parameters or the environment, never from this file:
  --host / FCP_ALLY_HOST       address of the device
  --user / FCP_ALLY_USER       login (may contain spaces; default: the local user name)
Login by SSH key only (~/.ssh/id_ed25519 etc.); no password is ever asked for or sent. The host key must already
be in ~/.ssh/known_hosts (RejectPolicy).

The remote shell of Windows OpenSSH is cmd.exe. PowerShell is run with -EncodedCommand, so no quoting has to survive
cmd.exe and no byte-order mark from a pipe reaches the remote parser.

Commands:
  run  "<cmd.exe command>"            run remotely, stream stdout/stderr, exit with the remote exit code
  ps   "<PowerShell code>"            run PowerShell code remotely
  psf  <local.ps1> [args...]          run a local PowerShell script remotely (its param() block gets the args)
  put  <local> <remote>               upload a file or a directory tree (SFTP), prints MB/s; skips unchanged files
  get  <remote> <local>               download one file
  pull <remote_dir> <local_dir>       download a directory tree (tar.gz over the SSH channel, Windows tar)
Remote paths: "~/x" is relative to the remote user's profile folder, otherwise SFTP form ("/C:/<profile folder>/...").
"""
import argparse
import base64
import getpass
import io
import os
import posixpath
import sys
import tarfile
import time

import paramiko


def connect(args):
    host = args.host or os.environ.get("FCP_ALLY_HOST")
    user = args.user or os.environ.get("FCP_ALLY_USER") or getpass.getuser()
    if not host:
        sys.exit("ally.py: no host (--host or FCP_ALLY_HOST)")
    c = paramiko.SSHClient()
    c.load_system_host_keys()
    c.set_missing_host_key_policy(paramiko.RejectPolicy())
    c.connect(host, username=user, password=None, timeout=15, allow_agent=False, look_for_keys=True,
              banner_timeout=30, auth_timeout=30)
    t = c.get_transport()
    t.set_keepalive(20)
    t.default_window_size = 64 * 1024 * 1024
    return c


def cmd_run(c, command):
    chan = c.get_transport().open_session()
    chan.set_combine_stderr(False)
    chan.exec_command(command)
    out, err = sys.stdout.buffer, sys.stderr.buffer
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


def ps_command(code):
    # UTF-8 output so the remote text survives; ProgressPreference off (the progress stream breaks over SSH)
    full = ("$ProgressPreference='SilentlyContinue'; [Console]::OutputEncoding=[Text.Encoding]::UTF8\n" + code)
    enc = base64.b64encode(full.encode("utf-16-le")).decode("ascii")
    return f"powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand {enc}"


def ps_quote(s):
    return "'" + s.replace("'", "''") + "'"


class Remote:
    def __init__(self, c):
        self.sftp = c.open_sftp()
        self.home = self.sftp.normalize(".")  # SFTP form of the remote profile folder

    def path(self, p):
        p = p.replace("\\", "/")
        if p == "~":
            return self.home
        if p.startswith("~/"):
            return posixpath.join(self.home, p[2:])
        return p


def _put_file(sftp, local, remote, totals):
    size = os.path.getsize(local)
    try:
        st = sftp.stat(remote)
        if st.st_size == size and int(st.st_mtime) == int(os.path.getmtime(local)):
            totals["skipped"] += 1
            return
    except IOError:
        pass
    sftp.put(local, remote, confirm=True)
    m = os.path.getmtime(local)
    sftp.utime(remote, (m, m))
    totals["bytes"] += size
    totals["files"] += 1


def _mkdirs(sftp, path):
    parts = []
    while path not in ("", "/") and not path.endswith(":"):
        try:
            sftp.stat(path)
            break
        except IOError:
            parts.append(path)
            path = posixpath.dirname(path)
    for p in reversed(parts):
        sftp.mkdir(p)


def cmd_put(c, local, remote):
    r = Remote(c)
    remote = r.path(remote)
    totals = {"bytes": 0, "files": 0, "skipped": 0}
    t0 = time.time()
    if os.path.isdir(local):
        for root, _dirs, files in os.walk(local):
            rel = os.path.relpath(root, local).replace("\\", "/")
            rdir = remote if rel == "." else posixpath.join(remote, rel)
            _mkdirs(r.sftp, rdir)
            for f in files:
                _put_file(r.sftp, os.path.join(root, f), posixpath.join(rdir, f), totals)
    else:
        _mkdirs(r.sftp, posixpath.dirname(remote))
        _put_file(r.sftp, local, remote, totals)
    dt = time.time() - t0
    mb = totals["bytes"] / 1e6
    print(f"put: {totals['files']} files ({totals['skipped']} unchanged skipped), {mb:.1f} MB in {dt:.1f} s"
          f" = {mb / dt if dt > 0 else 0:.1f} MB/s")
    return 0


def cmd_get(c, remote, local):
    r = Remote(c)
    os.makedirs(os.path.dirname(os.path.abspath(local)), exist_ok=True)
    r.sftp.get(r.path(remote), local)
    print(f"get: {remote} -> {local} ({os.path.getsize(local)} bytes)")
    return 0


def cmd_pull(c, remote_dir, local_dir):
    r = Remote(c)
    win = r.path(remote_dir).lstrip("/").replace("/", "\\")  # Windows form of the remote path
    chan = c.get_transport().open_session()
    chan.exec_command(f'tar -czf - -C "{win}" .')
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
    ap.add_argument("cmd", choices=["run", "ps", "psf", "put", "get", "pull"])
    ap.add_argument("args", nargs="*")
    a = ap.parse_args()
    c = connect(a)
    try:
        if a.cmd == "run":
            rc = cmd_run(c, " ".join(a.args))
        elif a.cmd == "ps":
            rc = cmd_run(c, ps_command(" ".join(a.args)))
        elif a.cmd == "psf":
            with open(a.args[0], encoding="utf-8-sig") as f:
                body = f.read()
            call = "& {\n" + body + "\n} " + " ".join(ps_quote(x) for x in a.args[1:])
            rc = cmd_run(c, ps_command(call))
        elif a.cmd == "put":
            rc = cmd_put(c, a.args[0], a.args[1])
        elif a.cmd == "get":
            rc = cmd_get(c, a.args[0], a.args[1])
        else:
            rc = cmd_pull(c, a.args[0], a.args[1])
    finally:
        c.close()
    sys.exit(rc)


if __name__ == "__main__":
    main()
