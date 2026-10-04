# MacAnalytics

<p align="center">
  <img src="logo.jpg" width="160" alt="MacAnalytics">
</p>

<p align="center"><strong>One menu for the state of your Mac.</strong></p>

A small C program. With no arguments it shows a menu, prints the report you pick, and exits.

| | Report | What you get |
|---|---|---|
| 1 | System | Model, chip, macOS, uptime, free disk |
| 2 | CPU | Overall and per-core load, load average |
| 3 | Memory | App, wired, compressed, cache, swap |
| 4 | Storage | Startup disk, APFS volumes, physical drives |
| 5 | Battery | Charge, health, cycles, power adapter |
| 6 | Thermal | SoC, power, battery, and SSD temperatures |
| 7 | Network | Interfaces, addresses, traffic since boot |
| 8 | Processes | Busiest CPU and largest memory |

`a` prints everything. Names and ranges work too.

```bash
make
./mi_temp
```

```bash
./mi_temp all
./mi_temp cpu memory storage
./mi_temp 2-4
```

Build needs clang and the macOS SDK. Memory units match Activity Monitor. Disk units match Finder. Thermal sensors use a private HID interface and sometimes need `sudo`.
