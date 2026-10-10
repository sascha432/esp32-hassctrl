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
