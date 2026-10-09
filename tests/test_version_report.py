"""Tests for the version a receiver reports in its heartbeat.

The bug this guards against is quiet and purely cosmetic, which is exactly why it is
easy to leave in. When a final release and its release candidate point at the same
commit — the final cut on the identical commit as the rc, no code changed between them
— `git describe --tags` resolves the ambiguity to the *prerelease* name. A receiver
fully up to date on the final build then reports e.g. `v1.5.1-rc1` on the admin fleet
page and reads as a unit stranded on a prerelease. It is not: it is running the final
code. So the reporter prefers a final tag over a prerelease when several tags share the
current commit, while still reporting a lone prerelease (a genuine canary) and a bare
hash when the commit carries no tag at all.

This builds throwaway git repositories rather than touching the receiver's own, so it
can put two tags on one commit without inventing a release here. It never changes which
release the updater installs — update.sh has its own ordering, tested separately.

Run directly — no test framework required:

    python3 tests/test_version_report.py
"""
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
os.environ.setdefault("CENTRAL_SERVER_URL", "http://127.0.0.1:9")

import radio_tracker as rt


def check(name, condition, detail=""):
    print(f"  {'PASS' if condition else 'FAIL'}  {name}{'  — ' + detail if detail else ''}")
    return bool(condition)


def git(repo, *args, check_rc=True):
    """Run git in `repo`, with a committer identity that does not depend on the ambient
    config a clean CI runner may not have."""
    result = subprocess.run(
        ["git",
         "-c", "user.name=test", "-c", "user.email=test@example.com",
         "-c", "commit.gpgsign=false", "-c", "tag.gpgsign=false",
         "-C", repo, *args],
        capture_output=True, text=True)
    if check_rc and result.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout.strip()


def new_repo(directory, tags=()):
    """A one-commit repository carrying `tags`, all on that single commit."""
    os.makedirs(directory, exist_ok=True)
    git(directory, "init", "-q", "-b", "main")
    with open(os.path.join(directory, "radio_tracker.py"), "w") as handle:
        handle.write("# a stand-in for the checked-out code\n")
    git(directory, "add", "radio_tracker.py")
    git(directory, "commit", "-q", "-m", "release commit")
    for tag in tags:
        git(directory, "tag", tag)
    return directory


def test_a_final_tag_wins_over_a_prerelease_on_the_same_commit():
    """The whole point: v1.5.1 and v1.5.1-rc1 on one commit must report v1.5.1, not the
    rc that git describe would otherwise pick."""
    print("\na final tag wins over a prerelease sharing the commit")
    with tempfile.TemporaryDirectory() as tmp:
        # Tagged rc first, final second, which is the real order a release is cut in —
        # and the order in which git describe prefers the rc.
        repo = new_repo(os.path.join(tmp, "tie"), tags=["v1.5.1-rc1", "v1.5.1"])
        ok = check("the final is reported, not the rc",
                   rt.installed_version(repo) == "v1.5.1",
                   rt.installed_version(repo))

        beta = new_repo(os.path.join(tmp, "beta"), tags=["v2.0.0", "v2.0.0-beta"])
        ok &= check("and -beta is a prerelease too, so the final still wins",
                    rt.installed_version(beta) == "v2.0.0",
                    rt.installed_version(beta))

        # Several finals on one commit (a re-tag, or a rename that kept the old tag):
        # report the highest, by the same version sort update.sh uses.
        many = new_repo(os.path.join(tmp, "many"),
                        tags=["v1.5.0", "v1.5.1-rc2", "v1.5.1"])
        ok &= check("with two finals present, the highest is reported",
                    rt.installed_version(many) == "v1.5.1",
                    rt.installed_version(many))
        return ok


def test_a_lone_prerelease_is_still_reported():
    """A canary ahead of any final release genuinely is on a prerelease, and hiding that
    would be the opposite mistake — it would read as up to date when it is not."""
    print("\na commit with only a prerelease tag still reports the prerelease")
    with tempfile.TemporaryDirectory() as tmp:
        repo = new_repo(os.path.join(tmp, "canary"), tags=["v1.6.0-rc1"])
        ok = check("the prerelease is reported unchanged",
                   rt.installed_version(repo) == "v1.6.0-rc1",
                   rt.installed_version(repo))
        return ok


def test_an_untagged_commit_reports_its_hash():
    """No tag at all: the fallback is the abbreviated commit hash, exactly as before."""
    print("\nan untagged commit falls back to the short hash")
    with tempfile.TemporaryDirectory() as tmp:
        repo = new_repo(os.path.join(tmp, "bare"))
        reported = rt.installed_version(repo)
        full = git(repo, "rev-parse", "HEAD")
        ok = check("it is a hex abbreviation of the current commit",
                   reported and full.startswith(reported) and 7 <= len(reported) < 40
                   and all(c in "0123456789abcdef" for c in reported),
                   reported)
        ok &= check("and it is not a tag name", not reported.startswith("v"))
        return ok


def test_a_dirty_tree_keeps_the_dirty_marker():
    """git describe --dirty flags a modified checkout; preferring the final tag must not
    silently drop that signal."""
    print("\na dirty working tree still reports -dirty on the chosen tag")
    with tempfile.TemporaryDirectory() as tmp:
        repo = new_repo(os.path.join(tmp, "dirty"), tags=["v1.5.1-rc1", "v1.5.1"])
        with open(os.path.join(repo, "radio_tracker.py"), "a") as handle:
            handle.write("# local edit\n")
        ok = check("the final tag is still chosen, with -dirty appended",
                   rt.installed_version(repo) == "v1.5.1-dirty",
                   rt.installed_version(repo))
        return ok


def test_a_checkout_with_no_git_reports_unknown():
    """A tarball install with no .git is unusual but must not crash the receiver."""
    print("\na directory that is not a git checkout reports 'unknown'")
    with tempfile.TemporaryDirectory() as tmp:
        ok = check("it falls back to 'unknown' rather than raising",
                   rt.installed_version(tmp) == "unknown",
                   rt.installed_version(tmp))
        return ok


TESTS = [
    test_a_final_tag_wins_over_a_prerelease_on_the_same_commit,
    test_a_lone_prerelease_is_still_reported,
    test_an_untagged_commit_reports_its_hash,
    test_a_dirty_tree_keeps_the_dirty_marker,
    test_a_checkout_with_no_git_reports_unknown,
]


if __name__ == "__main__":
    if len(sys.argv) > 1:
        wanted = {name.lstrip("-").replace("-", "_") for name in sys.argv[1:]}
        chosen = [t for t in TESTS if t.__name__ in wanted or
                  t.__name__.removeprefix("test_") in wanted]
        if not chosen:
            print(f"no test matches {sorted(wanted)}; known tests:")
            for t in TESTS:
                print(f"  {t.__name__.removeprefix('test_')}")
            sys.exit(2)
    else:
        chosen = TESTS

    results = [t() for t in chosen]
    print(f"\n{sum(results)}/{len(results)} passed")
    sys.exit(0 if all(results) else 1)
