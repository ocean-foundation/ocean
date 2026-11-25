#!/usr/bin/env python3
"""Select and tag the snapshots for an Ocean nightly."""
import datetime as dt
import json
import os
from pathlib import Path
import subprocess

from release import git, output, previous_release, published_releases


def revision(repo, tag):
    result = subprocess.run(["git", "-C", str(repo), "rev-parse", "--verify",
                             "--quiet", f"refs/tags/{tag}^{{commit}}"],
                            text=True, capture_output=True, check=False)
    return result.stdout.strip() if result.returncode == 0 else None


def require_main(repo, commit):
    if subprocess.run(["git", "-C", str(repo), "merge-base", "--is-ancestor",
                       commit, "origin/main"], check=False).returncode:
        raise ValueError(f"{repo}: {commit} is outside origin/main")


def publish_tag(repo, tag, commit, snapshot):
    current = revision(repo, tag)
    if current:
        if current != commit:
            raise ValueError(f"{repo}: {tag} already points at {current}")
        return
    git(repo, "-c", "user.name=github-actions[bot]", "-c",
        "user.email=41898282+github-actions[bot]@users.noreply.github.com",
        "tag", "-a", tag, commit, "-m", json.dumps(snapshot, sort_keys=True))
    git(repo, "push", "origin", f"refs/tags/{tag}:refs/tags/{tag}")


def snapshot_from_tag(repo, tag):
    try:
        snapshot = json.loads(git(repo, "for-each-ref", "--format=%(contents)", f"refs/tags/{tag}"))
    except json.JSONDecodeError:
        return {}
    return snapshot if isinstance(snapshot, dict) else {}


def nightly_tag(started_at):
    run_date = started_at.astimezone(dt.timezone.utc).date()
    return "nightly-" + (run_date - dt.timedelta(days=1)).strftime("%Y.%m.%d")


def main():
    repo = os.environ["GITHUB_REPOSITORY"]
    run_id = os.environ["GITHUB_RUN_ID"]
    created_at = output("gh", "api", f"repos/{repo}/actions/runs/{run_id}", "--jq", ".created_at")
    started_at = dt.datetime.fromisoformat(created_at.replace("Z", "+00:00"))
    tag = nightly_tag(started_at)
    releases = published_releases(repo)
    if any(release["tag_name"] == tag for release in releases):
        print(f"{tag} is already published")
        return

    compiler, stdlib = Path.cwd(), Path("build/.nightly-stdlib")
    main_tags = set(git(compiler, "tag", "--merged", "origin/main").splitlines())
    previous = previous_release(releases, main_tags, tag)
    stdlib_commit = revision(stdlib, tag)
    # An earlier attempt may have pushed the stdlib tag before pushing the compiler tag.
    snapshot = snapshot_from_tag(stdlib, tag) if stdlib_commit else {}
    compiler_commit = revision(compiler, tag)
    retry = bool(compiler_commit or snapshot.get("ocean"))
    if compiler_commit:
        recorded = snapshot_from_tag(compiler, tag).get("stdlib")
        if recorded and recorded != stdlib_commit:
            raise ValueError(f"{tag} has an unexpected standard-library commit")
    if not compiler_commit:
        compiler_commit = snapshot.get("ocean") or git(compiler, "rev-parse", "origin/main")
    stdlib_commit = stdlib_commit or git(stdlib, "rev-parse", "origin/main")
    require_main(compiler, compiler_commit)
    require_main(stdlib, stdlib_commit)

    if previous and not retry:
        previous_tag = previous["tag_name"]
        if (compiler_commit, stdlib_commit) == (revision(compiler, previous_tag), revision(stdlib, previous_tag)):
            published_at = dt.datetime.fromisoformat(previous["published_at"].replace("Z", "+00:00"))
            if started_at - published_at < dt.timedelta(days=30):
                print(f"Skipping {tag}: both repositories match {previous_tag}, published less than 30 days ago")
                return
            print(f"Tagging {tag}: 30 days have passed since {previous_tag}")

    snapshot = {"ocean": compiler_commit, "stdlib": stdlib_commit}
    publish_tag(stdlib, tag, stdlib_commit, snapshot)
    publish_tag(compiler, tag, compiler_commit, snapshot)
    print(f"{tag} points to the selected Ocean and standard-library snapshots")


if __name__ == "__main__":
    main()
