# TidyOS for ESP32

**One-click install: https://hardwaremack-prog.github.io/TidyOS-ESP32/** (Chrome or Edge)

A tiny general-purpose computer on a NodeMCU ESP32:

- **640 × 480 VGA desktop** (8 or 4 colours, picked automatically to fit in RAM)
- **Bluetooth keyboard and mouse** (Bluetooth LE)
- **microSD card** for your files (falls back to internal flash with no card)
- **Wi-Fi** with a clock that sets itself from the internet
- Apps: **Calculator, Neat Lite** (writing, with spell check), **Tidy Lite** (spreadsheet), **Ready Lite** (to-do), **Steady Lite** (bills), **Messages, Browser, Files, Settings, System**
- **Printing** to network printers (found automatically), through a Pi with CUPS, or Save as PDF
- **Messages** with The Shelf: chat with every Shelf on your Wi-Fi, and join the Shelf boards' 2.4 GHz Coms mesh

> Status: version 1.0. It compiles cleanly against the pinned library versions, but it has **not been tested on real hardware yet**.
> If something misbehaves, open the Serial Monitor at 115200 to see the start-up log.

---

## 1. Parts

| Part | Notes |
|---|---|
| NodeMCU ESP32 (ESP32-WROOM-32, 4 MB flash) | NodeMCU-32S or ESP32 DevKitC. Not a WROVER board, because GPIO 16/17 are used for the SD card. |
| VGA socket (DE-15 female) | A breakout board with screw terminals is easiest |
| Resistors | 3 × 270 Ω, 3 × 820 Ω (800 Ω is also fine), 2 × 100 Ω |
| microSD module + card | A 3.3 V SPI module. Cards up to 32 GB, formatted FAT32 |
| Bluetooth **LE** keyboard and mouse | Most keyboards and mice from the last ~8 years work, including multi-device ones. Very old "Bluetooth 3.0 only" models won't show up. |
| VGA monitor | Must accept 640×480 at 60 Hz (nearly all do) |

## 2. Wiring

![wiring](wiring.png)

| ESP32 pin | Through | Goes to |
|---|---|---|
| GPIO 22 | 270 Ω | VGA pin 1 (red) |
| GPIO 21 | 820 Ω | VGA pin 1 (red) |
| GPIO 19 | 270 Ω | VGA pin 2 (green) |
| GPIO 18 | 820 Ω | VGA pin 2 (green) |
| GPIO 5  | 270 Ω | VGA pin 3 (blue) |
| GPIO 4  | 820 Ω | VGA pin 3 (blue) |
| GPIO 23 | 100 Ω | VGA pin 13 (H-sync) |
| GPIO 15 | 100 Ω | VGA pin 14 (V-sync) |
| GND | – | VGA pins 5, 6, 7, 8, 10 |
| GPIO 16 | – | SD MISO |
| GPIO 17 | – | SD MOSI |
| GPIO 14 | – | SD SCK / CLK |
| GPIO 13 | – | SD CS |
| 3V3 (or VIN for 5 V modules) | – | SD VCC |
| GND | – | SD GND |

This is the same VGA pinout as the LilyGO "FabGL VGA32" board, so TidyOS also runs on that board unchanged.

## 3. Flash it (no tools needed)

1. Plug the NodeMCU into your computer with USB.
2. In **Chrome or Edge**, open **https://hardwaremack-prog.github.io/TidyOS-ESP32/** and click **Install TidyOS**.
3. Pick the ESP32's port. (If it won't connect, hold the **BOOT** button while clicking Install.)
4. Press **EN/RST** when it finishes.

Manual alternative: in https://espressif.github.io/esptool-js/ flash `firmware/TidyOS-ESP32-full.bin` at address **0x0**.

## 4. First start

1. TidyOS shows the desktop with a welcome note.
2. **Put your Bluetooth keyboard in pairing mode.** Until a keyboard is connected, TidyOS accepts any keyboard or mouse it finds in pairing mode, so you're never stuck.
3. **Put your mouse in pairing mode** next. (You can also add devices later in Settings → Find devices → Pair.)
4. Open **Settings** → *Find networks* → pick your network → type the password → **Connect** → **Save**.
5. Set your UTC offset under Settings → Clock (Michigan is −5, with Daylight saving ticked).

Paired devices reconnect by themselves on the next power-up.
No Bluetooth yet? Typing in the Arduino Serial Monitor (115200 baud) also works as a keyboard.

## 5. The apps

| App | What it does | Where files go |
|---|---|---|
| Calculator | Four-function with %, +/−. Works from the keyboard too | – |
| **Neat Lite** | Text editor with live spell check (dotted underline) and a Spelling button that offers suggestions. Ctrl+S save, Ctrl+O open, Ctrl+N new, Ctrl+P print. Saved light | `/SD/Documents/*.txt` |
| **Tidy Lite** | 26 × 200 spreadsheet. Formulas: `=A1+B2*3`, `SUM AVG MIN MAX COUNT ROUND IF ABS SQRT INT`, ranges like `A1:A9` | `.csv` (opens in the full Tidy, Excel, etc.) |
| **Ready Lite** | To-do list with done/undo and a progress count. Saves automatically | `/SD/tidyos/ready.txt` |
| **Steady Lite** | Monthly bills: amount, due day, paid. Shows total, paid and left, plus encouragement. "New month" resets paid flags | `/SD/tidyos/steady.csv` |
| Messages | Chat with The Shelf on your PCs and Pis, and with Shelf boards, over Wi-Fi. Second tab: the Shelf boards' 2.4 GHz Coms mesh | `/SD/tidyos/msg-*.txt` |
| Browser | Text web browser with numbered links. Click a `[number]` or type it in the address box. Backspace = back | – |
| Files | Browse the SD card. Open, new folder, rename, delete | – |
| Settings | Wi-Fi, Bluetooth, browser helper, clock, screen colours | saved in ESP32 flash |
| System | Free RAM, uptime, Wi-Fi signal, storage | – |

## 6. Printing

Neat, Tidy, Ready and Steady all have a **Print** button.

- **Network printer:** click *Find printers* and pick yours (it finds AirPrint / IPP printers on your Wi-Fi), or type its IP address. TidyOS sends a PDF, and if the printer doesn't take PDFs, it sends page images (PWG raster) instead. Most printers made since about 2015 accept one or the other.
- **Through the Pi helper:** if the helper (below) runs on a Pi that has CUPS with your printer added (`sudo apt install cups`), pick *Through the Pi helper*. Any printer the Pi can use will work.
- **Save as PDF:** writes `Documents/<name>.pdf` to the SD card.

## 7. Spell check

Neat underlines words it doesn't know as you type, using a built-in ~78,000-word English dictionary. **Spelling** steps through each word and offers *Change*, *Ignore* or *Add to dictionary*. Words you add are kept in `/SD/tidyos/mywords.txt`.

## 8. Messages and The Shelf

**The Shelf (Wi-Fi):** TidyOS speaks The Shelf's messaging protocol (presence on UDP 41234, HTTP on port 8080).
- It shows up in the Messages list of every Shelf on your network: the PC/Pi version and the Heltec/ESP32 boards.
- You can chat one-to-one with any of them.
- If a Shelf doesn't appear on its own, use *Add by address*.
- Pictures sent from a Shelf show as "[picture]".

**Coms mesh (2.4 GHz):** TidyOS joins the Shelf boards' ESP-NOW mesh as a full member.
- It does room-style chat, relays messages for others and sends "received" receipts.
- It asks for the last 12 hours of messages when it meets a board.
- ESP-NOW only hears boards on the same Wi-Fi channel. Boards running their own Wi-Fi use channel 1; boards on your home Wi-Fi use its channel.
- With no LoRa radio, it doesn't hear the 900 MHz side directly; a Heltec board carries messages between the two.

## 9. Browser: two ways to use it

**On its own:** plain `http://` sites load directly. Secure `https://` sites and searches go through
[FrogFind](http://frogfind.com), a free service that turns modern pages into simple HTML for old computers.

**With the helper (recommended):** run `helper/tidyos_proxy.py` on your Pi or The Shelf.
It does the https, decompression and HTML cleanup, so the ESP32 just shows text. It needs only Python 3, with nothing to install.

```bash
python3 tidyos_proxy.py            # prints the address to type into TidyOS
python3 tidyos_proxy.py --printer Office_Laser   # pick a CUPS printer for TidyOS printouts
```

Then in TidyOS: **Settings → Browser → Helper address** = `192.168.x.x:8088` → Save.
With the helper running, searches use DuckDuckGo.

To start it automatically on a Pi:

```bash
sudo cp tidyos_proxy.py /usr/local/bin/
sudo tee /etc/systemd/system/tidyos-helper.service >/dev/null <<'EOF'
[Unit]
Description=TidyOS browser helper
After=network-online.target
[Service]
ExecStart=/usr/bin/python3 /usr/local/bin/tidyos_proxy.py --port 8088
Restart=always
User=nobody
[Install]
WantedBy=multi-user.target
EOF
sudo systemctl enable --now tidyos-helper
```

## 10. Limits (the honest list)

- **RAM is the boss.** The ESP32 has 520 KB, and Wi-Fi plus Bluetooth use about half of it. A 640×480 screen takes 77 KB at 4 colours or 115 KB at 8 colours. TidyOS tries 8 colours first and falls back to 4 if the memory isn't there. You can force a choice in Settings.
- Only **Bluetooth LE** keyboards and mice work. Classic Bluetooth would need about 60 KB more RAM.
- The browser is text only: no images and no JavaScript. Pages are capped at ~16 KB of text.
- Documents open up to ~48 KB.
- The ESP32 only does 2.4 GHz Wi-Fi.
- Printouts are plain monospaced text (Courier). Very old or driver-only printers with no IPP/AirPrint support need the Pi helper with CUPS.
- Spell check is English only. Capitalised names it doesn't know get underlined; use *Add to dictionary*.

## 11. Building from source

**PlatformIO:** open this folder and click Build/Upload (`platformio.ini` is included).

**Arduino IDE:**
- Boards Manager: **esp32 by Espressif, version 2.0.17**. Version 3.x does not work with FabGL.
- Libraries: **FabGL 1.0.9** and **NimBLE-Arduino 1.4.3**
- Board: **ESP32 Dev Module**. Partition scheme: **Huge APP (3MB No OTA/1MB SPIFFS)**
- Open `source/TidyOS/TidyOS.ino` and click Upload.

Source map: `core.cpp` (start-up, desktop, theme), `input.cpp` (Bluetooth HID host),
`net.cpp` (Wi-Fi, web fetching, HTML→text), `widgets.cpp` (text area, spreadsheet engine), `apps.cpp` (all apps),
`print.cpp` (PDF / PWG raster / IPP), `spell.cpp` + `dict.cpp` (spell check), `shelfmsg.cpp` (The Shelf messaging + Coms mesh).

Built on [FabGL](https://github.com/fdivitto/FabGL) (GPL v3) and [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) (Apache 2.0).
