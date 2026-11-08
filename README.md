# Cardputer Roast

**English** | [中文](README_zh.md)

Cardputer Roast turns an **M5Stack Cardputer ADV** into a pocket controller for the **E-Roast / E-Meter621 BLE**
fluid-bed coffee roaster (the machine normally driven by the *myERoast* phone app). It talks to the roaster over
Bluetooth LE, follows a roast curve with the roaster's own PID, and walks you through a roast on the 240×135
screen: charge, drying, first crack, development, drop, cooling, and a summary that is saved to the SD card and
opens in Artisan. It runs as an app under the community [Launcher](https://github.com/bmorcelli/Launcher).

<p>
<img src="docs/screenshots/prep.png" width="49%" alt="Prep page">
<img src="docs/screenshots/roast.png" width="49%" alt="Roasting">
</p>

> **Safety.** This firmware switches a heater on and off. It is not affiliated with or endorsed by the roaster's
> maker, and it was written from observed behaviour, not from vendor documentation. Never leave a roast
> unattended, and keep the machine's own controls within reach.

## Features
- **Curve following.** The roaster's PID follows a target curve, computed exactly as the phone app does
  (built-in samples, your own curves, or Artisan/myERoast `.alog` files). No curve = heat by hand.
- **Marks and development window.** Y / F / G mark the end of drying, first crack and its end. A phase bar runs
  through the whole roast, and after first crack it outlines the drop window (time and temperature rise).
- **Alerts and safety nets.** Colour bars for the drop window, drop-now temperature, development limit, heater
  pinned or falling after first crack, low RoR and BLE loss. Automatic drop only at the safety temperature or long
  after the curve has ended.
- **Fan control with floors.** Fan ±1 % or ±0.1 %; going below 50 % and 40 % needs a deliberate long press.
- **Manual heat.** P switches between PID and manual heat at any time; ← → step the heater.
- **Curve editor.** The phone app's eight nodes (Start, Dry, FC start/end, SC start/end, Drop, Total), edited on
  the device with a live curve beside the table, saved to the SD card.
- **Logs.** Every roast is written as `.csv` (1 Hz), `.json` summary and `.alog` for Artisan.
- **Simulator.** A built-in thermal model for practising without a machine; a yellow SIM tag shows when it is on.
- **Colour-keyed UI.** Colour = what it controls: fan blue, heat orange, curve/PID violet, marks yellow/brown, drop
  and alarms red, "go" green. The bottom row always shows the keys that work right now.

## Screenshots
Rendered from the firmware's own drawing code by the desktop preview (`tools/preview`), 2× scale.

| | |
|---|---|
| ![Connect](docs/screenshots/connect.png) Connect: scans for `E-Meter…BLE` | ![Prep](docs/screenshots/prep.png) Prep: curve → batch → fan → charge |
| ![Curves](docs/screenshots/curves.png) Curve library | ![Editor](docs/screenshots/editor.png) Curve editor: nodes and the curve side by side |
| ![Roasting](docs/screenshots/roast.png) Roasting: Maillard, first crack expected | ![After FC](docs/screenshots/roast-fc.png) After first crack: development window on the phase bar |
| ![Drop now](docs/screenshots/drop-now.png) Drop-now temperature reached | ![Chart](docs/screenshots/chart.png) Chart: bean, target, RoR, heater |
| ![Cooling](docs/screenshots/cooling.png) Cooling: progress to the cold temperature | ![Summary](docs/screenshots/summary.png) Summary, saved to SD |
| ![Rules](docs/screenshots/rules.png) Rules and alerts | ![Keys](docs/screenshots/keys.png) Key map, grouped by colour |

## Install (with Launcher)
1. **Install Launcher** on the Cardputer ADV, with [M5Burner](https://docs.m5stack.com/en/download) or the
   [Launcher web flasher](https://bmorcelli.github.io/Launcher/).
2. **Download `cardputer-roast-<version>.bin`** from the [Releases](../../releases) page (every CI run also keeps
   it as an artifact). It is the bare app image that Launcher installs, not a full-flash image.
3. **Install it from Launcher**, either way:
   - **SD card:** copy the `.bin` to the SD card, open Launcher's SD browser, select the file and install it.
   - **USB serial:** with Launcher in its menu, run
     ```sh
     pip install pyserial
     python3 tools/launcher_flash.py Roast cardputer-roast-<version>.bin
     ```
4. **Optional:** turn on Launcher's boot-to-app setting (CFG) to start Roast at power-on. Esc Esc in Roast then
   shows Launcher's boot screen for about 5 s and reopens Roast.

Insert a FAT32 SD card for curves and logs; without one, the last roast stays in memory until you leave the
summary.

## Using it
Turn the *myERoast* app off first: the roaster accepts only one BLE connection at a time.

Roast scans for a device named `E-Meter…BLE` and connects on its own. To try it without a machine, press **D** on
the connect screen, or switch *simulator* on in Rules (**O**). The simulator can only be switched while the
machine is not running.

**H** shows the key map on every page, grouped by colour. **M** mutes (alarms still sound). The top-right corner
shows the battery, the link (green dot) or the SIM tag, a speaker when muted, and a red **SD** tag without a card.

### Prep
The page walks through four steps: curve → batch → fan → charge.

| Key | Action |
|---|---|
| C | Curve library. "No curve" (heat by hand) is the first entry; Enter uses, E edits, N starts a new curve. |
| E | Edit the selected curve (or start a new one). |
| W / B | Batch sheet: green weight and bean name in one place (Tab switches field). |
| O | Rules, alerts and simulator. |
| S | Fan on, no heat. Use it to find a fan setting with cold air before adding beans. |
| ↑ ↓ (`;` `.`) | Fan ±1 % (Aa held: ±0.1 %). While idle this changes only the start-fan preset. |
| Enter | Charge: starts the timer and the roaster's PID, which follows the curve. Needs the fan on. |
| X X | Stop the fan. |
| Tab | Curve preview. |
| Esc Esc | Back to Launcher. Blocked while the fan is running. |

### Roasting
| Key | Action |
|---|---|
| Y / F / G | Mark drying end / first crack / end of first crack. Press the same key twice more to clear the mark. |
| D D or Space Space | Drop. After the first press the bottom row asks for the second. |
| ↑ ↓ | Fan ±1 % (Aa: ±0.1 %). |
| P | Manual heat: PID off, heater held at its current output. ← → change it by ±5 % (Aa: ±1 %; holding repeats, at most 8 steps per hold). P again returns to the curve. |
| Tab | Numbers / chart (bean black, target violet, RoR grey, heater orange area). |

The phase bar runs through the whole roast: drying (sage) → Maillard (yellow, starts at Y) → development (brown,
starts at F). After first crack it outlines the drop window. Exit is locked until the drop.

### Cooling and summary
- Cooling shows the fan, the cooling rate and how far the beans are from the cold temperature.
- **X X** stops the fan and opens the summary. **Enter** finishes the log and leaves the fan running; the summary
  then says "fan still on" and **X X** still stops it there (and on Prep afterwards).
- **W** sets the roasted weight; **N** adds a note; **Enter** goes back to Prep.

The summary shows total time, development percentage and weight loss, the whole roast on the phase bar, first crack
and the rise to drop, RoR at first crack and 30 s later, peak heater output and curve-following error.

### Fan floors
The roaster's own minimum fan register reads 50 %. A short press of ↓ stops at **50 %** and again at **40 %**.
To go below a floor, sit on it and hold ↓ for at least 0.6 s. That moves you one step below the floor, and short
presses then work again down to the next floor. The same rule applies to the idle preset and to the 0.1 % steps.

### Rules, alerts and automatic actions
**O** opens the rules, grouped by purpose. Enter edits a value: ← → step it, digits type it, the allowed range is
shown underneath, and range rules (window, rise) take the lower then the upper end. Values are stored in NVS.

| Setting | Default |
|---|---|
| Development time window after first crack | 75–85 s |
| Temperature rise after first crack | +5 to +6 °C |
| Drop-now temperature | 206 °C |
| Safety auto-drop temperature | 225 °C |
| Auto-drop after the curve ends | 120 s |
| Low-RoR alert | below 5 |
| Heat-drop-after-FC alert | 5 % |
| Start fan | 53 % |
| Cold temperature | 40 °C |
| Curve offset | 0 °C |

The bottom bar turns green inside the drop window, red (and keeps beeping) past the development limit or at the
drop-now temperature, and yellow for a heater pinned at 95 % or more for 20 s, heater output falling after first
crack, a rise reached before the window, or low RoR 30 s after first crack.

On BLE loss the reading greys out and the bar turns red. The alarm sounds even when muted, because the roaster
keeps heating to the last target. Roast then reconnects on its own.

Roast drops by itself in only two cases, as safety nets: BT at or above the safety temperature, or the curve
having ended more than *auto-drop after end* seconds ago. Set that value to 0 to match the phone app, which drops
when the curve ends.

### Curve editor
The editor shows the same eight nodes as the phone app (Start, Dry, FC start, FC end, SC start, SC end, Drop,
Total) as a table, with the curve next to it and the selected node highlighted.

- **Space** toggles a node. Drop and Total are always on. A node switched on for the first time starts halfway
  between its enabled neighbours.
- **Enter** (or a digit) edits the row: minutes, Enter, seconds, Enter, temperature, Enter. Tab skips the time.
  A time must lie between the enabled neighbours, Drop must not be later than Total, and Total is at most 30:00.
- A grey temperature has not been edited: that node only pins the curve and does not reshape it, as in the phone
  app. Backspace returns a temperature to the preset.
- **S** saves to `roast/curves/` on the SD card, **N** renames, **B** sets the batch size, **Tab** previews.

New curves start from the phone app's dark-roast preset, and the curve shape is computed the same way the phone
app computes it (see [Parity](#parity-with-the-phone-app)).

## SD card
The card must be FAT32. Without one, logs live only in memory until the summary is closed.

| Path | Contents |
|---|---|
| `roast/curves/*.json` | Curves (format below). |
| `roast/curves/*.alog` | Artisan or myERoast exports, read-only; `temp2` is the target. |
| `roast/logs/<id>.csv` | One row per second: `t,bt,target,heat,fan,ror,pid,machine_t`. Marks and actions are lines starting with `#`. |
| `roast/logs/<id>.json` | Roast summary. |
| `roast/logs/<id>.alog` | Opens in Artisan. |

Curve JSON:
```json
{
  "name": "Gotiti 200g Light V3",
  "note": "Drop 75-85 s after FC at +5-6 C, max 206 C",
  "batch_g": 200,
  "total_s": 570,
  "points": [
    {"t": 0,   "c": 50,  "tag": "CHARGE"},
    {"t": 285, "c": 150, "tag": "DRY"},
    {"t": 470, "c": 199, "tag": "FCs"},
    {"t": 570, "c": 209, "tag": "DROP"}
  ]
}
```
- `t` is in seconds and `c` is in °C.
- Leave out `c` to keep the preset's temperature at that time.
- Tags are `CHARGE`, `DRY`, `FCs`, `FCe`, `SCs`, `SCe` and `DROP`.

## Protocol notes (E-Meter621 BLE)
- **GATT:** service `FFF0`.
  - Commands are written to `FFF1` (write-without-response + notify).
  - Replies arrive as notifications on `FFF2` (notify + read). Read directly, `FFF2` returns the device name.
  - The phone app's code mentions `FFE0`/`FFE1`, but this module exposes `FFF0`.
- **Framing:** Modbus RTU, slave 1. Registers are read with FC03 and written with FC06. A write is confirmed when
  the device echoes it back.
- **Registers** (values are ×10 unless noted):

  | Register | Meaning |
  |---|---|
  | 2 | roast timer (s) |
  | 4 | BT |
  | 9 | PID target |
  | 11 | heater % |
  | 12 | fan % |
  | 13 | `1` = start |
  | 14 | `2` = drop, `1` = stop |
  | 15 | PID on/off |
  | 16 | fan output |
  | 20 | PID cycle |
  | 21–23 | P/I/D |
  | 28 | minimum fan |
  | 29 | maximum heater |

- **Pairing:** the module does not answer SMP pairing. The ESP-IDF demo config sets `CONFIG_BT_NIMBLE_SM_LVL=2`,
  which makes NimBLE silently drop notifications on unencrypted links. Roast sets `ble_hs_cfg.sm_sec_lvl = 0`
  while its link is up and restores it afterwards.
- **Timing:** Roast reads about 2.8 frames/s. Commands are retried up to 3 times with a 2 s echo timeout, and
  15 s without data counts as a lost link.

## Parity with the phone app
The phone app's JavaScript was run in Node and compared with this C++ code case by case.

| Area | Match |
|---|---|
| Curve generation | The preset is cut or extended to the total time. The start temperature is blended in with a raised cosine. Each node's temperature edit stretches the curve linearly between neighbouring nodes. Results agree with the phone app to within 0.01 °C per second. |
| RoR | \|BT − BT 29 readings earlier\| ÷ 0.5, shown after 35 s. |
| Curve time base | Machine timer (register 2), falling back to the local timer if they differ by more than 5 s. |
| Commands | Same registers and values for start, charge without a curve, fan ±1, drop, stop, and the per-second target write. |

Deliberate differences:
- Charging with a curve writes the first target (register 9) before starting the PID.
- Turning PID off also writes the current heater value (register 11), so the output does not jump.
- P re-enables PID mid-roast.
- Heater steps are sent only in manual mode.
- The curve end does not force a drop (see *auto-drop after end*).

Not implemented:
- the phone app's automatic fan schedule;
- writing run parameters (registers 20–29).

## Building
ESP-IDF **v5.4.2**:
```sh
python3 fetch_repos.py        # clones M5GFX, M5Unified, mooncake, ... into components/
idf.py build                  # -> build/cardputer-roast.bin (the Launcher app image)
```
CI does the same in the `espressif/idf:v5.4.2` container. Pushing a `v*` tag publishes a release with the `.bin`
attached.

### Desktop preview
The UI can be checked without the device. `tools/preview/` compiles the real Roast code (pages, keys, curves,
logs and the simulator) against a stand-in HAL and a fake clock, plays a scripted roast and writes every page as
a 240×135 PNG:
```sh
export IDF_PATH=~/esp/esp-idf-v5.4.2      # for cJSON
tools/preview/build.sh && build_preview/preview build_preview/shots
python3 tools/preview/sheet.py build_preview/shots sheet.png   # optional contact sheet (Pillow)
```
CI uploads the same screenshots as the `ui-preview` artifact.

### Fonts
The UI uses Space Grotesk (SIL OFL 1.1, `tools/fonts/OFL.txt`) as anti-aliased VLW fonts compiled into the
firmware. `python3 tools/fonts/make_vlw.py` regenerates `main/apps/app_roast/fonts/roast_fonts_data.h` (Pillow).

## USB serial debug console
The app listens on the USB serial port. `tools/rdbg.py` drives it:
```sh
python3 tools/rdbg.py quiet key s wait 2 shot prep.png
python3 tools/rdbg.py raw regs 2
python3 tools/rdbg.py raw 'ls /sdcard/roast/logs' 2
```
The console accepts:
- key presses and holds, and typed text;
- full-screen (240×135) screenshots;
- `quiet`, `ping` and `regs`;
- SD listing (`ls`), file dump (`cat`), file removal under `/sdcard/roast/` (`rm`) and card info (`sdinfo`).

## License
[MIT](LICENSE). Built on the M5Stack [M5Cardputer-UserDemo](https://github.com/m5stack/M5Cardputer-UserDemo)
(`CardputerADV` branch, MIT), whose notice is kept in `LICENSE`. Third-party parts keep their own licenses:
- Space Grotesk font: SIL Open Font License 1.1 (`tools/fonts/OFL.txt`);
- M5GFX, M5Unified, mooncake, mooncake_log and smooth_ui_toolkit, fetched by `fetch_repos.py`: see their
  repositories.

Everything else in this repository, including the dark-roast preset table in
`main/apps/app_roast/preset_dark.h`, is released under the MIT license.

App packaging targets [bmorcelli/Launcher](https://github.com/bmorcelli/Launcher).
