# Cardputer Roast

A roasting controller for the **M5Stack Cardputer ADV** that drives an **E-Roast / E-Meter621 BLE** fluid-bed
roaster (the machine controlled by the *myERoast* phone app) over Bluetooth LE.

It runs as an app under the community [Launcher](https://github.com/bmorcelli/Launcher): Launcher boots it, and
Esc in Roast goes back to Launcher.

Roast uses the whole 240×135 screen; there is no system bar. The top-right corner shows the battery level, plus
a yellow **SIM** tag whenever the built-in simulator is active instead of the roaster.

> **Safety.** This firmware switches a heater on and off. It is not affiliated with or endorsed by the roaster's
> maker, and it was written from observed behaviour, not from vendor documentation. Never leave a roast
> unattended, and keep the machine's own controls within reach.

## Install

1. Install Launcher on the Cardputer ADV, using M5Burner or the
   [Launcher web flasher](https://bmorcelli.github.io/Launcher/).
2. Download `cardputer-roast-<version>.bin`:
   - releases are on the Releases page;
   - every CI run also uploads it as an artifact.

   This file is the bare app image that Launcher installs. It is not a merged full-flash image.
3. Install it in one of two ways:
   - **SD card:** copy the file to the SD card. In Launcher, open the SD browser, pick the file and install it.
   - **USB serial:** stay in the Launcher menu and run:
     ```sh
     pip install pyserial
     python3 tools/launcher_flash.py Roast cardputer-roast-<version>.bin
     ```
4. Optional: to start Roast automatically after power-on, turn on Launcher's boot-to-app setting (CFG menu).

   When this setting is on, Esc in Roast shows Launcher's boot screen for about 5 s, then reopens Roast.

## Using it

Turn the *myERoast* app off first: the roaster accepts only one BLE connection at a time.

Roast scans for a device named `E-Meter…BLE` and connects on its own. To try it without a machine, use the
built-in simulator (a simple thermal model): press **D** on the connect screen, or **D D** in prep. **R** goes back
to the real roaster.

### Prep
| Key | Action |
|---|---|
| S | Fan on, no heat. Use it to find a fan setting with cold air before adding beans. |
| ↑ ↓ (`;` `.`) | Fan ±1 % (with Aa held: ±0.1 %). While idle, this changes only the start-fan preset. |
| Enter | Charge: starts the timer and the roaster's PID, which follows the curve. |
| C / E | Pick a curve / edit a curve. |
| W / B | Green weight (g) / bean name. |
| O / H | Rules and alerts / help. |
| Tab | Curve chart. |
| M | Mute. |
| X X | Stop the fan. |
| Esc | Back to Launcher. Blocked while the fan is running. |

### Roasting
| Key | Action |
|---|---|
| Y / F / G | Mark drying end / first crack / end of first crack. Press the same key twice more to clear the mark. |
| D D or Space Space | Drop. |
| ↑ ↓ | Fan ±1 %. |
| P | Manual heat: PID off, heater held at its current output. ← → then change it by ±5 %. P again returns to the curve. |
| Tab | Toggle the numbers view and the chart. |

Exit is locked until the beans are dropped.

### Cooling and summary
- **X X** stops the machine. The screen tells you once the bean temperature (BT) is below the cold temperature.
- **W** sets the roasted weight, and the summary then shows weight loss.
- **N** adds a note.
- **Enter** goes back to prep.

The summary shows:
- drying, first-crack and drop times and temperatures;
- development time and percentage;
- temperature rise from first crack to drop;
- RoR at first crack and 30 s later;
- peak heater output;
- curve-following error.

### Fan floors
The roaster's own minimum fan register reads 50 %. A short press of ↓ stops at **50 %** and again at **40 %**.

To go below a floor, sit on it and hold ↓ for at least 0.6 s. That moves you one step below the floor, and short
presses then work again down to the next floor. The same rule applies to the idle preset and to the 0.1 % steps.

### Alerts and automatic actions
Rules can be changed with **O** in prep and are stored in NVS.

| Setting | Default |
|---|---|
| Development time window after first crack | 75–85 s |
| Temperature rise after first crack | +5 to +6 °C |
| Drop-now temperature | 206 °C |
| Safety auto-drop temperature | 225 °C |
| Auto-drop after the curve ends | 120 s |
| Low-RoR alert | below 5 |
| Cold temperature | 40 °C |
| Start fan | 53 % |
| Heat-drop-after-FC alert | 5 % |
| Curve offset | 0 °C |

The drop banner works like this:
- It turns green inside the drop window.
- It turns red, and keeps beeping, past the development limit or at the drop-now temperature.

There are separate alerts for:
- a heater pinned at 95 % or more for 20 s;
- heater output falling after first crack;
- low RoR 30 s after first crack;
- loss of the BLE link.

The BLE-loss alarm sounds even when muted, because the roaster keeps heating to the last target. Roast then
reconnects on its own.

Roast drops by itself in only two cases, as safety nets: BT at or above the safety temperature, or the curve
having ended more than *auto-drop after end* seconds ago. Set that value to 0 to match the phone app, which drops
when the curve ends.

### Curve editor
The editor has the same eight nodes as the phone app:

> Start, Dry, FC start, FC end, SC start, SC end, Drop, Total

Each row has three columns: enabled, time and temperature.
- **Enabled:** Enter or Space toggles a node. Drop and Total are always on. A node that is switched on for the
  first time starts halfway between its enabled neighbours.
- **Time:** type minutes, press Enter, type seconds, press Enter. A time must lie between the enabled neighbours,
  Drop must not be later than Total, and Total is at most 30:00.
- **Temperature:** type a value and press Enter. A grey value has not been edited: that node only pins the curve
  and does not reshape it, as in the phone app. Backspace returns a value to the preset.
- **Other keys:** S saves to `roast/curves/` on the SD card, N renames the curve, B sets the batch size, and Tab
  previews the curve.

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
- Heater ±5 is sent only in manual mode.
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
- SD listing, file dump and card info.

## Credits
Built on the M5Stack [M5Cardputer-UserDemo](https://github.com/m5stack/M5Cardputer-UserDemo) (`CardputerADV`
branch, MIT; see `LICENSE`), with M5GFX, M5Unified, mooncake and smooth_ui_toolkit. App packaging targets
[bmorcelli/Launcher](https://github.com/bmorcelli/Launcher).
