import os
import subprocess
import json


def _has_local_work(path, local_branch):
    """True if `path` holds work that only exists here.

    xiaozhi-esp32 is a git-ignored checkout carrying local-only commits (the
    ApplicationCore extraction and UsbProtocol) on a branch that is deliberately
    never pushed anywhere. A plain `git checkout <tag>` would silently swap the
    working tree back to vanilla upstream and the build would quietly lose those
    features, so refuse to touch such a checkout at all.
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
        if local_branch:
            print(
                f"WARNING: {path} was cloned fresh at '{ref}'. The local-only "
                f"'{local_branch}' branch is NOT in this checkout and cannot be "
                f"recovered from any remote."
            )
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