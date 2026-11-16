# HEROLTE — iOS 16 on the Galaxy S7: state of the port

Summary for whoever picks this up (a developer, or me in December). Everything
here is measured on the device; guesses are marked as such. The long-form
record is [HANDOVER-COMPLET.md](HANDOVER-COMPLET.md) (French) and
[../HEROLTE_PROGRESS.md](../HEROLTE_PROGRESS.md) (Spanish, chronological).

Target: **SM-G930F (herolte, Exynos 8890, Mali-T880 MP12)**, Apple's XNU
8796 (the iOS 16 generation) booted by Samsung's S-Boot through `shim/herolte/`,
running the **unmodified iPhone X iOS 16 userland** to SpringBoard.

---

## 1. What works, verified on the phone

| Area | State | Evidence |
|---|---|---|
| **Boot** | S-Boot → our loader in BOOT (`sda5`) → XNU → SpringBoard, 4–6 min | every boot log in `build/boot-*.txt` |
| **CPU** | 8 cores; Mongoose 2 600 MHz on charger / 1 976 on battery; 728 MHz idle | `HEROLTE CPU: Mongoose at 2600 MHz` |
| **RAM** | ~3 GiB (second bank + the free pieces of the first) | patches 0326–0329 |
| **Storage** | Exynos UFS driver, iOS root HFSX in USERDATA, read/write | `bsd/dev/herolte_ufs.c`, patches 0204+ |
| **GPU** | Mali-T880 at 650 MHz through EAGL → OSMesa → Panfrost → our job service | `GL_RENDERER = Mali-T880 (Panfrost)` |
| **Display** | 750×1334 @2x drawn, scaled by the GPU to the 1440×2560 panel, two buffers on IDMA, real 60 Hz | patches 0296, 0298, 0322 |
| **Wi-Fi** | BCM4359 over PCIe, the phone's own firmware, **WPA2 handshake in the kernel**, `en0` + Apple80211 for wifid | `CONNECTED with WPA2 in 1649 ms`, `DHCP OFFER`, `en0 so far … frames sent` |
| **Sound** | Loudspeaker through I2S + an AudioServerPlugIn (`heroaudio=3`) | `gpu/hakios-audio` |
| **Touch / buttons** | Main panel digitizer and the physical keys | `SWEET HID: touch …`, `A5XELTE KEYS:` |
| **Capacitive keys** | Left = app switcher, right = back, 3 s LED; the phantom `0x82` press is filtered | patches 0347–0349, 0352 |
| **Battery / charger** | MAX77854 at `0x36` (gauge) and `0x66` (charger): `CHG_INT 0xB0`, `CHGIN_OK 0xB2`, `CHGIN_DTLS 0xB3`, live, no latch | `HEROLTE PWR: plug=… vbat=…` |
| **OLED sleep** | DCS `0x28`/`0x10` to sleep and `0x11`/`0x29` to wake, GPU 338/650 MHz, Mongoose back to 728 | `HEROLTE PWR: sleep …` / `wake …` |
| **Thermal** | Four TMU blocks; above 80 °C the Mongoose drops to 728 MHz, back below 70 °C | patch 0331 |
| **Drop zone** | DROP v2: SHA-256, generation, atomic staging, forward recovery | `HEROLTE DROP: package N ready for userland` |

## 2. What is open

| Area | Where it stands | Next step |
|---|---|---|
| **Bluetooth BCM4359** | Hardware mapped and driven: **UART1 `0x14C20000`** (not `0x13630000`, which is `disabled`), pins **GPE0-0..3** function 2, **BT_REG_ON = GPJ1-7** (PERIC1 `+0x60`), **BT_WAKE = GPI2-3** (FSYS0 `+0x20`), **BT_HOST_WAKE = GPA2-3** (ALIVE `+0x40`). Firmware `bcm4359C0_semco.hcd` (59 702 B) loads from the drop zone. Driver `bsd/dev/herolte_bt.c`: 8N1 115200 RTS/CTS, HCI H4. **The HCI reset and the `.hcd` download are the unproven part.** iOS's `bluetoothd` is still in `heronoexec`. | Read `HEROLTE BT: reset ok=…`, `fw bytes=59702`; then expose `IOBluetoothHCIController` / `/dev/herobt` and take `bluetoothd` out of `heronoexec` (watch the 608-byte command line) |
| **USB device mode** | DWC3 answers: `GSNPSID 0x5533280a`, `GCTL 0x30c12004` (device mode), clocks running at handover. But `DCTL=0`, `DALEPENA=0` and **`SETEPCONFIG` on ep0 never raises `CMDACT`** (`ep0 cmd 0x1 timeout`). `DEPSTARTCFG` passes, so the controller answers and the block is in the command FIFO. | Port `dwc3-exynos.c` + `phy-exynos-usbdrd.c` (~970 lines): PHY init, PMU isolation, DCFG, `RUN_STOP=1`, event buffer, EP0 |
| **No interrupts anywhere** | No `IOInterruptEventSource` / `ml_install_interrupt` in any patch: every wait is a poll with a deadline. GPU IRQs are known (SPI 224 GPU, 225 JOB, 226 MMU). | This is the single biggest performance lever (see §4) |
| **Modem, camera, sensors, Metal** | Not started. Metal is why the Today view and Control Center blurs are black. | — |
| **Home gestures** | `home-button-type 1` works (physical Home). Type 2 brings UIKit's red `rdar:45025538` status bar at this size. | — |

---

## 3. The rules this hardware taught us (do not relearn them the hard way)

### 3.1 Never touch `0x14C00000` as if it were a gate you can read

`CMU_FSYS0` register **reads freeze the CPU** when the block is not clocked for
it. `0x14C00000` holds the UART1 pinmux/clock path used by Bluetooth:
`script 6bb58b3` ("never read a CMU register in the Bluetooth clock path") exists
because an early probe hung the kernel there. Write blindly, or check the
devicetree first — never read back to "verify".

Corollary for the USB work: the same rule applies to `CMU_FSYS0_BASE
0x10E90000`. The gate status registers (`+0x1C40`, `+0x1C4C`, `+0x1C50`,
`+0x1C54`) read as `0` when the gate is **open**, not closed — that mistake
cost a whole session.

### 3.2 Never hook SpringBoard's private classes

**A theme injected into SpringBoard took the phone down.** `herosb.c` hooked
`SBDockView`, `SBAppSwitcherPageView`, `_UIStatusBarBatteryView`; on the next
boot the device looped on `xnu mmu on` and froze with a black screen, and only
a full battery drain rearmed the PMIC. The commit is reverted (`86e9dc4`) and
the rule is absolute: **no modification of SpringBoard or of any private
UIKit/CoreAnimation class from a userspace dylib.** iOS 16's interface stays
stock. Details: [HANDOVER-COMPLET.md §23](HANDOVER-COMPLET.md).

SpringBoard is the one process whose failure takes the whole UI with it. A
change there needs a recovery path that does not involve flashing a kernel —
which means it cannot be tested safely at all.

### 3.3 Verify a flash with the payload size, not the partition size

BOOT is 41 943 040 bytes; the image is ~18.7 MB. Comparing the whole partition
to the image fails on padding that is not the image's. Always:

```bash
SZ=$(stat -c %s build/SAFE-xnu-boot-bt9.img)
adb shell "head -c $SZ /dev/block/sda5 | sha256sum"
```

### 3.4 Two reboots after a drop, always

The EAGL shim is loaded **before** the drop installer runs (`EAGL shim loaded`
appears above `package N ready for userland`). One boot installs, the next loads.

### 3.5 Always pass `--generation N`

`DEFAULT_GENERATION = 1` in `tools/herolte-drop.py`. The kernel installs a
package only when the generation changes, so with the default **only the first
package ever written is installed** and every later one is silently ignored.

### 3.6 The L2 flush after every GPU job is required

`hg_flush_mode` 2 (no flush after vertex jobs) gives a **black screen**; 3
(no flush after fragment jobs) gives **artefacts**. Mode 1 is correct. Same for
the presentation blit's sampler encoding: do not guess it.

### 3.7 The command line is full (604 of 608 bytes)

Adding a boot argument means removing one of the same length, or changing a
default in the code.

---

## 4. Performance: where the time actually goes

Measured, not estimated:

- An **idle frame costs 4–10 ms** against a 16.7 ms budget.
- **85–90 % of a draw is inside Mesa/Panfrost**; the shim's wrapper is 10–15 %.
- The GPU is busy **16 %** of the time: it waits, it does not saturate.
- Cost is **per GL call** (~29 µs), not per pixel: lowering the render
  resolution does not help.
- Typing draws **150 times per frame** (33 when idle) → 52 ms. That is the
  keyboard stutter, and it is a queue, not a freeze.
- Real cadence: **64–67 % of frames at 60 Hz, 27–29 % at 30 Hz.** Not a 30 fps
  ceiling: 60 Hz with every third frame missing its slot.

**The one lever left** is asynchronous GPU submission. `herolte_gpu_submit()`
is synchronous because the caller reuses the job chain; overlapping it needs
duplicated chains or DRM fences. That, and GPU interrupts (SPI 225), would
recover the ~4 ms of wait per frame — exactly the margin missing for 60 Hz.

**Closed by measurement, do not reopen:** lowering the resolution, removing
the L2 flushes, changing the blit filter, optimising the surface copy (it is
already zero-copy at 0 µs).

---

## 5. Résumé en français

**Ce qui fonctionne sur le téléphone :** le démarrage jusqu'à SpringBoard,
le GPU Mali-T880, le Wi-Fi BCM4359 avec WPA2 négocié dans le noyau, le son par
le haut-parleur, le tactile et les boutons, les touches capacitives
(multitâche / retour / LED), la jauge et la charge MAX77854 lues en temps réel,
la veille OLED avec les DCS `0x28`/`0x10`/`0x11`/`0x29`, les plafonds
thermiques et les trois gigaoctets de RAM.

**Ce qui reste :** le Bluetooth BCM4359 (le matériel est cartographié et piloté,
le handshake HCI reste à prouver), l'USB en mode périphérique (`SETEPCONFIG`
sur ep0 ne rend jamais la main), et l'absence totale d'interruptions dans le
port — c'est le plus gros levier de performance qui reste.

**Les règles à ne pas réapprendre :**

1. **Ne jamais lire `0x14C00000`** (ni un registre CMU non horlogé) : cela fige
   la CPU. Les registres de porte de `CMU_FSYS0` renvoient `0` quand la porte
   est **ouverte**.
2. **Ne jamais hooker SpringBoard** ni une classe privée d'UIKit : un thème
   injecté a gelé le téléphone sur `xnu mmu on`, écran noir, PMIC bloqué.
   L'interface d'iOS 16 reste d'origine.
3. **Vérifier un flash avec la taille du payload**, pas celle de la partition :
   `SZ=$(stat -c %s …)` puis `head -c $SZ`.
4. **Deux redémarrages** après chaque drop.
5. **Toujours passer `--generation N`** : sans cela, seul le premier paquet est
   jamais installé.
6. **Le flush L2 après chaque job GPU est indispensable** (mode 1) : le retirer
   donne un écran noir ou des artefacts.
7. **La ligne de commande est pleine** (604 octets sur 608).

---

*Last measured on the device: 15–16 November 2026. Development paused until
December 2026; the phone went back to Android in the meantime.*
