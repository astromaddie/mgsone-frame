# Public Source Checks

I publish this fork as source. Retail discs, extracted assets, translated game
code and private device packages stay outside Git.

Before publishing a branch, check the history that will actually be pushed:

```sh
git status --short
git diff --check
git log --format='%h %an <%ae> %s' upstream/main..HEAD
git rev-list --objects HEAD | git cat-file \
  --batch-check='%(objectname) %(objecttype) %(objectsize) %(rest)'
gitleaks git . --log-opts=HEAD --redact \
  --report-format=json --report-path=/path/to/task-scratch/gitleaks.json
```

The publication scan uses Gitleaks 8.30.1 with its default rules. Review any
findings without publishing the report or printing credentials. A pass has no
unresolved secrets, retail data or generated game translations in reachable
history. An ignored file in the working directory isn't proof that an older
commit doesn't contain it.

Inspect every reachable blob for executable and archive signatures as well
as names. Original PlayStation executables start with `PS-X EXE`; raw disc
sectors have the CD sync marker. Also check for private keys, disc formats,
asset caches, memory cards and savestates. Any binary or media needs a known
source. This tree inherits ELFIO's executable test fixtures and documentation
images, plus the upstream project's header image. Compare their blob IDs with
`upstream/main` before treating them as unchanged dependencies.

For the README, check its relative links, command paths and controller table
against the launcher and OpenXR bindings. Keep Frame downloads separate from
upstream desktop releases. Publishing this source doesn't publish a playable
binary or the private packages produced from retail inputs.

The [third-party notices](../packaging/THIRD_PARTY_NOTICES.md) describe the
inherited licensing limits. Don't replace those notices with a blanket engine
licence claim.
