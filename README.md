# mmwave6002

ESP32 + Hi-Link mmWave radar experiments. Two independent PlatformIO projects, one per module.

## HLK_LD6002/LD6002_00 — raw sniffer + TinyFrame decode (HLK-LD6002B)

Started as "read the LD6002 breath/heart-rate module" — turned out the physical unit on hand
is actually the **LD6002B (3D human presence / position tracking)** variant, not the plain
respiratory/heart-rate LD6002. Confirmed by matching a live capture against Hi-Link's official
`HLK-LD6002B human presence Communication protocol.pdf` (TinyFrame format) — decoded real
target coordinates (x≈0.16m, y≈0.82m, z≈0.08m) matching a person sitting in front of the sensor.

**Key finding: the working UART baud rate is 115200**, not the 1,382,400 commonly quoted for
this chip family — confirmed by sweeping candidate bauds and finding 115200 is the only one
that produces valid TinyFrame `01 50 ...` headers with matching checksums. A generic CP210x
USB-serial bridge cannot reliably do 1,382,400 baud anyway (silent failures, all-zero readback)
— this project reads the module through an ESP32's hardware UART instead, sidestepping that.

**Protocol summary** (see Hi-Link's PDF for the full spec):
- TinyFrame: `SOF(1) ID(2,BE) LEN(2,BE) TYPE(2,BE) HEAD_CKSUM(1) DATA(LEN,fields LE) DATA_CKSUM(1)`
- `TYPE 0x0A0A` — presence per detection area (4× uint32, 1=someone/0=nobody)
- `TYPE 0x0A04` — target list (count + per-target x/y/z float, dop_idx, cluster_id)
- Checksum: XOR all bytes in the covered range, then invert, keep the low byte

**Wiring** (LD6002B → ESP32, GPIO16/17 — same pins the LD2450 project below already uses):
```
GND -> GND
TX0 -> GPIO16 (ESP32 RX2)
```
`RX0` deliberately left unconnected — it's already driven by the module's onboard USB-UART
bridge; adding the ESP32 as a second driver on that line would contend with it. `VCC` left
unconnected too — the module was powered via its own USB cable during this work.

Official docs (Hi-Link Google Drive, linked from https://www.hlktech.net/index.php?id=1180):
communication protocol PDF, specification PDF, firmware upgrade + OTA tool, vendor test GUI.

## HLK_LD2450/LD2450_00 — earlier project

Pre-existing work using `rbegamer/HLK-LD2450` + `espsoftwareserial`, `board=denky32`, radar UART
also on GPIO16/17 at 256000 baud. Untouched by the LD6002B work above.
