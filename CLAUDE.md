# Instructions for Claude Code

- **ESP-IDF first.** `tools/build.py`, `tools/build_fs.py` and the other tools fail with "IDF_PATH is
  not set" in a plain shell. Run them in PowerShell after the activation script, in the same command:

  ```powershell
  . C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1 | Out-Null; python tools/build_fs.py wt32_sc01 -p COM9
  ```

  The shell state does not persist between tool calls, so every call starts with the `.` line.
- Boards and ports: see README.md ("Flash") and docs/TODO.md. Flash the board under test after a change.
- **Icon list changed** (`components/core/src/icons.cpp`, `UI_GLYPHS` in `tools/build_icons.py`): run `python tools/build_icons.py`
  (fonts of ui and `web/icons.svg`), commit the generated files with the change, flash firmware and web image.
- **Feature list with every change.** `README.md` starts with the list of features. A feature that
  is new, changed or gone changes the list in the same commit; keep it short, one line per point.
- **Docs with every change.** Update the affected `docs/*.md` (architecture, adding-a-board, CHANGELOG, the
  pages under `docs/ui` and `docs/devices`) in the same commit as the code. Check `docs/TODO.md`: mark items
  done or changed, add new open points, update the state lines.
- **Changelog with every change.** `docs/CHANGELOG.md`: an entry under today's date (newest
  first, inside the unreleased version), under Added, Changed, Fixed or Removed, saying what a
  user or developer sees. A change that reverses an entry of the same day replaces it instead of
  adding a second one.
- **NVS writes follow the policy** in docs/architecture.md section 9.3: values changed by live
  controls or by using the device go through `platform::set_blob_later` (10 s delay), explicit
  saves and secrets are written at once, nothing writes per frame or per touch.
- **Skip `docs/.retracted/`.** Don't read, search, update or cite anything in it; leave it out of
  searches over `docs/`.
- **Sample config with every change.** `docs/dashboard-sample/` documents the file format and where
  every setting, password and token goes. Update it whenever the format, a key, or the place a
  setting lives changes (`tools/host_test.py` imports it).
