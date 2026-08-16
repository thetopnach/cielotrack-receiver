# Releasing

Receivers update themselves overnight from signed tags. A release is therefore not an
announcement — it is code that installs itself as root on other people's hardware while
they are asleep. This is what that costs to do safely.

## The trust model, briefly

Three things have to line up before a receiver will install a release:

1. the tag is signed by the release key,
2. that key matches the one pinned to `/etc/cielotrack/allowed_signers` on the receiver,
3. the receiver still works afterwards, or it puts the previous release back.

The pinned copy is the load-bearing part. `update.sh` verifies against it and never
against the `allowed_signers` file in the repository it is about to install — if it
trusted that one, anyone able to push a tag could also push the key vouching for it, and
the signature would prove nothing.

The release key does nothing else. It is not the key that authenticates to GitHub, and
it is not the key that reaches any server. Those are used constantly, from machines used
for other things; this one signs releases and sits still. It is also passphrase
protected, which means **a release cannot be cut unattended, by automation, or by an
agent** — only by someone who can type the passphrase. That is deliberate. It is the
last thing standing between a compromised laptop and root on every receiver.

## Cutting one

```bash
cd ~/cielotrack-receiver
git pull
```

Check CI is green before tagging, not after. A tag is the thing receivers act on, so a
red one is not a mistake you get to quietly amend:

```bash
gh run list --limit 3
```

Then tag. The message is read by whoever is deciding whether to trust the update, so say
what changed and, if anything, what it asks of them:

```bash
git tag -s v1.2.0 -m "What changed, in a sentence or two."
git push origin main --tags
```

`git tag -s` prompts for the passphrase. If it does not, something is wrong — check that
`user.signingkey` still points at `~/.ssh/cielotrack_release_ed25519` and that the key is
still encrypted:

```bash
ssh-keygen -y -P '' -f ~/.ssh/cielotrack_release_ed25519   # must FAIL
```

Finally, verify the tag the way a receiver will, against the file that gets pinned:

```bash
git tag --verify v1.2.0
```

Expect `Good "git" signature`. Anything else means receivers will refuse it, which is the
pin working — do not work around it, find out why.

## Numbering

The question is not how big the change is, it is what it asks of the operator.

| | |
|---|---|
| **patch** — `v1.1.1` | Nothing is asked. It installs overnight and nobody notices. |
| **minor** — `v1.2.0` | Something is asked, or something visible changes: a new fault code, a config file, a manual step for existing installs. |
| **major** — `v2.0.0` | Existing installs break without intervention. |

A release that existing receivers will *refuse* — a signing key rotation, for instance —
is at least a minor, and the README needs a section telling those operators what to do.
They will not find out any other way: their receiver simply keeps running the old
version and logs a verification failure nobody is watching.

## What happens next

Nothing, for a while. Each receiver picks a random time between 02:00 and 04:00 **its
own local time**, checks for a new tag, verifies it, installs it, and then checks itself
using the same faults the fleet page shows. If detections stop reaching the queue or the
status file cannot be written, it restores the previous release *and* the previous
service unit and restarts.

So a bad release does not take the fleet down at once. Early updaters roll themselves
back before later ones begin, and you have until roughly 02:00 in the westernmost
timezone you have a receiver in to notice and publish a fix.

Watch it land:

```bash
journalctl -u cielotrack-update -f      # on a receiver
```

The fleet page shows the version each receiver reports, which is how you tell a rollout
from a hope.

## Rotating the key

Only when you have to — a rotation strands every existing install until its operator
re-pins by hand, because their pinned copy is exactly what refuses the new key.

1. `ssh-keygen -t ed25519 -C "CieloTrack release signing" -f ~/.ssh/cielotrack_release_ed25519_new`
   with a passphrase.
2. Replace the key line in `allowed_signers`, keeping the principal the same.
3. Point `user.signingkey` at the new file.
4. Cut a **minor** release, and add a README section named for the version telling
   existing operators to re-pin:
   `git show vX.Y.0:allowed_signers | sudo tee /etc/cielotrack/allowed_signers`

`provision.sh` deliberately refuses to overwrite a pinned key that differs from the one
in the checkout. A pin that can silently replace itself is not a pin.

## If the key is lost

There is no recovery path that does not involve every operator. Receivers verify against
what they pinned, so a new key means every one of them refuses every future release until
someone with shell access re-pins it by hand.

Which is why the key is backed up somewhere that is not this SD card, and why the
passphrase is stored separately from it. If you are reading this because that did not
happen: rotate the key as above, and expect the fleet to sit on its current version until
each operator acts.
