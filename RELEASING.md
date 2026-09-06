# Releasing HardwareScope

HardwareScope releases are built by GitHub Actions from the tagged source. The
stable update manifest is changed only after GitHub has published and verified
the release assets.

## Release process

1. Update the version in `CMakeLists.txt` and add `RELEASE_NOTES_<version>.md`.
2. Merge the tested changes into `main`.
3. Create and push the matching tag, for example `v2.0.0`.
4. The **Build and publish release** workflow builds and tests the Windows x64
   application, installer, portable ZIP, and checksums.
5. The workflow uploads those files to a **draft candidate**, not an advertised
   stable release. Download and qualify those exact bytes. Do not rebuild between
   qualification and promotion.
6. Record the candidate source commit, installer SHA-256, machine/Windows/driver
   details and evidence for deterministic tests, production UI, a real older-version
   upgrade, installer failure recovery, FPS, reference hardware, and a five-minute
   resource check. Run `tests/validate_release_evidence.ps1` against that record
   and installer. Review the referenced evidence; the validator checks the record,
   not the truth of a manually entered result. Only then may the maintainer promote
   the same draft assets to a public stable release. Keep the portable ZIP and its
   checksums in the same reviewed candidate.
7. The **Publish verified update manifest** workflow downloads the public
   installer, verifies its name, size, URL, and SHA-256 checksum, then commits
   the new `updates/latest.json` to `main`.

Promotion is a maintainer gate, not a claim that GitHub branch protection is
already configured. Required reviews/status checks and release permissions must
be configured by the repository owner before broad distribution. An older or
duplicate release event cannot regress or rewrite the stable manifest. A failed
push caused by concurrent main changes must be rerun against fresh main.

`clean_install_validation.ps1` without `-PreviousInstallerPath` verifies clean
installation and same-version reinstall only. Supply a strictly older native
installer for the historical-upgrade test. These destructive tests are restricted
to disposable hosted Windows runners; never run them on the developer's installed PC.

For a failed release, retain the known-good manifest and prefer a forward repair
version. Do not replace existing public asset bytes or lower the normal stable
version to perform an undocumented rollback.

Never point `updates/latest.json` at a draft, local file, or asset that has not
been downloaded and verified from the public GitHub release.

## Signing gate

The current pipeline publishes unsigned releases until HardwareScope has been
accepted by SignPath Foundation and the repository has been connected to its
SignPath project. Do not add placeholder IDs or tokens to `release.yml`.

After acceptance, the release pipeline must submit only GitHub-hosted workflow
artifacts built from the release tag. Signing requests require manual approval.
The HardwareScope application, updater, sensor service, and final installer are
eligible project artifacts. The upstream `PresentMon.exe` binary must remain
outside HardwareScope's signing scope.

After signing is enabled, verify each published executable with
`Get-AuthenticodeSignature` and fail the release if any expected signature is
missing or invalid. Update the signing-status wording in `README.md` only after
that enforcement is active.

If either workflow fails, leave the previous stable manifest unchanged, correct
the source or automation, and rerun the failed workflow.

## Repository-owner rename compatibility

The GitHub owner changed from `Aydren1` to `Cero-SC` after version 2.0.4.
HardwareScope 2.0.4 and earlier accept only installer URLs under the former
owner name. GitHub currently redirects that release path to the current
repository, so `publish-update-manifest.yml` intentionally keeps the legacy
installer URL while verifying and publishing assets from `Cero-SC/HardwareScope`.

New builds fetch the manifest from the current repository and trust release
paths under either owner. Do not remove the legacy URL from the manifest or
validator without an explicit migration plan for installations that have not
yet upgraded past 2.0.4.
