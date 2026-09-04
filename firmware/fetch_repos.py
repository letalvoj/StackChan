import os
import subprocess
import json


def _has_local_work(path, local_branch):
    """True if `path` holds work that may only exist here.

    xiaozhi-esp32 is a git-ignored checkout carrying the fork's own commits (the
    ApplicationCore extraction, UsbProtocol, WebsocketServerProtocol selection).
    Those now live on the `wasm` branch of letalvoj/xiaozhi-esp32, so a fresh
    clone gets them -- but an existing checkout may still hold work that has not
    been pushed yet, and a plain `git checkout <ref>` would silently swap the
    working tree out from under it. Refuse to touch such a checkout at all.
    """
    if not local_branch or not os.path.exists(path):
        return False

    exists = subprocess.run(
        ["git", "-C", path, "rev-parse", "--verify", "--quiet", local_branch],
        capture_output=True,
    )
    return exists.returncode == 0


def clone_or_update_repo(
    repo_url, path, ref=None, with_submodules=False, local_branch=None
):
    if _has_local_work(path, local_branch):
        print(
            f"Skipping {path}: local-only branch '{local_branch}' is present. "
            f"Refusing to check out '{ref}' over it."
        )
        return

    if not os.path.exists(path):
        subprocess.run(["git", "clone", repo_url, path], check=True)
    else:
        subprocess.run(["git", "-C", path, "fetch"], check=True)

    if ref:
        subprocess.run(["git", "-C", path, "checkout", ref], check=True)

    if with_submodules:
        subprocess.run(
            ["git", "-C", path, "submodule", "update", "--init", "--recursive"],
            check=True,
        )


def fetch_dependencies():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    config_path = os.path.join(script_dir, "repos.json")

    with open(config_path) as f:
        repos = json.load(f)

    for repo in repos:
        repo_path = os.path.join(script_dir, repo["path"])
        clone_or_update_repo(
            repo["url"],
            repo_path,
            repo.get("branch"),
            repo.get("with_submodules", False),
            local_branch=repo.get("local_branch"),
        )


if __name__ == "__main__":
    fetch_dependencies()