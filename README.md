# Custom Plates

Plugin that puts real text on number plate attachments.

The plates are normal attachments from More Attachments, so you fit and move them with the game's own tools. The meshes come with their own artwork, so this plugin covers each one with a blank in your plate colour and adds the lettering on both faces.

Everything is set per car in Tuning, under License Plate: a template, up to nine characters, text and plate colours, a border, a band, and the text's size, height and position. It saves to `Binaries\Win64\plugins\custom_plates.ini`. Only you see your plate for now.

## Install

Needs [Plugin Loader](https://github.com/qxZap/mt-plugin-loader) (`version.dll`) in
`MotorTown\Binaries\Win64\`, and More Attachments for the plates. Put `mt_plugin_CustomPlates.dll` in `MotorTown\Binaries\Win64\plugins\`.

## Build

`build.bat` (MSVC 2022, x64) writes `out\mt_plugin_CustomPlates.dll`.
