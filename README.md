# Ktuyome OS

[![Build](https://github.com/OWNER/ktuyome/actions/workflows/build.yml/badge.svg)](https://github.com/OWNER/ktuyome/actions/workflows/build.yml)

A small 32-bit x86 operating system with a PS5-style interface, written in C (about 4,200 lines of code plus a generated font table). Current version: **1.5**.

## Build and run
    sudo apt install build-essential gcc-multilib grub-pc-bin xorriso mtools qemu-system-x86
    make            # produces ktuyome.iso
    make run        # boots in QEMU with a 64 MB persistent disk (disk.img)

On real hardware or VirtualBox: boot the ISO through BIOS/legacy mode. Attach a virtual IDE disk to keep your files.

## Layout
| File | Purpose |
|---|---|
| boot.S, linker.ld | Multiboot header (asks GRUB for 1280x720x32), GDT, ISR stubs |
| arch.c | IDT, PIC, 1 kHz PIT, RTC, PS/2 keyboard + mouse (scroll wheel), ATA PIO disk, power |
| gfx.c | Backbuffer, antialiased rounded rects, shadows, glows, alpha text, bilinear bitmap scaling, wallpapers |
| svg.c | SVG renderer: paths (incl. arcs), shapes, transforms, gradients, strokes, exact-area antialiasing |
| fs.c | KTFS filesystem (directories, files up to 7.5 MB spanning contiguous 16 KB blocks), persisted to the ATA disk |
| png.c | PNG support: decoder (all colour types and bit depths, transparency, interlaced) and encoder used by screenshots |
| clip.c | Clipboard: one shared text buffer, input sanitising, and the Ctrl+C / X / V / A key mapping every app uses |
| ui.c | Boot splash, home screen, app host, dialogs, widgets, main loop (also meters CPU use for the Task Manager) |
| apps.c, apps2.c, zip.c, viewer.c, paint.c | Notes, Text Editor, Calculator, Files, Zip, Image Viewer, Paint, Settings and About |
| vendor/miniz | Vendored miniz (MIT) for zip create/extract |
| vendor/bearssl | Vendored BearSSL (MIT) for TLS/crypto |
| vendor/quickjs | Vendored QuickJS (MIT) for JavaScript runtime |
| discord.c | Discord client (WebSocket gateway, REST) |
| taskmgr.c | Task Manager: CPU graph, memory breakdown, storage, process list |
| net.c | PCI scan, Intel e1000 driver, ARP, IPv4, ICMP, UDP, DHCP client, DNS resolver, TCP client |
| netapp.c | Network app: console with ping, traceroute, nslookup, tcp, scan, http, udp |

## Controls
Arrows or mouse to move, Enter or click to open, Tab switches Apps/System, Esc goes back, F10 shuts down, F12 takes a screenshot.
Editor: Ctrl+S save, Ctrl+O open, Ctrl+N new, F5 SVG preview. Notes: Ctrl+N new, Ctrl+D delete.
Files: Ctrl+F or / search; Ctrl+Space toggle multi-select; Space select; Delete removes selected; "Zip sel" zips selection.
Zip: Enter or Extract extracts an archive; "Zip up" zips the selected file/folder; Ctrl+F or / search.

## Zip archives and Files search (1.5)
- **Zip app**: browse folders, select a `.zip` to see its entries, press Enter to browse inside and extract a single entry, or use Extract to extract all. "Zip up" zips the selected file/folder. Supports scrollable lists and search.
- **Files multi-select**: Ctrl+Space toggles selection, Space selects, Shift+click range-selects, Ctrl+A selects all, Delete removes all selected, and "Zip sel" creates `selected.zip` from the selection. Search matches are highlighted.
- **Search**: Ctrl+F or `/` filters Files and Zip lists by name; matches are highlighted.
- **Home screen**: scrolls horizontally when apps overflow, with arrow buttons, mouse wheel, and saved/restored scroll position/tab.
- **Settings persistence**: remembers home tab/scroll and last opened Files/Zip folder.

## PNG and screenshots (1.4)
**PNG support.** Ktuyome reads PNG files of every colour type (grayscale, RGB, palette, with alpha) and bit depth (1 to 16), including tRNS transparency and interlaced (Adam7) images, up to 8192 x 8192 and 6 million pixels.
- **Files**: PNG files get a thumbnail with their size in the preview pane; Enter or a double click opens the full-size viewer (any key or a click closes it). The Text Editor does not open PNGs, since they are binary.
- **Icons**: put a PNG named like the SVG (for example `/system/icons/notes.png`) next to the icons and it replaces the SVG; any size works, it is scaled into the 128 px and 48 px icon slots. Delete the PNG to go back to the SVG.

**Screenshots.** F12, from anywhere (home screen, any app, dialogs), saves the screen as `/images/shot-YYYYMMDD-HHMMSS.png` (a `-2`, `-3`... suffix is added if two shots land in the same second) and shows a toast with the name. The mouse cursor and toasts are left out of the picture, and the display brightness setting is not applied. Screenshots are plain 24-bit RGB PNGs and open in any image viewer. The `/images` folder is created automatically, also on disks made by older versions.
- Encoding uses adaptive scanline filters, LZ77 and fixed Huffman codes; a typical 1280x720 screen is a few hundred KB, and encoding takes a moment, so the screen pauses briefly.
- Files can now be larger than 16 KB: they take a run of contiguous 16 KB blocks, up to 7.5 MB. The storage pool grew from 2.5 MB to 7.5 MB (480 blocks). Disks from 1.3 load unchanged.
- To get a screenshot out of the VM, mount the disk image (`disk.img`) with a KTFS-aware tool, or use the host's own screenshot of the QEMU window; there is no shared folder yet.

## Clipboard (1.3)
All apps share one system clipboard (up to 32 KB of text). Text is normalised when copied: CRLF becomes LF, tabs become four spaces, and characters outside printable ASCII are dropped.

| Keys | Action |
|---|---|
| Ctrl+C, Ctrl+Insert | Copy |
| Ctrl+X, Shift+Delete | Cut |
| Ctrl+V, Shift+Insert | Paste |
| Ctrl+A | Select all |

- **Notes and Text Editor**: Shift+arrows, Shift+Home/End and Ctrl+Shift+arrows select; Ctrl+Left/Right jump by word; Ctrl+Home/End go to the start/end. With the mouse, drag to select, double-click to select a word, Shift+click to extend. Typing, Backspace, Delete and Enter replace the selection.
- **Prompts** (save as, new file, URL bar, Discord token): Ctrl+V pastes the first line, Ctrl+A selects the field so the next key or paste replaces it, Ctrl+C / Ctrl+X take the whole field. Prompts now hold 511 characters (was 90).
- **Files**: Ctrl+C / Ctrl+X pick the selected file or folder, Ctrl+V pastes it into the folder you are in (copy gets " copy" added to the name when it already exists; cut moves). The item's path is also put on the text clipboard.
- **Calculator**: Ctrl+C or a click on the display copies the shown value; Ctrl+V pastes a number (`1,234.5`, `-7`).
- **Network**: Ctrl+V pastes the first line into the command line. Ctrl+C still interrupts a running command; Ctrl+Shift+C or Ctrl+Insert copies the command line, or the whole output when the command line is empty. Ctrl+X cuts the command line.
- **Browser**: Ctrl+V opens the URL bar with the pasted text, Ctrl+C copies the page address, Ctrl+Shift+C the focused link's address, Ctrl+A the page text. Ctrl+L now selects the URL so typing or pasting replaces it.
- **Discord**: Ctrl+V pastes the token when logged out, or into the message box inside a channel (line breaks become spaces). Ctrl+C / Ctrl+X copy / cut the message you are typing; Ctrl+C otherwise copies the loaded channel history.

The clipboard lives inside Ktuyome. There is no link to the host clipboard of QEMU or VirtualBox (that needs a guest agent), so text moves between apps, not between the VM and your desktop.

## Task Manager
Apps tab, last tile. Performance view: 60-second CPU graph, memory breakdown, uptime, frames per second, storage. Processes view: every app with status, CPU, memory and times opened; Enter or click switches to that app. Tab/Left/Right switch views.
Ktuyome has no preemptive scheduler, so CPU is measured from the TSC (cycles outside `hlt`) and attributed to the foreground app, or to System on the home screen. Memory is the statically allocated footprint reported by the linker.

## Networking
Ktuyome drives the Intel e1000 NIC, which is the default in QEMU and the default "Intel PRO/1000" adapter in VirtualBox. It gets its address, gateway and DNS server over DHCP, so it works behind any NAT (QEMU user networking gives 10.0.2.15, gateway 10.0.2.2, DNS 10.0.2.3; VirtualBox NAT is similar).
Run `make run` as usual (QEMU adds a NAT network by itself). In VirtualBox leave the adapter on NAT, type Intel PRO/1000 MT Desktop.
The Network app (Apps tab) is a console: `help`, `ip`, `ping [-c N] host`, `traceroute host`, `nslookup host`, `tcp host port`, `scan host [from-to]`, `http host[:port][/path]`, `udp host port text`, `arp`, `stats`, `dhcp`, `setip`, `setdns`. Ctrl+C stops a running command; commands keep running while you leave the app.
Not implemented: HTTPS/TLS, IPv6, IP fragmentation, listening sockets. Ktuyome is a NAT client, not a router. Traceroute through QEMU's NAT only shows the first hop.

## SVG icons
Every home-screen icon is an SVG file in /system/icons. Edit one in the Text Editor, press Ctrl+S, and it updates immediately.

## Third-party components
- **Discord client**: `discord.c` implements a Discord client (REST + WebSocket Gateway) for chat.
- **BearSSL**: vendored in `src/vendor/bearssl` (MIT) for TLS/crypto primitives.
- **QuickJS**: vendored in `src/vendor/quickjs` (MIT) as a JavaScript runtime.
- **miniz**: vendored in `src/vendor/miniz` (MIT) for zip create/extract and PNG compression.
- **Fonts**: Inter and DejaVu Sans Mono (SIL OFL).

## Image Viewer and Paint (1.9)
- **Image Viewer**: browses PNG files and displays them with pan (Ctrl+arrows) and zoom (+/-, wheel).
- **Paint**: simple drawing canvas with brush/erase, color swatches, and export to PNG (`/images/paintN.png`).

## Fonts
src/fontdata.c is generated by tools/mkfont.py from Inter (SIL OFL) and DejaVu Sans Mono.
