# NHL Scoreboard USB Flasher

This Windows WinForms utility finds the serial ports known to PlatformIO,
flashes an NHL Scoreboard release image, and shows the complete esptool log.
The progress bar updates from esptool's live write percentage.

## Requirements

- Windows 10 or later
- PlatformIO Core installed for the current Windows user at
  `%USERPROFILE%\.platformio`
- A USB data cable and the ESP32-S3 serial-driver support used by the board

## Build and run

Build a single-file Windows executable from this directory with the .NET 8 SDK:

```powershell
dotnet publish -c Release
```

Run `bin\Release\net8.0-windows\win-x64\publish\NhlScoreboardFlasher.exe`.
Choose a release image from `releases/`, select the ESP32-S3 serial port, and
click **Flash**. The port list refreshes automatically while the app is open.

Use `nhl_scoreboard_<version>_factory.bin` for a new board or to restore a
board completely. It includes the bootloader and partition table and is
written from flash offset `0x0`. The regular
`nhl_scoreboard_<version>.bin` application image is written at `0x10000` and
requires an existing compatible NHL Scoreboard partition layout.
