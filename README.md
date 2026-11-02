# Custom Plates

Plugin that puts real text on number plate attachments.

The plates are normal attachments from More Attachments, so you fit and move them with the game's own tools. The meshes come with their own artwork, so this plugin covers each one with a blank in your plate colour and adds the lettering on both faces.

Early prototype: one text for every plate, set in `Binaries\Win64\plugins\custom_plates.ini` under `[Plate]` (`Text`, `Size`, `X`, `Z`, `Depth`). Per-car text, letter rows in Tuning and plate templates are planned. Only you see your text.

## Install

Needs [Plugin Loader](https://github.com/qxZap/mt-plugin-loader) (`version.dll`) in
`MotorTown\Binaries\Win64\`, and More Attachments for the plates. Put `mt_plugin_CustomPlates.dll` in `MotorTown\Binaries\Win64\plugins\`.

## Build

`build.bat` (MSVC 2022, x64) writes `out\mt_plugin_CustomPlates.dll`.
