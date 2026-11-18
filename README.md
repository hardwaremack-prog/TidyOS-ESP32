# TidyOS for ESP32

**One-click install: https://hardwaremack-prog.github.io/TidyOS-ESP32/** (Chrome or Edge)

A tiny general-purpose computer on a NodeMCU ESP32:

- **Two editions, picked with a switch on the flasher:**
  - **VGA** (frozen at 2.6.8, no longer updated): a 640 × 480 desktop on a VGA monitor (8 or 4 colours, picked automatically to fit in RAM) with a Bluetooth keyboard and mouse
  - **HTML5:** the same desktop in a web browser on any phone, tablet or PC on your Wi-Fi. No monitor or resistors; the browser is the keyboard and mouse
- **Bluetooth keyboard and mouse** (Bluetooth LE, VGA edition)
- **microSD card** for your files (falls back to internal flash with no card)
- **Wi-Fi** with a clock that sets itself from the internet
- Apps: **Calculator, Neat Lite** (writing with fonts, sizes, colours and spell check), **Tidy Lite** (spreadsheet: opens and saves .csv and .xlsx, with drag-select, right-click insert/delete rows and columns, resizable sizes, left/center/right, Auto sum, check boxes, row shading and a frozen top row), **Ready Lite** (to-do), **Steady Lite** (bills), **Rugged Lite** (grid-down guide), **Doodle** (MacPaint-style drawing), **Pictures** (JPEG/BMP viewer), **Music** (MP3 and MIDI with shuffle, plays in your browser), **Nic Sweep** (who's on your network, export to Tidy or Excel), **Screenshot** (also the Print Screen key), **Clock** (watch face, alarm, presets, world time, stopwatch), **Latch** (password safe - opens the same .latch vaults as the Latch web app), **Messages, Browser, Files, Settings, Task Manager, Ping, Switches**
- **Printing** to network printers (found automatically), through a Pi with CUPS, or Save as PDF
- **Messages** with The Shelf: chat with every Shelf on your Wi-Fi, and join the Shelf boards' 2.4 GHz Coms mesh
- **Wi-Fi file sharing** (like Shelf Lite): one switch turns on a web page for copying files to and from the SD card
- **Games:** Sammy the Snake (with shuffled MIDI music), Minesweeper, Blocks, 2048, **Bricks** (Arkanoid-style), **Paradrop** (Airborne!-style), **Deep Drill**, **Missile Command** and **StuntCopter**. Arrow keys or WASD
- **Games:** every game that moves has a **Pause** button next to New game. Sammy's one **Play music / Pause music** button plays your MIDI folder, shuffled
- **HTML Games / HTML Programs:** put web games in **SD:/games** and web programs in **SD:/programs** (a .html file, or a folder with index.html); Play opens them in a new browser tab
- **Windows minimize to the bottom bar:** the **–** button next to the X sends a window to the bottom bar (between RAM and the clock); click its button there to bring it back. Games pause while minimized
- **Bottom bar:** TidyOS, version and this board's ID (e.g. `v2.5.2  A93160`), Wi-Fi and address, free RAM with an **Optimize** button that frees what isn't being used, minimized windows, and the clock
- **Opens Office files:** .csv and .xlsx open in Tidy; .rtf, .doc and .docx open in Neat
- **Start-up screen:** an **Update** button (switches to black & white just for the update, then puts your colours back), and a start-up splash where the app icons march in one by one like a classic Mac while each step is ticked off. It waits for the weather and headlines so the desktop opens filled in
- **Little OLED screen (optional):** a 128 × 64 SSD1306 on **GPIO 21 (SDA) / 22 (SCL)**, the same wiring as The Shelf on ESP32, is found at start-up. It shows the start-up progress, then turns through: how to get in (hotspot name, password and address, or the home Wi-Fi address and .local name), the clock, the weather, world times, the system (RAM, Wi-Fi, uptime), the song playing and new messages, about 5 seconds each
- **Top bar of the web page (HTML5):** Keyboard, Task Manager, **Restart**, **Sleep** (deep sleep), Full screen, Size, and a **volume bar** in Music
- **Desktop panel:** time, date, weather in °F with a 4-day temperature graph (day letters, a red **freezing** line at 32° with an arrow, and point at the graph to see that hour's temperature), Moscow / Los Angeles / Tokyo times **with their temperatures**, daily affirmations (>> shows up to 3), up to six news headlines, a world map shaded for day and night with a you-are-here marker, and a drop box: drag files onto the page and TidyOS asks which SD folder they go in
- **Git-Store:** a cloud folder for each board (up to 1 GB, 25 MB a file), kept by a free Cloudflare Worker - see [gitstore/](gitstore/README.md)
- **Task Manager** (Ctrl+Shift+Esc): live CPU graphs for both cores (with a Test button that runs a core flat out), RAM, open apps, every FreeRTOS task, network, storage and power
- **Start-up switches:** before TidyOS starts you can turn the SD card, mesh, blue light and Shelf messages on or off and pick 4 colours, 8 colours or black & white, to trade features for memory
- **Updates itself from GitHub:** Settings → Check for updates

> Status: version 2.7.0. It runs every day on a NodeMCU-32S (HTML5 edition, in-line SD wiring). Newer features may still have rough edges.
> If something misbehaves, open the Serial Monitor at 115200 to see the start-up log.

---

## What it looks like

*Real screenshots of version 2.5.2 running on a NodeMCU-32S (HTML5 edition, 4 colours), taken from the browser page.*

| | |
|---|---|
| ![Desktop](docs/screens/01-desktop.png) | **Desktop** – apps on the left; on the right the time, weather with a 4-day graph (the mouse is over it, showing Sat 11pm 41°, with "freezing →" marking 32°), world times with their temperatures, an affirmation, six headlines and a day/night map. The bottom bar shows the version, board ID, Wi-Fi, RAM and the **Optimize** button. |
| ![Games](docs/screens/02-games.png) | **Games** – nine games. Every window has **–** (minimize to the bottom bar) next to **X**. |
| ![Sammy the Snake](docs/screens/03-sammy-paused.png) | **Sammy the Snake** – New game, **Pause / Resume**, and one **Play music / Pause music** button. |
| ![Clock](docs/screens/04-clock.png) | **Clock** – watch face, world time, alarm with presets, stopwatch. Sammy is minimized on the bottom bar. |
| ![Music](docs/screens/05-music.png) | **Music** – MP3 and MIDI tabs, volume bar, shuffle. Plays through the browser. |
| ![Settings](docs/screens/06-settings.png) | **Settings** – Wi-Fi with **Run TidyOS Hotspot**, Bluetooth (VGA edition), browser helper, clock, colours, file sharing and updates. |

### Drawn previews of the other apps

![TidyOS previews](docs/previews/TidyOS-previews.png)

*These are drawn previews made from TidyOS's own layouts, 4-colour palette and FabGL fonts.*

### What's new in 2.5

- **Minimize to the bottom bar** (the – button beside X), and an **Optimize** button next to RAM.
- **Temperature graph:** point at it for the time and temperature; "freezing →" marks the 32° line.
- **World times show temperatures** for Moscow, Los Angeles and Tokyo.
- **Games:** a Pause button; Sammy's single Play/Pause music button (and no more lock-up with a big MIDI folder).
- **Settings → Run TidyOS Hotspot:** switches TidyOS to its own network only (it leaves your home Wi-Fi): phones and PCs join TidyOS-xxxxxx and open 192.168.4.1. No internet while it's on; turn it off to go back to your Wi-Fi.
- **Start-up:** Mac OS 9-style icon parade with a checklist and a smooth bar.
- **HTML Games / Programs** read **SD:/games** and **SD:/programs**.
- Fixed: a false "the installer didn't run" note after a successful update.

### Earlier

| | |
|---|---|
| ![Desktop](docs/previews/02-desktop.png) | **Desktop** – 24 apps in four columns. The bottom bar shows your Wi-Fi, address and free memory. |
| ![Start-up switches](docs/previews/17-startup-switches.png) | **Start-up switches** – pick what runs before TidyOS starts: SD card, mesh, blue light, Shelf messages, and 4 colours / 8 colours / black & white. It boots by itself after a few seconds. |
| ![Doodle](docs/previews/18-doodle.png) | **Doodle** – a little MacPaint: pencil, brush, eraser, spray, lines, boxes, ovals, paint fill and text, with undo. Saves black-and-white .bmp pictures. The picture lives on the SD card, not in RAM. |
| ![Pictures](docs/previews/19-pictures.png) | **Pictures** – JPEG and BMP photos, dithered to the screen's colours like a classic Mac. **Full colour** opens the original in your browser (HTML5 edition). |
| ![Music](docs/previews/20-music.png) | **Music** – MP3, M4A, WAV and OGG from the SD card, played through your computer's speakers by the browser (HTML5 edition). |
| ![Nic Sweep](docs/previews/21-nic-sweep.png) | **Nic Sweep** – sweeps your network and lists every device, even ones that ignore ping, with its maker when it can tell. **Add all to Ping** keeps watching them; **Open in Tidy** or **Save Excel** keeps a copy. |
| ![Neat Lite word processor](docs/previews/03-neat-word-processor.png) | **Neat Lite word processor** – fonts, a size slider for any word, bold, underline, **text colours**, alignment, paragraph spacing, spell check; saves .rtf that Word and WordPad open. *The text area is drawn by Neat's real layout code with FabGL's fonts.* |
| ![Task Manager](docs/previews/12-task-manager.png) | **Task Manager** – live CPU and memory graphs. **Test** runs a core flat out to see what it can do. |
| ![Bricks](docs/previews/22-games-bricks.png) | **Bricks** – Arkanoid-style: break every brick, catch capsules for a wide paddle, a slower ball or an extra life. |
| ![Paradrop](docs/previews/23-games-paradrop.png) | **Paradrop** – Airborne!-style: guard the gun from helicopters, paratroopers and (from level 2) jets dropping bombs. |
| ![Ping](docs/previews/14-ping.png) | **Ping** – keep pinging a saved list of addresses until you press Stop, with up/down, times and loss for each. |
| ![Updating](docs/previews/16-update.png) | **Updating from GitHub** – Settings → Check for updates downloads and installs a new version by itself. |

## HTML5 edition (desktop in your web browser)

![TidyOS in a web browser](docs/previews/13-html5-in-browser.png)

*Drawn desktop, real browser page: this is the HTML5 page running against a simulated board.*

1. On the flasher page, flip the switch to **HTML5** and click **Install TidyOS (HTML5)**. The only wiring you need is the optional microSD module.
2. Power the board. On a phone or PC, join the Wi-Fi hotspot named after the board, **TidyOS-xxxxxx** (e.g. `TidyOS-A93160`, password `tidyos123`; each board has its own; before 2.6.3 it was TidyOS-Setup). The desktop pops up, or open **http://192.168.4.1**.
3. Open **Settings**, join your home Wi-Fi, **Save**.
4. Back on your home Wi-Fi, open the address at the bottom of the desktop, or **http://tidyos-xxxxxx.local** (the board ID in small letters).

No luck with the hotspot? With the board plugged into a computer, open a serial monitor at 115200 and type `!wifi Your network name|your password` then Enter. `!status` shows the address to open.

**Addresses to log in:**

| Where you are | Join this Wi-Fi | Open |
|---|---|---|
| TidyOS hotspot (first start, no home Wi-Fi yet, or Settings → Run TidyOS Hotspot) | **TidyOS-xxxxxx** (the board ID, e.g. TidyOS-A93160), password `tidyos123` | **http://192.168.4.1** |
| Home Wi-Fi | your own | the address on the bottom bar (e.g. `http://10.0.0.205`) or **http://tidyos-xxxxxx.local** (the board ID from the bottom bar in small letters, e.g. `tidyos-a93160.local`; every board has its own name) |
| File sharing | either of the above | same address plus **:8081** (e.g. `http://192.168.4.1:8081`, `http://tidyos-a93160.local:8081`) |

How it works: TidyOS still draws into its normal 640 × 480 screen memory. A small web server on port 80 sends only the rows that changed to the browser over a WebSocket (about 20 updates a second), and the browser sends keys, clicks, wheel and touches back. A phone's on-screen keyboard works through the **Keyboard** button in the top bar.

- One browser at a time. Opening TidyOS somewhere else takes over and the first tab says so.
- Bluetooth is switched off in this edition, and no VGA signal is generated (the picture is only kept in memory for the browser). With 4 colours that leaves about 85 KB free; pick 8 colours or black & white in the start-up switches.
- In the top bar, **Size** cycles fit → sharp (crisp whole pixels) → 1:1.
- If it can't join your Wi-Fi within 25 seconds, the board's TidyOS-xxxxxx hotspot comes back. It turns itself off a few minutes after TidyOS is on your home Wi-Fi, unless **Settings → Run TidyOS Hotspot** is on: then TidyOS stays on its own network only and doesn't join your Wi-Fi.
- Finding it: the bottom bar shows its address (e.g. `10.0.0.205`); `http://tidyos-xxxxxx.local` (the board ID in small letters, e.g. `tidyos-a93160.local`) works on most PCs, iPhones and Macs; each board has its own name, so several can share a Wi-Fi. A DHCP reservation in your router keeps the address from changing.
- Weather, news and city temperatures are looked up by **your browser** (free Open-Meteo and NPR/BBC feeds), not by the board, so they need the page open.
- Wi-Fi file sharing moves to port **8081** (`http://tidyos-a93160.local:8081`), because port 80 shows the desktop.
- No VGA pins are driven in this edition, so the microSD module can use the **in-line wiring** from The Shelf: plugged straight onto GND · 19 · 18 · 5 · 17 (MISO 19, CLK 18, MOSI 5, CS 17), with its 3v3 pin bent up and one jumper to 3V3. The classic wiring (16/17/14/13) works too; TidyOS tries both. Task Manager → Storage shows which one it found.
- The browser page scales the screen with whole pixels and then smooths it slightly, so text stays even at any window size. **Size** switches to exact 1:1 pixels.

## Switches (save memory)

The **Switches** icon has three on/off switches. All are **off by default** because the ESP32 is short of RAM, and each is remembered after a restart:

| Switch | When on | Memory |
|---|---|---|
| SD card | Documents and TidyOS data live on the microSD card. Off: they go to internal flash (about 1 MB); files on the card stay there | ~23 KB |
| Wireless mesh | The Coms 2.4 GHz mesh with Shelf boards nearby. Shelf messages over Wi-Fi work either way | a few KB |
| Blue light | The status light below | ~2 KB |

## The blue status light

When switched on (Switches), the blue LED on the NodeMCU (GPIO 2) shows what TidyOS is doing. The red one is the power light.

| Blue LED | Means |
|---|---|
| Slow blink | Starting up |
| Breathing (slow fade in and out) | Ready |
| Quick flicker | The microSD card is being read or written |
| Fast flash | Low on memory: close an app |

## Updating from inside TidyOS

**Settings → Check for updates.** TidyOS restarts into a lean update mode, checks this repo's web-flasher manifest, and if there's a newer version it downloads it to the **SD card** (HTTPS, certificate-checked, SHA-256 verified). It then restarts into a small **update loader**, which checks the file again, installs it and starts the new TidyOS. It takes about 2 minutes. Your Wi-Fi, settings and files stay.

- Needs Wi-Fi and the microSD card in the board (TidyOS switches the card on just for the update).
- The loader lives in its own flash slot, so a board needs **one install from the web flasher** (1.8 or later) before in-app updates work.
- If anything goes wrong (no Wi-Fi, no card, a damaged download), nothing is changed and TidyOS starts as before.

Flash layout (4 MB): TidyOS app 0x10000 (2.9 MB) · update loader 0x300000 (384 KB) · internal storage 0x360000 (576 KB).

## If it crashes

TidyOS records the crash just before the restart. **Task Manager → Power & info** shows it under *Last crash* (reason, free memory and a list of addresses), and it's added to `tidyos/crash.txt`. A photo of that screen is enough to find the line of code: the matching debug file for the current version is in `debug/`.

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
3. Pick the ESP32's port. When updating, leave **Erase device** unticked to keep your Wi-Fi and settings. If it won't connect: hold **BOOT**, tap **EN**, let go of **BOOT**. The board then waits in install mode as long as you need. (A 10 µF capacitor from EN to GND usually makes this automatic.)
4. Press **EN/RST** when it finishes.

Manual alternative: in https://espressif.github.io/esptool-js/ flash `firmware/TidyOS-ESP32-full.bin` (VGA) or `firmware/TidyOS-ESP32-HTML5-full.bin` (HTML5) at address **0x0**.

## 4. First start (VGA edition)

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
| **Rugged Lite** | The Rugged app's offline guide for when the grid is down. **If this happens:** 33 emergency cards (bleeding, CPR, CO, pipes freezing, tornado...). **Field guide:** 60 topics in 11 groups, with search. **How long can we last?:** water, food, firewood and fuel in days, flagging anything under a week. **Barter board:** have / need / trades made. All of it can be printed | `/SD/tidyos/rugged.txt` |
| Browser | Text web browser with numbered links. Click a `[number]` or type it in the address box. Backspace = back | – |
| Files | Browse the SD card. Open, new folder, rename, delete. **Wi-Fi sharing** button | – |
| Games | Sammy the Snake (arrow keys), Minesweeper (click, right-click to flag), Blocks (falling blocks: arrows, Up turns, Space drops, P pauses), 2048 (arrow keys). Best scores are kept | – |
| Settings | Wi-Fi, Bluetooth, browser helper, clock, screen colours | saved in ESP32 flash |
| Ping | Saved list of IP addresses or names. **Start** pings every one about once a second until **Stop**: up/down, last, average, min, max and loss for each, plus a running log like `ping -t`. A target counts as down after 2 misses in a row. List saved in `tidyos/ping.txt` | – |
| Task Manager | Live CPU (per core) and RAM graphs, open apps (switch / end task), all FreeRTOS tasks with core, priority and stack left, network (signal, IP, router ping, traffic, Shelves, mesh, Bluetooth), storage (SD, flash, settings, RAM), power estimate, chip temperature, last reset reason. Open with Ctrl+Shift+Esc or Ctrl+Alt+Del | – |

## 6. Printing

Neat, Tidy, Ready and Steady all have a **Print** button.

- **Network printer:** click *Find printers* and pick yours (it finds AirPrint / IPP printers on your Wi-Fi), or type its IP address. TidyOS sends a PDF, and if the printer doesn't take PDFs, it sends page images (PWG raster) instead. Most printers made since about 2015 accept one or the other.
- **Through the Pi helper:** if the helper (below) runs on a Pi that has CUPS with your printer added (`sudo apt install cups`), pick *Through the Pi helper*. Any printer the Pi can use will work.
- **Save as PDF:** writes `Documents/<name>.pdf` to the SD card.

## 7. Wi-Fi file sharing

Turn on **Wi-Fi sharing** in Files (or *File sharing* in Settings). Then open the address it shows (like `http://192.168.1.40`, or `http://tidyos-xxxxxx.local`, the board ID in small letters) in any browser on the same Wi-Fi: phone, PC or Pi.
- Browse folders, download files, upload many files at once (button or drag and drop), make folders, rename and delete.
- Uploads go straight to the SD card in small pieces, so big files are fine.
- It stays off until you switch it on, and TidyOS remembers your choice after a restart.
- Anyone on your Wi-Fi can reach it while it's on, so switch it off when you're done.

## 8. Spell check

Neat underlines words it doesn't know as you type, using a built-in ~78,000-word English dictionary. **Spelling** steps through each word and offers *Change*, *Ignore* or *Add to dictionary*. Words you add are kept in `/SD/tidyos/mywords.txt`.

## 9. Messages and The Shelf

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

## 10. Browser: two ways to use it

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

## 11. Limits (the honest list)

- **RAM is the boss.** The ESP32 has 520 KB, and Wi-Fi plus Bluetooth use about half of it. A 640×480 screen takes 77 KB at 4 colours or 115 KB at 8 colours. TidyOS tries 8 colours first and falls back to 4 if the memory isn't there. You can force a choice in Settings.
- Only **Bluetooth LE** keyboards and mice work. Classic Bluetooth would need about 60 KB more RAM.
- The browser is text only: no images and no JavaScript. Pages are capped at ~16 KB of text.
- Documents open up to ~48 KB.
- The ESP32 only does 2.4 GHz Wi-Fi.
- Printouts are plain monospaced text (Courier). Very old or driver-only printers with no IPP/AirPrint support need the Pi helper with CUPS.
- Spell check is English only. Capitalised names it doesn't know get underlined; use *Add to dictionary*.
- The NodeMCU has no power meter, so Task Manager's power figure is an **estimate** from what's switched on. CPU % is measured from each core's idle time, so it settles after a few seconds. The chip temperature sensor is rough (±10 °C) and missing on some chips.

## 12. Building from source

**PlatformIO:** open this folder and click Build/Upload (`platformio.ini` is included).

**Arduino IDE:**
- Boards Manager: **esp32 by Espressif, version 2.0.17**. Version 3.x does not work with FabGL.
- Libraries: **FabGL 1.0.9** and **NimBLE-Arduino 1.4.3**
- Board: **ESP32 Dev Module**. Partition scheme: **Huge APP (3MB No OTA/1MB SPIFFS)**
- Open `source/TidyOS/TidyOS.ino` and click Upload.
- Optional, for the SD-activity flicker and the crash recorder: add `-Wl,--wrap=ff_disk_read,--wrap=ff_disk_write,--wrap=esp_panic_handler` to the linker flags (PlatformIO already does).
- For the HTML5 edition, uncomment `#define TIDYOS_HTML5` in `config.h` first (PlatformIO: build the `nodemcu-32s-html5` environment).

Source map: `core.cpp` (start-up, desktop, theme), `input.cpp` (Bluetooth HID host),
`net.cpp` (Wi-Fi, web fetching, HTML→text), `widgets.cpp` (text area, spreadsheet engine), `apps.cpp` (all apps),
`print.cpp` (PDF / PWG raster / IPP), `spell.cpp` + `dict.cpp` (spell check), `shelfmsg.cpp` (The Shelf messaging + Coms mesh),
`fileserver.cpp` (Wi-Fi file sharing page), `games.cpp` (the games), `screen.cpp` (HTML5 edition: web screen, WebSocket, setup hotspot).

Built on [FabGL](https://github.com/fdivitto/FabGL) (GPL v3) and [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) (Apache 2.0).

## 13. Guides

Two reference books for the hardware TidyOS runs on (PDF):

- **[The ESP32 Classic Field Guide](docs/guides/ESP32-Classic-Field-Guide.pdf)**: the chip, CPU, memory, radios, a colour-coded pinout of the NodeMCU / DevKit V1 board, every pin and what it can do, sleep modes, and project ideas.
- **[Heltec V3 vs Heltec V4 vs NodeMCU ESP32](docs/guides/Heltec-V3-vs-V4-vs-NodeMCU-ESP32.pdf)**: side-by-side specs, ESP32-S3 vs classic ESP32, what changed on the V4, which pins each board uses, and which board to pick for which project.
