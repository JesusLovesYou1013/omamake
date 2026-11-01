# Known issues

Problems seen while building OmaMake that come from the system around it, kept here so they stay on record in every copy of the project. When one is solved, its resolution is added under it, whether an update fixed it or we did. Solved issues stay listed.

## Notification toast blanks the whole screen

Status: worked around on this machine with the setting below, waiting on an Omarchy fix, first seen 2026-10-30.

Problem: while an Omarchy notification toast shows, the whole screen goes black except the toast: every window, the wallpaper and the top bar. It comes back when the toast goes.

Not OmaMake: it happens with OmaMake closed. Turning Hyprland's blur off and making the toast solid both leave it black.

Suspected, not proven: a system update. It started in the first session on these versions:

| Package | Before | After |
|---|---|---|
| hyprland | 0.56.2-4 | 0.57.0-1 |
| hyprutils | 0.14.2-1 | 0.14.3-1 |
| mesa, vulkan drivers | 26.2.5-1 | 26.2.6-1 |
| qt6-declarative | 6.12.0-1 | 6.12.0-3 |

Reproduce: `notify-send -t 3000 "Blank test" "one"`.

Cause, found 2026-10-31: Hyprland 0.57 turned `quirks:skip_non_kms_dmabuf_formats` on by default, and on this VM's graphics that leaves apps with a single buffer format, XR24, which has no alpha. The toast is a full-screen transparent window on the overlay layer, so without alpha it paints black over everything. Reported at [omacom/omarchy#17061](https://github.com/omacom/omarchy/issues/17061).

Seen here: `quirks:skip_non_kms_dmabuf_formats` reads true, OmaMake's graphics format table has one entry, and all 5 of its buffers are XR24.

Resolution: tested and working 2026-10-31 with `hl.config({ quirks = { skip_non_kms_dmabuf_formats = false } })`, then `hyprctl reload` and `omarchy restart shell`. `hyprctl eval` alone doesn't work, because the format list is only rebuilt on a reload. Apps started before the change stay without alpha until restarted.

Side effect seen: the wallpaper was missing after the shell restart. Picking a new one in OmaSkins brought it back, after several tries. Why it went missing is not known.

## Window background is black instead of see-through

Status: worked around on this machine with the setting below, waiting on an Omarchy fix, first seen 2026-10-30.

Problem: OmaMake's model space shows black where the wallpaper should show through. It worked at 07:38 on 2026-10-30, before the session restarted onto Hyprland 0.57.

Not OmaMake or its engine: the app still draws the same see-through pixels on both engines (143 of 255 unfocused), but they reach Hyprland in a buffer with no alpha.

Cause: the same as the toast above, and the same resolution cleared both.
