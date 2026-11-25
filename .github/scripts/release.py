#!/usr/bin/env python3
"""Build and package tagged Ocean releases."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tarfile
import zipfile

from dependency_bundle import build_bundle


LEGACY_REPO = "ocean-foundation/ocean-c"
LEGACY_TAG = "nightly-2026.11.22"
NIGHTLY = re.compile(r"nightly-\d{4}\.\d{2}\.\d{2}")
VERSION = re.compile(r"v?\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?")


def output(*args):
    return subprocess.check_output(args, text=True).strip()


def git(repo, *args):
    return output("git", "-C", str(repo), *args)


def published_releases(repo):
    pages = json.loads(output("gh", "api", "--paginate", "--slurp", f"repos/{repo}/releases?per_page=100"))
    return [release for page in pages for release in page if not release["draft"] and release["published_at"]]


def previous_release(releases, main_tags, tag):
    candidates = [release for release in releases
                  if release["tag_name"] in main_tags and release["tag_name"] != tag
                  and (not NIGHTLY.fullmatch(tag) or not NIGHTLY.fullmatch(release["tag_name"])
                       or release["tag_name"] < tag)]
    return max(candidates, key=lambda release: release["published_at"], default=None)


def build_matrix():
    manifest = json.loads(Path("release-deps.json").read_text(encoding="utf-8"))
    matrix = []
    for target in manifest["targets"]:
        if not target["profile"]:
            continue
        profile = manifest["profiles"][target["profile"]]
        packages = [profile["packages"]]
        if target["system"] == "windows":
            packages.extend(f"{target['prefix']}-{name}" for name in profile["native_packages"])
        matrix.append(dict(target, packages=" ".join(packages), toolchain=profile.get("toolchain", "")))
    return {"include": matrix}


def prepare(args):
    tag = args.tag
    if not (NIGHTLY.fullmatch(tag) or VERSION.fullmatch(tag)):
        raise ValueError(f"Unsupported release tag: {tag}")
    repo = os.environ["GITHUB_REPOSITORY"]
    releases = published_releases(repo)
    if any(release["tag_name"] == tag for release in releases):
        print(f"{tag} is already published")
        return
    main_tags = set(git(Path.cwd(), "tag", "--merged", "origin/main").splitlines())
    previous = previous_release(releases, main_tags, tag)
    matrix = build_matrix()
    prerelease = "-" in tag.split("+", 1)[0]
    with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as stream:
        print(f"tag={tag}", file=stream)
        print(f"bootstrap_repo={repo if previous else LEGACY_REPO}", file=stream)
        print(f"bootstrap_tag={previous['tag_name'] if previous else LEGACY_TAG}", file=stream)
        print(f"matrix={json.dumps(matrix, separators=(',', ':'))}", file=stream)
        print(f"prerelease={'true' if prerelease else 'false'}", file=stream)


def checksum(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def bootstrap(args):
    _, system = args.target.split("-", 1)
    project = "ocean-c" if args.repo == LEGACY_REPO else "ocean"
    suffix = ".exe" if system == "windows" else ""
    archive = f"{project}_{args.tag}_{args.target}.tar"
    checksums = f"SHA256SUMS_{args.target}.txt"
    directory = Path("build/bootstrap")
    directory.mkdir(parents=True, exist_ok=True)
    subprocess.run(["gh", "release", "download", args.tag, "--repo", args.repo,
                    "--dir", str(directory), "--clobber", "--pattern", archive,
                    "--pattern", checksums], check=True)
    expected = dict(line.split(maxsplit=1)[::-1]
                    for line in (directory / checksums).read_text(encoding="utf-8").splitlines() if line.strip())
    if checksum(directory / archive) != expected.get(archive):
        raise RuntimeError(f"SHA-256 mismatch for {archive}")
    executable = directory / ("ocean" + suffix)
    with tarfile.open(directory / archive) as source:
        member = source.getmember(project + suffix)
        if not member.isfile():
            raise RuntimeError(f"Unexpected compiler entry in {archive}")
        with source.extractfile(member) as stream, executable.open("wb") as target:
            shutil.copyfileobj(stream, target)
    executable.chmod(0o755)


def native_path(path):
    if os.environ.get("MSYSTEM"):
        return subprocess.check_output(["cygpath", "-m", str(path)], text=True).strip()
    return str(path)


def package(args):
    arch, system = args.target.split("-", 1)
    actual_system = {"Linux": "linux", "Darwin": "macos", "Windows": "windows"}[platform.system()]
    actual_arch = {"AMD64": "x86_64", "arm64": "aarch64", "ARM64": "aarch64"}.get(
        platform.machine(), platform.machine())
    if (arch, system) != (actual_arch, actual_system):
        raise RuntimeError(f"{args.target} requires a matching runner, got {actual_arch}-{actual_system}")
    suffix = ".exe" if system == "windows" else ""
    stdlib = Path("build/.nightly-stdlib").resolve()
    bootstrap_binary = Path("build/bootstrap") / ("ocean" + suffix)
    env = dict(os.environ, OCEAN_SRC=native_path(stdlib), OCEAN_BUILD_CACHE="off")
    if system == "macos":
        # Link the compiler against the SDK's libffi, which ships with macOS.
        env.update(PKG_CONFIG_LIBDIR="", PKG_CONFIG_PATH="")
    subprocess.run([str(bootstrap_binary.resolve()), "--version"], check=True)
    subprocess.run(["make", "release", f"VERSION={args.tag}", f"CROSS_TARGETS={args.target}",
                    f"OCEAN={native_path(bootstrap_binary.resolve())}",
                    "RELEASE_FLAGS=--static" if system == "windows" else "RELEASE_FLAGS="],
                   env=env, check=True)
    binary = Path("build/release") / f"ocean-{args.tag}-{system}-{arch}{suffix}"
    version = subprocess.check_output([str(binary.resolve()), "--version"], env=env, text=True)
    if f"Ocean compiler (ocean) {args.tag}\n" not in version:
        raise RuntimeError(f"Unexpected version from {binary}")
    subprocess.run([str(binary.resolve()), "test", "./...", "--notty"], env=env, check=True)

    output = Path("dist")
    output.mkdir(exist_ok=True)
    stem = f"ocean_{args.tag}_{args.target}"
    archives = [output / f"{stem}.tar"]
    with tarfile.open(archives[0], "w") as stream:
        stream.add(binary, arcname="ocean" + suffix)
    if system == "windows":
        archives.append(output / f"{stem}.zip")
        with zipfile.ZipFile(archives[-1], "w", zipfile.ZIP_DEFLATED) as stream:
            stream.write(binary, arcname="ocean.exe")
    bundle = build_bundle(output, args.tag, arch, system, compress=not args.defer_compression)
    if not args.defer_compression:
        write_checksums(output, args.target, [*archives, bundle])


def write_checksums(output, target, paths):
    text = "".join(f"{checksum(path)}  {path.name}\n" for path in paths)
    (output / f"SHA256SUMS_{target}.txt").write_text(text, encoding="utf-8")


def compress(args):
    output = Path("dist")
    bundle = output / f"ocean-deps_{args.tag}_{args.target}.tar"
    subprocess.run(["xz", "--threads=2", "--force", str(bundle)], check=True)
    write_checksums(output, args.target, [output / f"ocean_{args.tag}_{args.target}.tar",
                                         bundle.with_suffix(".tar.xz")])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(required=True)
    for name, handler in (("prepare", prepare), ("bootstrap", bootstrap), ("package", package), ("compress", compress)):
        command = commands.add_parser(name)
        command.add_argument("--tag", required=True)
        if name != "prepare":
            command.add_argument("--target", required=True)
        command.set_defaults(handler=handler)
        if name == "bootstrap":
            command.add_argument("--repo", required=True)
        if name == "package":
            command.add_argument("--defer-compression", action="store_true")
    args = parser.parse_args()
    args.handler(args)


if __name__ == "__main__":
    main()
