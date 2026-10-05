# Vita JP Overlay

Overlay for japanese learners. Text recognition uses Google Lens, and the definitions come from
[jiten.moe](https://jiten.moe) or [jpdb.io](https://jpdb.io).

The idea comes from [meikidroid](https://github.com/rtr46/meikidroid)

![The overlay over a visual novel: the recognized text with the selected word highlighted, and its jiten.moe entry below](docs/screenshot.jpg)

## Requirements

- A PS Vita on firmware 3.60 to 3.74 with HENkaku Ensō or h-encore.
- Wi-Fi.
- An API key from jiten.moe or jpdb.io.

## Installing

### From the release zip

1. Download `VitaJPOverlay-<version>.zip` from the [Releases](../../releases) page.
2. Copy the zip's `ur0` folder to the root of `ur0:` on the Vita. VitaShell's FTP server or
  USB mode both work. You should end up with:
  - `ur0:tai/VitaJPOverlay_Kernel.skprx`
  - `ur0:tai/VitaJPOverlay_Shell.suprx`
  - `ur0:data/VitaJPOverlay/vitajpoverlay.rco`
3. Open `ur0:tai/config.txt` (or `ux0:tai/config.txt`, if that is the one your taiHEN uses) and
  add two lines:
  - `ur0:tai/VitaJPOverlay_Kernel.skprx` on a new line under `*KERNEL`
  - `ur0:tai/VitaJPOverlay_Shell.suprx` on a new line under `*main`
4. Copy the zip's `config.ini` to `ux0:data/VitaJPOverlay/config.ini` and set your API key: `jiten_api_key`, or
  `dictionary = jpdb` and `jpdb_api_key`. Change any other settings you like. The file is read
   each time the overlay opens, so later changes need no reboot.
5. Reboot.

To update, copy the new files over the old ones and power-cycle the Vita. Your `config.ini`
stays as it is.

### From a computer, over FTP

If you have the source and a built copy of the plugins, `tools/install_ftp.sh` does the
steps above in one go. It needs bash, curl and python3 (macOS or Linux). Start VitaShell's FTP
server (press Select in VitaShell) and run:

```bash
tools/install_ftp.sh --set dictionary=jiten --set jiten_api_key=your-key <vita-ip>
```

It backs up and patches the taiHEN config files, uploads the plugins and writes `config.ini`.
Running it again later keeps your settings. `--uninstall` removes the plugin lines and
`--status` prints the plugin logs.

## Using it

In a game:


| Input      | Action                                                                                                        |
| ---------- | ------------------------------------------------------------------------------------------------------------- |
| L + R      | Open or close the overlay (`toggle_button`).                                                                  |
| Select + R | Subtitles on or off (`subtitle_button`): the recognized text in a strip at the top, updated as the game runs. |


In the overlay:


| Button           | Action                                                                                                                                       |
| ---------------- | -------------------------------------------------------------------------------------------------------------------------------------------- |
| ◀ ▶              | Previous or next word. The word is highlighted in the text and its definition is shown below.                                                |
| ▲ ▼              | The word on the line above or below.                                                                                                         |
| Either stick     | Scroll a long definition.                                                                                                                    |
| ×                | Add the word to Anki (when set up, see [Anki](#anki)). A green √ marks words already in your deck.                                           |
| □                | Choose the area to read in this game. Drag a box on the touchscreen, then press × to keep it or ○ to cancel. Holding □ sets the full screen. |
| ○, or the toggle | Close the overlay.                                                                                                                           |




## Settings

All settings live in `ux0:data/VitaJPOverlay/config.ini`. The area to read (□ in the overlay) is saved per game in
`ux0:data/VitaJPOverlay/region.ini`.


| Key                   | Values                                                              | Default    | What it does                                                                                 |
| --------------------- | ------------------------------------------------------------------- | ---------- | -------------------------------------------------------------------------------------------- |
| `dictionary`          | `jiten`, `jpdb`                                                     | `jiten`    | Which dictionary looks up the words.                                                         |
| `jiten_api_key`       | text                                                                | empty      | Your jiten.moe API key.                                                                      |
| `jpdb_api_key`        | text                                                                | empty      | Your jpdb.io API key.                                                                        |
| `non_japanese_filter` | `lines`, `none`                                                     | `lines`    | Drop recognized lines with no kana or kanji.                                                 |
| `font_size_ja`        | 8 to 40                                                             | 18         | Size of the Japanese text: the sentence, headwords and readings.                             |
| `font_size_en`        | 8 to 40                                                             | 14         | Size of the English text: meanings and messages.                                             |
| `toggle_button`       | `l+r`, `select`, `start`, `select+l`, `select+r`, `rear_double_tap` | `l+r`      | What opens and closes the overlay. Buttons are hidden from the game; rear taps are not.      |
| `subtitle_button`     | same as `toggle_button`                                             | `select+r` | What turns subtitles on and off.                                                             |
| `ocr_mode`            | `auto`, `on_press`                                                  | `auto`     | Recognize in the background, or only when the overlay opens.                                 |
| `log_host`            | IPv4 address                                                        | empty      | Send debug logs to `tools/udp_log_listener.py` on that computer.                             |
| `log_file`            | `on`, `off`                                                         | `off`      | Write a debug log to `ux0:data/VitaJPOverlay/log.txt` (256 KB at most, plus one older file). |




## Anki

× adds selected word to Anki, along with the reading, furigana, definitions, the sentence, a screenshot of the game and the frequency rank. Words already in your deck show a green √.

1. In Anki, install [AnkiConnect](https://ankiweb.net/shared/info/2055492159).
2. Tools → Add-ons → AnkiConnect → Config: set `"webBindAddress": "0.0.0.0"`, then restart Anki.
3. In `config.ini`, set `anki_host = auto` (the Vita searches your network once and remembers the computer) or your computer's IP. Set `anki_deck` to your deck; it's created if missing.
4. The default note type is [Lapis](https://github.com/donkuri/lapis). For another note type, set `anki_note_type` and the `anki_field_*` keys to its field names; leave a key empty to skip that data. Duplicates are checked on the note type's first field, so map the word to that field.


| Key                     | Default              | What it does                                                                         |
| ----------------------- | -------------------- | ------------------------------------------------------------------------------------ |
| `anki_host`             | empty                | The computer running Anki: empty = off, `auto` = search the network, or `IP[:port]`. |
| `anki_deck`             | `Default`            | Deck for new cards.                                                                  |
| `anki_note_type`        | `Lapis`              | Note type for new cards.                                                             |
| `anki_tags`             | `vita-jp-overlay`    | Tags for new cards, separated by spaces.                                             |
| `anki_field_word`       | `Expression`         | Field for the word.                                                                  |
| `anki_field_reading`    | `ExpressionReading`  | Field for the reading in kana.                                                       |
| `anki_field_furigana`   | `ExpressionFurigana` | Field for the word with its reading, `言葉[ことば]`.                                      |
| `anki_field_definition` | `MainDefinition`     | Field for the definitions, as a numbered list.                                       |
| `anki_field_sentence`   | `Sentence`           | Field for the recognized text, with the word in bold.                                |
| `anki_field_picture`    | `Picture`            | Field for the screenshot of the game.                                                |
| `anki_field_frequency`  | `FreqSort`           | Field for the frequency rank.                                                        |




## Privacy

- The chosen area of the screen is sent to Google Lens for text recognition. In `auto` mode,
and while subtitles are on, that happens every time the area changes while a game is running.
- The recognized text is sent to jiten.moe (`api.jiten.moe`) or jpdb.io, along with your API
key.
- With Anki set up, the cards (including the screenshot) go to Anki on your computer over your
local network. `anki_host = auto` looks for it on port 8765 of the other devices on your
network.
- Nothing else leaves the console. Logs go only where `log_host` and `log_file` send them.



## Troubleshooting

The plugins write two logs to `ux0:data/VitaJPOverlay/`: `kernel.txt` from the kernel plugin
and `status.txt` from the SceShell plugin. Both start over at each boot (the previous boot's
copies are kept as `*.prev.txt`) and stop at 64 KB. `tools/install_ftp.sh --status <vita-ip>`
prints all of them.

Some games turn Wi-Fi off to save power. For those, install
[NoPowerLimits](https://github.com/Electry/NoPowerLimitsVita) alongside this plugin
(`tools/install_ftp.sh --add-kernel-plugin NoPowerLimits.skprx <vita-ip>` does it for you).

## Building

Set up the toolchain once (vitasdk, taiHEN, the ScePaf headers and stubs, and psp2cxml-tool):

```bash
tools/setup_toolchain.sh
```

Build and run the tests on your computer:

```bash
cmake -S . -B build/host -G Ninja && cmake --build build/host && ./build/host/vjo-tests
```

Build the plugins and the release zip (`build/vita/VitaJPOverlay-<version>.zip`):

```bash
cmake -S vita -B build/vita && cmake --build build/vita --target release
```

`vjo-cli` runs the same pipeline on a screenshot:

```bash
VJO_JITEN_KEY=your-key ./build/host/vjo-cli --dict jiten screenshot.jpg --nav
```



## Credits

- [meikidroid](https://github.com/rtr46/meikidroid) by rtr46 for the idea.
- [jmdict-vita](https://github.com/shoui520/jmdict-vita) by shoui520 for rich-text layout.
- [PSVshellPlus](https://github.com/GrapheneCt/PSVshellPlus) by GrapheneCt for the
SceShell/ScePaf plugin pattern, and [PSVshell](https://github.com/Electry/PSVshell) by
Electry and [reVita](https://github.com/MERLev/reVita) by MERLev for the hook patterns.
- [ScePaf-RE](https://github.com/GrapheneCt/ScePaf-RE),
[vitasdk-paf-component](https://github.com/Princess-of-Sleeping/vitasdk-paf-component) and
[psp2cxml-tool](https://github.com/Princess-of-Sleeping/psp2cxml-tool), which make ScePaf
usable from vitasdk.
- [BearSSL](https://bearssl.org), [jsmn](https://github.com/zserge/jsmn) and
[acutest](https://github.com/mity/acutest)



## License

GPL-3.0. See [LICENSE](LICENSE).