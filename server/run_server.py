#!/usr/bin/env python3
"""
Runs the Aether Clash lobby server and keeps everything up to date.

  * Game updates: every RELEASE_MINUTES it looks up the newest release on GitHub and keeps a copy of its
    AetherClash.exe in the "updates" folder. The lobby server hands that copy to players' games when they
    start, so their games update themselves (this works while the GitHub repository is private, because only
    this server needs the token).
  * Server updates: every UPDATE_MINUTES it fetches the newest aether_server.py. If it changed, the new file
    is saved and the server restarts onto it - but only once nobody is connected, so no match is cut off.
  * If the server ever stops, it is started again.

Settings (environment variables, all optional):
  AETHER_PORT       port to listen on                              (default 7777)
  GITHUB_TOKEN      read-only GitHub token; needed while the repository is private
  RELEASE_MINUTES   how often to look for a new game release        (default 5, 0 = never)
  UPDATE_MINUTES    how often to look for a new server version      (default 15, 0 = never)
  AETHER_REPO       owner/name of the repository                    (default sneekyedd/AetherClash)
"""
import hashlib
import json
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
UPDATES = os.environ.get("AETHER_UPDATE_DIR") or os.path.join(HERE, "updates")
REPO = os.environ.get("AETHER_REPO", "sneekyedd/AetherClash")
SERVER_MINUTES = float(os.environ.get("UPDATE_MINUTES", "15"))
RELEASE_MINUTES = float(os.environ.get("RELEASE_MINUTES", "5"))


def log(*parts):
    print(time.strftime("%Y-%m-%d %H:%M:%S"), *parts, flush=True)


def github(path, accept="application/vnd.github+json", timeout=20):
    req = urllib.request.Request(f"https://api.github.com/repos/{REPO}/{path}",
                                 headers={"User-Agent": "AetherClashServer", "Accept": accept})
    if os.environ.get("GITHUB_TOKEN"):
        # "unredirected" so the token is not passed on when GitHub redirects a download to its file storage
        req.add_unredirected_header("Authorization", "Bearer " + os.environ["GITHUB_TOKEN"])
    return urllib.request.urlopen(req, timeout=timeout)


def fetch_latest_server():
    with github("contents/aether_server.py", accept="application/vnd.github.raw") as r:
        data = r.read()
    compile(data, "aether_server.py", "exec")       # refuse anything that is not valid Python
    return data


def mirror_latest_release():
    """Keeps updates/AetherClash.exe and updates/latest.json matching the newest GitHub release."""
    with github("releases/latest") as r:
        rel = json.load(r)
    tag = rel.get("tag_name", "")
    asset = next((a for a in rel.get("assets", []) if a["name"].lower().endswith(".exe")), None)
    if not asset:
        return
    try:
        with open(os.path.join(UPDATES, "latest.json")) as fh:
            if json.load(fh).get("tag") == tag:
                return                                  # already have it
    except (OSError, ValueError):
        pass
    log("downloading game release", tag)
    os.makedirs(UPDATES, exist_ok=True)
    tmp = os.path.join(UPDATES, "AetherClash.exe.part")
    digest = hashlib.sha256()
    with github(f"releases/assets/{asset['id']}", accept="application/octet-stream", timeout=120) as r, \
            open(tmp, "wb") as out:
        while True:
            chunk = r.read(1 << 16)
            if not chunk:
                break
            out.write(chunk)
            digest.update(chunk)
    if os.path.getsize(tmp) != asset.get("size", os.path.getsize(tmp)):
        os.remove(tmp)
        raise IOError("incomplete download")
    os.replace(tmp, os.path.join(UPDATES, "AetherClash.exe"))
    info = {"tag": tag, "url": "/aether/AetherClash.exe", "sha256": digest.hexdigest()}
    with open(os.path.join(UPDATES, "latest.json.part"), "w") as fh:
        json.dump(info, fh)
    os.replace(os.path.join(UPDATES, "latest.json.part"), os.path.join(UPDATES, "latest.json"))
    log("players will now update to", tag)


def players():
    try:
        with open(STATUS) as fh:
            return int(fh.read().strip() or 0)
    except (OSError, ValueError):
        return 0


def start():
    env = dict(os.environ, AETHER_STATUS_FILE=STATUS, AETHER_UPDATE_DIR=UPDATES)
    return subprocess.Popen([sys.executable, SERVER], env=env)


def main():
    child, pending = start(), None
    last_server = time.time()
    last_release = 0.0                              # check for a game release straight away
    while True:
        if RELEASE_MINUTES > 0 and time.time() - last_release >= RELEASE_MINUTES * 60:
            last_release = time.time()
            try:
                mirror_latest_release()
            except Exception as e:                  # offline, no token for a private repo, ...: try again later
                log("game release check failed:", e)
        if SERVER_MINUTES > 0 and time.time() - last_server >= SERVER_MINUTES * 60:
            last_server = time.time()
            try:
                new = fetch_latest_server()
                with open(SERVER, "rb") as fh:
                    if new != fh.read():
                        pending = new
                        log("server update found, waiting for the server to be empty")
            except Exception as e:
                log("server update check failed:", e)
        if pending and players() == 0:
            with open(SERVER, "wb") as fh:
                fh.write(pending)
            pending = None
            child.terminate()
            child.wait(timeout=10)
            child = start()
            log("server updated and restarted")
        if child.poll() is not None:                # crashed: bring it back
            log("server stopped, restarting")
            time.sleep(2)
            child = start()
        time.sleep(5)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
