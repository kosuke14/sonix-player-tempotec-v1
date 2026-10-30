# Custom SD-card theme

Copy [`sonix-player/examples/custom-theme.ini`](../sonix-player/examples/custom-theme.ini)
onto the SD card as **`Themes/custom.ini`** (create `Themes` at the card root).
Edit it with a text editor, safely eject the card, then open **Settings →
Appearance → Custom**. To see subsequent edits, return to Appearance (or tap
Custom again). The choice persists across restarts as long as the card and
file are available at startup; if not, the player starts in Dark. Selecting
Custom with a missing or invalid file shows an error and keeps the current
appearance. Editing a selected Custom theme while the player is running takes
effect when Appearance is reopened; invalid edits leave the last valid palette
in memory.

Every key in `[theme]` is required, each with a six-digit `#RRGGBB` value.
Lines starting with `;` or `#` are comments. `screen_bg` is the page,
`surface` the cards and rows, `surface_pressed` their pressed state, `panel`
the status bar and player controls, `cover_bg` the empty cover, `text_primary`
and `text_secondary` the foreground, and `accent` the highlight. Custom uses
the standard dark layout and one accent for functional icons. Some player
chrome and semantic highlights use built-in colours; this is a palette theme,
not an icon/font/layout replacement. Dynamic tint, if enabled in Appearance,
can alter the background and surface colours further. Accent presets override
the file’s `accent` when a non-default preset is selected; select the first
accent circle to use the file’s own colour.
