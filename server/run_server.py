#!/usr/bin/env python3
"""
Runs the Aether Clash lobby server and keeps it up to date.

Every UPDATE_MINUTES it asks GitHub for the newest aether_server.py. If it has changed, the new file is
saved and the server is restarted - but only once nobody is connected, so matches are never cut off.

Settings (environment variables, all optional):
  AETHER_PORT      port to listen on                      (default 7777)
  UPDATE_MINUTES   how often to look for a new version    (default 15, 0 = never update)
  GITHUB_TOKEN     a read-only GitHub token. Needed only while the repository is private.
  AETHER_REPO      owner/name of the repository           (default sneekyedd/AetherClash)
"""
import os
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.join(HERE, "aether_server.py")
if not os.path.exists(SERVER):                      # running from a checkout: the server sits one folder up
    SERVER = os.path.join(os.path.dirname(HERE), "aether_server.py")
STATUS = os.path.join(HERE, "players.txt")
REPO = os.environ.get("AETHER_REPO", "sneekyedd/AetherClash")
MINUTES = float(os.environ.get("UPDATE_MINUTES", "15"))


def fetch_latest():
    headers = {"User-Agent": "AetherClashServer", "Accept": "application/vnd.github.raw"}
    if os.environ.get("GITHUB_TOKEN"):
        headers["Authorization"] = "Bearer " + os.environ["GITHUB_TOKEN"]
    req = urllib.request.Request(f"https://api.github.com/repos/{REPO}/contents/aether_server.py", headers=headers)
    with urllib.request.urlopen(req, timeout=15) as r:
        data = r.read()
    compile(data, "aether_server.py", "exec")       # refuse anything that is not valid Python
    return data


def players():
    try:
        with open(STATUS) as fh:
            return int(fh.read().strip() or 0)
    except (OSError, ValueError):
        return 0


def start():
    env = dict(os.environ, AETHER_STATUS_FILE=STATUS)
    return subprocess.Popen([sys.executable, SERVER], env=env)


def main():
    child, pending, last_check = start(), None, time.time()
    while True:
        time.sleep(5)
        if child.poll() is not None:                # crashed: bring it back
            print("server stopped, restarting", flush=True)
            time.sleep(2)
            child = start()
        if MINUTES > 0 and time.time() - last_check >= MINUTES * 60:
            last_check = time.time()
            try:
                new = fetch_latest()
                with open(SERVER, "rb") as fh:
                    if new != fh.read():
                        pending = new
                        print("update found, waiting for the server to be empty", flush=True)
            except Exception as e:                  # offline, private repo without a token, ...: try again later
                print("update check failed:", e, flush=True)
        if pending and players() == 0:
            with open(SERVER, "wb") as fh:
                fh.write(pending)
            pending = None
            child.terminate()
            child.wait(timeout=10)
            child = start()
            print("server updated and restarted", flush=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
