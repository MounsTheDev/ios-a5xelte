# Passation complète — iOS 16 sur Galaxy S7 (herolte, Exynos 8890)

Document destiné à une IA ou un développeur qui reprend le projet **de zéro**.
Il contient : ce qu'est le projet, comment il se construit, comment il s'installe,
comment il se mesure, ce qui marche, ce qui ne marche pas, et **tous les pièges
rencontrés** avec leurs symptômes exacts.

Dernière mise à jour : 16 novembre 2026, fin de phase (développement en pause
jusqu'aux vacances de décembre 2026 ; le téléphone est repassé sous Android).

> **Reprise rapide :** lire d'abord [HEROLTE-PROGRESS.md](HEROLTE-PROGRESS.md)
> (une page, anglais + résumé français) : ce qui marche, ce qui reste, et les
> sept règles à ne pas réapprendre. Le présent document est le détail complet.

---

## 1. Le projet en une page

Faire tourner **iOS 16.6 (build 20G75)** sur un **Samsung Galaxy S7 (SM-G930F,
herolte, Exynos 8890)**.

Ce n'est pas une émulation : le noyau XNU d'Apple est compilé pour cette machine
et démarré par le bootloader Samsung via un shim. Les frameworks iOS (SpringBoard,
backboardd, CoreAnimation, UIKit) tournent réellement, sur le vrai matériel.

Conséquence directe : **tout ce qu'Apple attend d'un iPhone doit être réimplémenté
pour ce SoC** — l'affichage, le GPU Mali (alors qu'iOS attend un PowerVR), le
tactile, l'audio, le Wi-Fi, la batterie. C'est ce que fait ce dépôt, sous forme de
**275 patchs noyau** et de plusieurs bibliothèques userspace.

Le port fonctionne : il démarre, affiche SpringBoard, lance des applications
(iTunes Store, YouTube, Discord), se connecte au Wi-Fi, joue du son. Les
touches capacitives (multitâche/retour/LED), la veille OLED et les plafonds
batterie sont validés ; le Bluetooth BCM4359 est en bring-up (Phase 1 `bt1`
flashée, Phase 2 `bluetoothd` à faire).

---

## 2. La machine

| Élément | Valeur |
|---|---|
| SoC | Exynos 8890 (4× Mongoose + 4× Cortex-A53) |
| GPU | ARM Mali-T880 MP12, base MMIO **`0x14ac0000`** |
| Contrôleur d'horloge GPU | CMU_G3D **`0x14aa0000`** |
| Écran | OLED 1440×2560 (Samsung), affiché en 750×1334 par le port |
| RAM | 4 Gio physiques, dont **3 Gio** donnés à XNU |
| Partition boot | **`/dev/block/sda5`**, 40 Mio |
| Tactile | Samsung sec_ts, **0x48 sur HSI2C `0x14E10000`**, INT sur GPA1-0 |
| Touches capacitives | bus **`i2c-gpio`** (bit-bang), **`touchkey@20`** et **`touchkey@40`** |
| Interruptions GPU | SPI **224 (GPU), 225 (JOB), 226 (MMU)** |

---

## 3. État actuel — ce qui marche

- Démarrage complet jusqu'à SpringBoard, en 4 à 6 minutes (le premier boot après
  un nouveau paquet est plus long : il réinstalle ~690 Mo).
- Affichage GPU (Mali) via un pilote Panfrost/OSMesa réécrit pour ce port.
- Accélération graphique réelle : le rendu est fait par le GPU, pas par le CPU.
- Wi-Fi (Samsung AMFI, firmware chargé depuis la drop zone).
- Audio (HDA-like via I2S + codec), y compris les haptiques routées vers le moteur.
- Batterie, capteurs de température, boutons physiques (Power/Volume/Home).
- Applications installées et lançables.
- **Extinction d'écran instantanée** (noir en un tick, plus de fondu) et
  veille OLED réelle : DCS `0x28`/`0x10` au verrouillage, `0x11`/`0x29` au
  réveil, GPU 338 MHz écran éteint.
- **Touches capacitives** : gauche = multitâche, droite = retour, LED 3 s ;
  le faux appui `0x82` est filtré depuis `0352`.
- **Batterie** : plafonds 1 976/546 MHz hors secteur, 1 976/419 MHz sous
  3 550 mV ; chargeur lu en temps réel (`CHGIN_OK` + `CHGIN_DTLS`).
- CPU Mongoose à **2 600 MHz** secteur / **1 976 MHz** batterie, GPU
  650/546 MHz ; 728 MHz au repos.
- Trois Gio de RAM utilisables.

## Ce qui ne marche pas / limites connues

- **Le clavier saccade** : il dessine 150 fois par frame, chaque draw coûtant
  ~300 µs côté Mesa. Voir §11.
- **Pas d'interruptions** : tout le noyau est en polling (§11.4).
- USB non fonctionnel (endpoint 0 répond mais la configuration échoue).
- Pas de DVFS continu : des paliers fixes par profil (secteur/batterie), pas
  d'ajustement fin selon la charge.
- Bluetooth : le matériel `bt1` est flashé et la drop 17 installée ; les logs
  `HEROLTE BT` sont à valider, et la pile iOS (`bluetoothd`, Phase 2) reste
  à faire.
- Le port est en mode développement : `debug=0xc`, pas de sécurité.

---

## 4. Anatomie du dépôt

```
xnu/                 arbre XNU (xnu-8796.141.3, macOS 13.5)
  patches/           267 patchs .patch, appliqués DANS L'ORDRE ALPHABÉTIQUE
bsd/dev/             pilotes ajoutés par les patchs (herolte_gpu.c, herolte_drop*.c…)
iokit/Kernel/arm/    SweetFramebuffer.cpp (le framebuffer)
gpu/
  hakios-gl/src/eagl.c      LE shim OpenGL actif (celui qui tourne)
  eagl/src/eagl.m           ancien shim, NE TOURNE PAS (mais contient des diagnostics)
  mesa/                     script de build + patchs de Mesa (téléchargé au build)
  hakios-audio/             plugin audio CoreAudio
M/lib/               bibliothèques userspace prêtes à mettre dans la drop zone
drop/                fichiers de la drop zone (gl.env, disabled.list, wifi/, …)
ipa/                 les .ipa à installer
tools/               outils de packaging, de log, de drop
build/               artefacts locaux (images, journaux, patchs en cours)
backup/boot-stock.img  boot d'origine, sauvegarde de référence
docs/                documentation
.github/workflows/   CI (xnu.yml, mesa-ios.yml, …)
```

---

## 5. Construire

### 5.1 Le noyau (workflow `xnu.yml`)

```bash
gh workflow run xnu.yml -R <owner>/<repo> --ref <branche> \
  -f machine_config=HEROLTE \
  -f kernel_config=DEVELOPMENT \
  -f macos_version=13.5
```

Durée : **14 à 18 minutes**. Sortie : un artefact contenant
`kernel.development.herolte`.

**Piège** : si l'upload d'artefact échoue (`Failed to CreateArtifact`), le build a
réussi quand même. Relancer avec :

```bash
gh run rerun <run-id> --failed
```

### 5.2 Le shim graphique (workflow `mesa-ios.yml`)

Se déclenche **automatiquement** sur tout push touchant `gpu/hakios-gl/**`.
Produit `libhakiosgl.dylib`, `libOSMesa.8.dylib`, `libherosb.dylib`, etc.

### 5.3 Récupérer les artefacts

```bash
gh run download <run-id> -D .dl
find .dl -name "kernel.development.herolte"
```

---

## 6. Fabriquer une image de boot

Trois étapes, dans cet ordre.

### 6.1 Le payload

```bash
python3 tools/mkpayload.py <kernel> build/herolte-3g.adt build/payload.bin \
  --cmdline "$(cat build/cmdline-2288.txt)"
```

`herolte-3g.adt` est le device tree (3 Gio de RAM). La ligne de commande est
**limitée à 608 octets** — la version actuelle en fait 604, il reste 4 octets.

### 6.2 Le pack

```bash
python3 tools/herolte-pack-boot.py pack backup/boot-stock.img \
  shim/herolte/out/xnu_boot.Image build/xnu-boot.img \
  --append build/payload.bin --partition-size 41943040
```

Affiche `candidate_sha256=…` : **c'est le hash à vérifier après flash**.

### 6.3 La vérification

```bash
python3 tools/herolte-verify-boot.py build/xnu-boot.img   # doit dire "0 fallos"
```

---

## 7. Flasher (TWRP obligatoire)

Le téléphone doit être **en TWRP**, écran allumé (sinon adb se coupe), câble branché.

```bash
adb devices                     # doit afficher "recovery"
SZ=$(stat -c %s build/xnu-boot.img)
adb push build/xnu-boot.img /tmp/b.img
adb shell "dd if=/tmp/b.img of=/dev/block/sda5 bs=1048576 conv=fsync"
adb shell "head -c $SZ /dev/block/sda5 | sha256sum"   # doit égaler candidate_sha256
adb reboot
```

**Toujours vérifier le hash après écriture.** Une écriture partielle ne se voit pas
au boot : elle donne un téléphone qui ne démarre pas.

### Sauvegarde de secours

`build/SAFE-xnu-boot-6a9a5404.img` est l'image stable de référence (2 600 MHz,
extinction instantanée, mode 1). La garder : c'est le retour arrière en 30 secondes.

---

## 8. La drop zone — LE piège majeur du projet

C'est un paquet de ~690 Mo écrit à la fin de la partition `sda5`, que le noyau
installe dans le système de fichiers au démarrage.

```bash
python3 tools/herolte-drop.py adb --generation N \
  autorun=M/hero-run test=M/eagl_ca_test gl.env=drop/gl.env \
  test.env=drop/test.env disabled.list=drop/disabled.list \
  wallpaper.off=drop/wallpaper.off gestures.off=drop/gestures.off \
  lib/libdrm.2.dylib=M/lib/libdrm.2.dylib \
  lib/libOSMesa.8.dylib=M/lib/libOSMesa.8.dylib \
  lib/libheroeagl.dylib=M/lib/libhakiosgl.dylib \
  lib/libherosb.dylib=M/lib/libherosb.dylib \
  lib/libheroapp.dylib=M/lib/libheroapp.dylib \
  audio/HakiosAudio=M/audio/HakiosAudio audio/Info.plist=M/audio/Info.plist \
  wifi/fw.bin=drop/wifi/fw.bin wifi/clm.blob=drop/wifi/clm.blob \
  wifi/nvram.txt=drop/wifi/nvram.txt wifi/network.txt=drop/wifi/network.txt \
  "app=ipa/<fichier>.ipa" ...
```

Durée : **12 à 15 minutes**.

### ⚠️ Piège n°1 : la génération

`DEFAULT_GENERATION = 1` dans `tools/herolte-drop.py`. Le noyau n'installe un
paquet que si la **génération change**. Avec la valeur par défaut, **seul le tout
premier paquet a jamais été installé** — toutes les réécritures suivantes sont
silencieusement ignorées, et on croit tester une nouvelle bibliothèque alors que
l'ancienne tourne toujours.

**Toujours passer `--generation N` avec un N qui change.** Vérifier ensuite dans
le journal : `HEROLTE DROP: package N ready for userland`.

### ⚠️ Piège n°2 : le shim est chargé AVANT l'installation

Dans le journal, `EAGL shim loaded (pid 56)` apparaît **avant**
`package N ready for userland`. Autrement dit, la bibliothèque installée ne prend
effet qu'au démarrage **suivant**.

**Conséquence : après une drop, il faut DEUX redémarrages** — un pour installer,
un pour charger. Sans ça, on mesure l'ancienne version et on devient fou.

### ⚠️ Piège n°3 : le démon adb qui redémarre

`herolte-drop.py` lit la sortie d'adb pour trouver la taille de partition. Si le
démon adb vient de démarrer, son message `* daemon started successfully *` pollue
la sortie et l'outil plante avec `invalid literal for int()`. Relancer suffit.

---

## 9. Arguments de démarrage (604/608 octets)

```
heroaudio=3 heroquantum=500 debug=0xc keepsyms=1 -noprogress rd=ufsroot0
ufsroot=USERDATA ufsrw=1 rootrw=1 cs_enforcement_disable=1
unentitled_ios_sim_launch=1 ignition_level=1 launchd_unsecure_cache=1
wdt=-1 -wdt_no_pmu wdt_test=no_user_checkin a5fb_keep_on=1 a5fb_noconsole=1
a5fb_perf=1 a5fb_hz=60 sweetfb_native=1 sweetfb_scale=1 sweetfb_lw=750
sweetfb_lh=1334 sweetfb_gpu=1 cpus=8 herogl=1 sweettop=1
vm_compression_limit=98304 vm_compressor=2
heronoexec=vmd,bluetoothd,searchd,ReportCrash,aggregated,analyticsd,cloudd,homed,driverkitd
glsb3=1 herogpu_mhz=650 herocpu_mhz=2288 herosb=1 sweetsample=-
```

Arguments utiles, tous lus par `PE_parse_boot_argn` :

| Argument | Effet |
|---|---|
| `herocpu_mhz=1976\|2288\|2600` | fréquence Mongoose (table dans `herolte_gpu.c`) |
| `herogpu_mhz=338…650` | fréquence G3D |
| `herogpu_flush=0\|1\|2\|3` | nettoyage du cache L2 autour des jobs (**voir §10.3**) |
| `herogpu_sleep_us=N` | granularité d'attente du GPU (défaut 50) |
| `sweetfb_lw/lh` | résolution logique de rendu |
| `sweetfb_cahz=N` | fréquence annoncée à CoreAnimation (défaut 120) |
| `sweetfb_gpu=1` | présentation par le Mali (blit plein écran) |
| `a5fb_perf=1` | active les statistiques de frame dans le journal |
| `herogl=1` | insère le shim GL dans backboardd |
| `herosb=1` | shim SpringBoard (relève les limites de cadence à 60) |
| `glwait=0` | ne pas attendre la frame dans le shim |
| `glsync=0`, `glcopy=1`, `glblit=0` | variantes de synchronisation (tests) |

**La ligne est pleine.** Pour ajouter un argument, il faut en retirer un de longueur
équivalente, ou modifier le défaut dans le code.

---

## 10. Le pilote GPU (`bsd/dev/herolte_gpu.c`)

~3 500 lignes. C'est la pièce la plus délicate du port.

### 10.1 Ce qu'il fait

Il programme le job manager Mali directement : écrit `HEAD_NEXT`, `AFFINITY`,
`JOB_CONTROL` (avec les bits de flush), puis `START`, et **attend la fin en
pollant** `JOB_IRQ_RAWSTAT`.

```c
uint32_t herolte_gpu_submit(unsigned js, uint64_t jc, uint64_t *us);
```
`js` = 0 (fragment) ou 1 (vertex/tiler). `jc` = adresse GPU de la chaîne de jobs,
fournie par l'appelant (Mesa). L'appel est **synchrone et tient un verrou**.

### 10.2 Pourquoi c'est synchrone

La chaîne de jobs est réutilisée par l'appelant. Soumettre le job suivant avant
que le précédent soit fini écraserait la chaîne en cours de lecture. C'est la
raison structurelle : **on ne peut pas pipeline sans dupliquer les chaînes**, ce
qui demande des fences côté DRM. C'est le chantier de fond (§13).

### 10.3 Les modes de flush — attention, danger

```c
hg_flush_mode : 0 = avant et après chaque job (comme Linux)
                1 = seulement après (défaut historique, CORRECT)
                2 = pas après les jobs de vertex   → ÉCRAN NOIR
                3 = pas après les jobs de fragment → ARTEFACTS
```

**Les modes 2 et 3 ont été testés et sont cassés.** Le nettoyage du cache L2 après
chaque job est **indispensable** : c'est lui qui rend les pixels visibles à
l'écran. Ne pas réessayer sans une raison très solide.

### 10.4 Le blit de présentation

`herolte_gpu_present_draw()` dessine un quad plein écran 750×1334 → 1440×2560.
Le sampler est réglé par `S_SAMPLER + 8 = 9 | (9<<4) | (9<<8)` (bilinéaire).

**Ne pas toucher à cet encodage sans la documentation Mali.** Une tentative de
passer en plus proche voisin (`1 | (1<<4) | (1<<8)`) a produit un **bug visuel
franc** : l'hypothèse « 4 bits = mode d'enveloppe + drapeau linéaire » est fausse.

Le blit coûte 3 à 4 ms par frame et c'est un **plancher** : 3,7 millions de pixels
à remplir, c'est la dalle.

### 10.5 Les interruptions

Le port **n'enregistre aucune interruption** : aucun `IOInterruptEventSource`,
aucun `ml_install_interrupt` dans les 267 patchs. Les `assert_wait_deadline` du
pilote GPU n'ont **aucun `wakeup()`** correspondant : ils ne se réveillent que par
expiration du délai.

Les numéros sont connus (SPI 224/225/226, lus dans l'arbre de périphériques) mais
le port n'a pas d'infrastructure pour les router.

---

## 11. La chaîne graphique — où part le temps

### 11.1 Les couches

```
App / SpringBoard
   └─ CoreAnimation
        └─ OpenGL ES
             └─ libhakiosgl.dylib   (le shim, gpu/hakios-gl/src/eagl.c)
                  └─ libOSMesa.8.dylib → Mesa → Panfrost
                       └─ libdrm.2.dylib → herodrm (userspace) → ioctl
                            └─ herolte_gpu.c (noyau) → Mali T880
```

### 11.2 Les mesures (à reproduire avant toute optimisation)

Le shim journalise, toutes les 20 frames :

```
hakios-gl: frame N: X GL calls, Y draws, Z triangles, C clears; …
hakios-gl: fN: draw <wrapper>/<mesa> us, attach A, flush F, wait W, copy C, frame T
```

Le framebuffer journalise toutes les 300 présentations :

```
A5XELTE FB: frames=… fps=… bytes=… avg_copy_us=… max_copy_us=…
            missed=… gaps=a/b/c/d swaps=… overwritten=… swapgap=a/b/c/d vbl=…
```

**`swapgap` est la seule mesure honnête de cadence** : c'est l'intervalle entre
deux swaps, classé en <12 ms / 12-20 ms (60 Hz) / 20-40 ms (30 Hz) / >40 ms (repos).
Le champ `fps=` est **trompeur** : il compte les appels de présentation, et le
timer vsync représente la même image en boucle (on a vu 183 « frames » pour
18 swaps réels).

### 11.3 Résultats mesurés

| Constante | Valeur |
|---|---|
| Attachement des IOSurfaces | **0 µs** — zéro-copie, 22 attaches en 1000 frames |
| `glFlush` | 16-30 µs |
| Attente GPU | 2 à 4 ms |
| Copie de sortie | 0,6 à 4,8 ms |
| **Frame typique (accueil)** | **4 à 10 ms** |
| **Frame avec clavier** | **19 à 66 ms** |
| Répartition du draw | **Mesa 85-90 %**, wrapper du shim 10-15 % |
| GPU occupé | **16 %** — il attend, il ne sature pas |
| Cadence réelle (défilement) | **64-67 % à 60 Hz, 27-29 % à 30 Hz** |

### 11.4 Conclusion sur les performances

Le rendu n'est pas le goulot : une frame d'accueil fait 4 à 10 ms pour un budget
de 16,7 ms. Ce qui coûte, c'est **le coût par appel GL** (~29 µs, dont l'essentiel
dans Mesa/Panfrost) multiplié par le nombre d'appels.

Le clavier dessine **150 fois par frame** (contre 33 pour l'accueil) : 150 × ~350 µs
= 52 ms. D'où le gel ressenti, qui n'est pas un blocage mais une file qui s'allonge.

**Pistes fermées par la mesure** (ne pas les rouvrir sans données nouvelles) :
- baisser la résolution de rendu (le coût est par appel, pas par pixel) ;
- supprimer les flushs L2 (écran noir / artefacts) ;
- changer le filtre du blit (bug visuel) ;
- optimiser la copie de surface (elle est à zéro) ;
- le `glFinish` redondant du shim (supprimé, gain marginal).

**Piste ouverte** : le coût par draw dans Panfrost. Il faut un profileur, ou
patcher Mesa via `gpu/mesa/patch-mesa.py` (qui fait des remplacements de texte
avec assertion — une erreur fait échouer le build proprement).

---

## 12. Chantier ouvert : les touches capacitives

Objectif : les deux touches rétroéclairées du S7 (gauche = multitâche, droite =
retour), inactives sous iOS.

### Ce qui est déjà établi (lu dans l'arbre de périphériques sous TWRP)

```
i2c@20 : compatible = "i2c-gpio"        ← bus BIT-BANGÉ, pas un contrôleur HSI2C
         gpios = <phandle 0x5a pin 2>, <phandle 0x5a pin 3>   (SDA, SCL)
         i2c-gpio,delay-us présent
         enfants : touchkey@20 (mc96ft16xx) et touchkey@40 (cypress_touchkey)
```

Linux les expose en `/sys/bus/i2c/devices/10-0020` et `10-0040`.

**Fausses pistes écartées** :
- `gpio_keys` ne contient que les boutons **physiques** (POWER, VOLUMEDOWN,
  VOLUMEUP, HOMEPAGE) — pas les capacitives ;
- le bus du tactile principal est différent (0x48 sur HSI2C `0x14E10000`) ;
- il n'y a **pas** de nœud `touchkey` sur les bus HSI2C `0x136x0000`.

**Résolu le 15 novembre 2026** (arbre vivant sous TWRP + `pinctrl-exynos.c`
d'`android_kernel_samsung_universal8890`) :

| Élément | Valeur |
|---|---|
| phandle `0x5a` | `pinctrl@14CC0000/gpe5` (banc GPE5, 8 pins) |
| Base du banc | **`0x14CC0000 + 0x20`** — table PERIC1 : gpe0 `0x00`, **gpe5 `0x20`**, gpe6 `0x40`, gpj1 `0x60`… |
| Registres | CON `+0x00`, DAT `+0x04`, PUD `+0x08` |
| SDA | **GPE5-2** |
| SCL | **GPE5-3** |
| IRQ | **GPA0-0**, actif bas (`pinctrl@10580000`, CON `0x00`, DAT `0x04`) |
| Contrôleur monté | **ABOV MC96FT16xx** à `0x20` (sub-det GPA3-0 = 0 ; TWRP : `touchkey_get_chip_vendor=ABOV`, fw `0x0f`) |
| Registres ABOV | `0x00` BTNSTATUS (bits 1:0 récent, 3:2 retour ; 2=appui, 1/3=relâche), `0x01` fw, `0x02` PCB, `0x14` modèle |
| Alimentation | S2MPB02 **LDO16** (`TOUCH_VDD_3.3V`), `boot-on-ldo`, laissée allumée par S-Boot |

⚠️ **L'ordre des pins est inversé par rapport à la note d'origine** : le
pilote `i2c-gpio` de Samsung lit le **premier** `gpios` comme **SDA**
(`i2c-gpio.c` : `*sda_pin = of_get_gpio(np, 0)`), et les propriétés enfants
`abov,sda-gpio`/`abov,scl-gpio` le confirment. Ne pas suivre le binding
mainline (SCL d'abord) : le bus ne répondrait pas.

Le pilote de l'étape 1 est `bsd/dev/herolte_touchkey.c` (patch `0347`) : il
muxe GPE5-2/3 en GPIO, bit-bang à ~160 kHz, lit `0x00`/`0x01`/`0x02`/`0x14`
et journalise les appuis. Il ne pilote que les deux lignes du bus ; aucun
geste, aucun registre du contrôleur n'est écrit, pas de rétroéclairage.

Image de l'étape 1 : `build/xnu-boot-tk2.img`, sha256
`0297057c136e18aaff3dd6584b8e110654e77a94ffca3b2e0abce41b0b79f296`
(build `xnu` 43725950220, 11 min 18 s, aucun patch sauté). Vérifiée par
`herolte-verify-boot.py` : 0 fallos.

### Étape 1 — résultat mesuré (15 novembre 2026)

Premier boot (patch `0347`, lecture seule) : `gpe5 con=0x220000 dat=0xff
pud=0x5005`, `gpa0 dat=0x3f`, **aucun ACK** à `0x20`, IRQ jamais assertée.
Les broches et le mux étaient corrects ; le contrôleur n'était pas alimenté.

Le `TOUCH_VDD_3.3V` est bien **S2MPB02 LDO16** sur HSI2C9 `0x13680000`
(GPD1-4/5, fonction 2). Le noyau XNU n'a pas de régulateur : la LDO était
restée **coupée** (`L16CTRL=0x6c`, 3,3 V mais bit 7 à 0). Le patch `0348`
lit l'ID du S2MPB02, puis `0x2d`, et écrit `0xec` si besoin.

Deuxième boot : `ldo16=0x6c` → `0xec` relu, et le contrôleur répond :

```
gauche : raw=0x2 (appui) puis 0x1 (relâche)
droite : raw=0x8 (appui) puis 0x4 (relâche)
IRQ GPA0-0 : passe à 0 sur l'événement, revient à 1 après lecture
```

Les valeurs `0x40`/`0x80` sont des drapeaux internes du MC96FT16xx (le
pilote Linux les ignore) ; seuls les bits 1:0 et 3:2 portent les touches.
L'ID (`0x01`/`0x02`/`0x14`) est encore lu `0000` au premier essai : le
chip démarre après l'activation de la LDO, il faut **200 ms + retries**
avant la première lecture. Le reste de la chaîne est validé.

**Étape 1 terminée.** Le contrôleur est lu, les appuis sont journalisés,
aucun geste n'est injecté.

### Étape 2/4 — injection et rétroéclairage (patch `0349`)

- **Gauche** (`rec=P`) : deux appuis Home à 80 ms d'écart (down, 40 ms, up,
  40 ms, down, 40 ms, up) → multitâche iOS.
- **Droite** (`back=P`) : swipe du numériseur virtuel, x 2 → 320 à y 667,
  ~120 ms (down à 2, pas de 32 toutes les 12 ms, up à 320).
- **LED** : `CMD_LED_ON` (`0x10`) écrit sur `BTNSTATUS` `0x00` à chaque
  appui, `CMD_LED_OFF` (`0x20`) 3 s après le dernier.
- **Front d'appui strict** : seuls les bits 0-3 sont suivis, et l'action
  part sur la transition vers `2` (appui), jamais sur la relâche ni sur la
  relecture qui suit la désassertion de l'IRQ. Depuis `0352`, il faut **deux
  échantillons propres identiques à 20 ms** (le `0x82` parasite est ignoré)
  et la garde est passée à **500 ms**.
- **ID** : 200 ms après la LDO, 3 essais par registre, cycle LDO off/on en
  dernier recours.

Image : `build/xnu-boot-tk3.img`, sha256
`09d5231c9030468d535377b2366f3e0d93fa9bd70b1afe7b09a505e6d5fa2321`
(build `xnu` 43727793429, 12 min 58 s). **Testé et validé** : multitâche à
gauche, retour à droite, LED 3 s. Le correctif `0352` (image `fix1`) filtre
ensuite le faux appui `0x82` et passe la garde à 500 ms.

### Plan de travail initial (conservé pour l'historique)

1. **Étape 1 — lire, ne rien injecter.** I2C bit-bangé sur les deux broches,
   lecture du registre d'état, journalisation des appuis. Valide le matériel sans
   risque pour l'interface.
2. **Étape 2 — injecter un appui simple.** Traduire la touche gauche en double
   appui Home (deux événements espacés de ~80 ms) pour ouvrir le multitâche.
3. **Étape 3 — le geste de retour.** Glissement du bord gauche vers le centre
   dans le numériseur virtuel (x: 2 → 320, y: 667, ~120 ms).
4. **Étape 4 — le rétroéclairage.** Régulateur LED 3,3 V pendant 3 s après un
   toucher, coupé à l'extinction de l'écran.

L'injection HID existe déjà dans le port (le bouton Home en envoie), et le
bit-bang évite d'écrire un pilote de contrôleur HSI2C.

---

## 13. Chantier de fond : la soumission asynchrone

C'est le seul levier de performance restant qui ne sacrifie ni la stabilité ni
l'image.

**Le problème** : `herolte_gpu_submit()` bloque jusqu'à la fin du job. Avec 58 à
150 jobs par frame, le CPU passe son temps à attendre un GPU occupé à 16 %.

**Pourquoi ce n'est pas trivial** : la chaîne de jobs est réutilisée par
l'appelant ; il faut soit la dupliquer, soit des fences côté DRM pour savoir quand
un buffer est réutilisable. Le port a des « syncobjs » mais ils sont toujours
signalés, précisément parce que la soumission est synchrone.

**Ce que ça rapporterait** : recouvrir 4 ms d'attente par frame, soit exactement
la marge qui manque pour tenir 60 Hz en permanence.

---

## 14. Déboguer

### Lire le journal

```bash
python3 tools/herolte-log-pull.py adb build/boot.txt
```

Le journal est **persistant** : il survit aux redémarrages et se lit depuis TWRP.
Taille 4 Mio, en anneau (les vieilles lignes disparaissent).

⚠️ **Le writer tronque les lignes longues.** Une ligne de log utile doit rester
courte, sinon le chiffre intéressant est coupé. C'est pour ça que les mesures sont
sur des lignes séparées et brèves.

### Chercher dans le journal

```bash
grep -a "A5XELTE FB: frames=" boot.txt | tail -5      # cadence et copies
grep -a "submits, GPU" boot.txt | tail -3             # occupation GPU
grep -a "HEROLTE DROP" boot.txt                       # quelle génération est installée
grep -a "hakios-gl: f" boot.txt | tail -5             # répartition par frame
grep -a -i "fault\|timeout\|failed" boot.txt          # erreurs
```

### Les repères de démarrage sain

```
HEROLTE GPU: … mali-t880 …
HEROLTE GL: backboardd launch 1: EAGL shim inserted
SWEET FB: AppleCLCD 1440x2560 …
HEROLTE DROP: package N ready for userland
HEROLTE CPU: Mongoose at 2600 MHz
HEROLTE TK: id fw=0xf pcb=… model=… st=… 1111
```

Les quatre `1` de la ligne `HEROLTE TK: id` sont les ACK des lectures
`0x01`/`0x02`/`0x14`/`0x00`. Un `0` signifie que le contrôleur n'a pas
acquitté : vérifier d'abord l'alimentation (S2MPB02 LDO16, `TOUCH_VDD_3.3V`,
`regulator-boot-on`), puis SDA/SCL (GPE5-2/GPE5-3, pas l'inverse).

---

## 15. Les pièges, avec leurs symptômes

| Symptôme | Cause | Solution |
|---|---|---|
| La nouvelle bibliothèque ne change rien | `--generation` non incrémenté | toujours `--generation N` |
| Toujours une version de retard | shim chargé avant l'installeur | **deux** redémarrages |
| Écran noir | flush L2 supprimé (mode 2) | rester en mode 1 |
| Artefacts après quelques minutes | flush L2 partiel (mode 3) | rester en mode 1 |
| Bug visuel franc sur le blit | encodage du sampler Mali | ne pas y toucher |
| `invalid literal for int()` au drop | démon adb qui redémarre | relancer |
| Build « échoué » mais noyau produit | upload d'artefact en timeout | `gh run rerun --failed` |
| `fps=` incohérent avec le ressenti | compte les présentations, pas les swaps | lire `swapgap` |
| adb perd le téléphone | écran TWRP en veille | réveiller TWRP |
| Écran qui s'assombrit au lieu de s'éteindre | fondu en 7 étapes | corrigé (noir immédiat) |

---

## 16. Références rapides

```bash
# Construire
gh workflow run xnu.yml -R <repo> --ref <branche> \
  -f machine_config=HEROLTE -f kernel_config=DEVELOPMENT -f macos_version=13.5

# Suivre
gh run list -R <repo> --limit 3
gh run view <id> -R <repo> --json status,conclusion

# Image
python3 tools/mkpayload.py <kernel> build/herolte-3g.adt build/payload.bin \
  --cmdline "$(cat build/cmdline-2288.txt)"
python3 tools/herolte-pack-boot.py pack backup/boot-stock.img \
  shim/herolte/out/xnu_boot.Image build/xnu-boot.img \
  --append build/payload.bin --partition-size 41943040
python3 tools/herolte-verify-boot.py build/xnu-boot.img

# Flash (TWRP)
adb push build/xnu-boot.img /tmp/b.img
adb shell "dd if=/tmp/b.img of=/dev/block/sda5 bs=1048576 conv=fsync"

# Journal (TWRP)
python3 tools/herolte-log-pull.py adb build/boot.txt
```

---

## 17. Conseils à la prochaine IA

1. **Mesurer avant de modifier.** Ce projet a consommé plusieurs cycles sur des
   hypothèses fausses (attachement des surfaces, résolution, filtre du blit) que
   la mesure a démenties.
2. **Toujours vérifier le hash après un flash.** Une écriture partielle ne se voit
   pas tout de suite.
3. **Ne jamais supposer qu'une bibliothèque est chargée.** Vérifier
   `package N` dans le journal, et se souvenir des deux redémarrages.
4. **Les patchs sont appliqués dans l'ordre alphabétique.** Un patch qui en modifie
   un autre doit avoir un numéro supérieur. Vérifier avec
   `git apply --check` sur une reconstruction complète avant de pousser.
5. **Le port n'a pas d'interruptions.** Tout ce qui attend se réveille par timeout.
6. **La ligne de commande est pleine.** Vérifier `wc -c` avant de packager.
7. **Sauvegarder l'image stable** avant chaque expérience risquée.

---

## 18. Veille et consommation (pwr1)

### Ce qui existait déjà

- `SweetFramebuffer.cpp`, `kSweetFBRequestPowerChange` : noir en un tick,
  timer vsync ralenti à 5 Hz, `herolte_screen_off_at` posé.
- `herolte_gpu.c`, `hg_power_thread` : après 3 s écran éteint, Mongoose
  revient aux valeurs du bootloader (728 MHz, 675 mV) ; il remonte à
  2 600 MHz au rallumage. Les A53 ne bougent pas.
- **Aucun envoi DCS** : le DSIM `0x13900000` n'était que sondé, et
  `SweetFramebuffer` se contentait de noircir la frame.

### Ce que pwr1 ajoute (patch `0350`)

- `herolte_panel_power(bool on)`, appelé sur un vrai changement d'état.
- **Sleep** : DCS `0x28`, 20 ms, `0x10`, 50 ms, GPU → 338 MHz (PLL puis
  tension).
- **Wake** : GPU → 650 MHz (tension puis PLL), réveil du thread de
  puissance sur son canal dédié pour Mongoose avant la première frame,
  DCS `0x11`, 120 ms, `0x29`, `fDirty` forcé.
- `hg_clock_set` devient `hg_gpu_rate` (table 338…650) ; le boot reste à
  650 MHz.
- Journal : `HEROLTE PWR: sleep cpu=low(3s) gpu=338 dsi=28/10` et
  `HEROLTE PWR: wake cpu=high gpu=650 dsi=11/29`.

### DSIM 8890 (Samsung `decon_8890`)

- `PKTHDR 0x3c`, `FIFOCTRL 0x4c` bit 10 = header FIFO empty,
  `INTSRC 0x34` bit 28 = PH FIFO empty, `CMD_CONFIG 0x78`.
- DCS short write : `data_id 0x05`, `data0` = commande, en-tête =
  `(cmd << 8) | 0x05`.

### Image

`build/xnu-boot-pwr1.img`, sha256
`8efaa5de88391b7aafab030d5b12b55dbfd63c6944fab2491b501e9a63994304`
(build `xnu` 43733817607, 19 min 53 s, aucun patch sauté ; vérifiée par
`herolte-verify-boot.py` : 0 fallos).

### Test

Écran allumé → Power : le journal doit montrer `sleep`, le GPU à 338, puis
`Mongoose to 728 MHz (screen off…)` après 3 s. Power : `wake`, GPU 650,
Mongoose 2 600 avant la première frame. Vérifier l'absence d'artefact et
le réveil instantané.

---

## 19. Batterie, coupures et profils (pwr2, patch `0351`)

### Paliers et tensions relevés

| CPU Mongoose | vdd_mngs | registre |
|---|---|---|
| 728 MHz (bootloader) | 675 mV | état boot |
| **1 976 MHz** | **912,5 mV** | `0x62` |
| 2 288 MHz | 993,75 mV | `0x6f` |
| **2 600 MHz** | **1 075 mV** | `0x7c` |

| GPU Mali | vdd_g3d | registre |
|---|---|---|
| **338 MHz** | **600 mV** | `0x30` |
| 390 / 419 MHz | 612,5 mV | `0x32` |
| **546 MHz** | **662,5 mV** | `0x3a` |
| 600 MHz | 681,25 mV | `0x3d` |
| **650 MHz** | **700 mV** | `0x40` |

### Source d'alimentation

- Pas de pilote chargeur : `herolte_fg_read()` (MAX77854 à `0x36`) donne
  `mA`, positif en charge. `herolte_battery.c` publie le dernier
  `herolte_fg_mv`/`herolte_fg_ma` (un seul utilisateur I2C).
- `hg_power_thread` : `ma > 20` = secteur, `ma < -20` = batterie, sinon
  garde l'état (hystérésis, pour une batterie pleine sur secteur).
- `vbat` = moyenne de 5 échantillons **distincts** (la jauge met à jour
  environ une fois par seconde).

### Profils

| État | CPU Mongoose | GPU Mali | idle → 728 |
|---|---|---|---|
| Secteur | 2 600 MHz | 650 MHz | 3 s |
| Batterie | **1 976 MHz** | **546 MHz** | **1,8 s** |
| Batterie < 3 550 mV | 1 976 MHz | **419 MHz** | 1,8 s |
| Écran off / SoC chaud | 728 MHz | 338 MHz | — |

### Code

- `hc_go_rate()` : n'importe quel palier de `hc_rates[]` ; en montée la
  tension puis EMA/diviseurs/switch/PLL, en descente l'inverse.
- `hc_go_low()` : état bootloader exact (728 MHz, 675 mV).
- `hg_gpu_cap` : le réveil DCS utilise le plafond courant, pas 650 fixe.
- Logs : `HEROLTE PWR: src=bat|ac cap_cpu=… cap_gpu=… vbat=…mV` à chaque
  changement, et la ligne 60 s `HEROLTE POWER` porte src/vbat/cap.

### Image

`build/xnu-boot-pwr2.img`, sha256
`4dd0d8d100cd6255cc1adfb2aeeb24ed858a230a8646429a9529ea020e9f978c`
(build `xnu` 43736632527, 15 min 41 s, aucun patch sauté ; vérifiée par
`herolte-verify-boot.py` : 0 fallos).

### Chantier 1

**Terminé et validé sur banc** : pwr1 (DCS `0x28`/`0x10`/`0x11`/`0x29`,
GPU 338/650) + pwr2 (plafonds batterie, anti-UVLO, jauge lissée), image
`xnu-boot-pwr2.img` vérifiée à 0 fallos. La validation sur téléphone
(flash + cycles secteur/batterie) reste la dernière étape.

---

## 20. Correctif multitâche fantôme et charge (patch `0352`)

- **Multitâche fantôme** : le MC96FT16xx renvoie parfois `0x82` (appui
  récent + bit 7) après la commande LED ; le pilote y voyait un appui et
  injectait le double Home. Il exige maintenant **deux échantillons propres
  identiques** à 20 ms (nibble haut à 0), un échantillon sale remet le
  compteur à zéro, et la garde passe à 500 ms.
- **Charge non détectée** : `SweetBattery` ne regardait que `ma > 20`, donc
  un chargeur faible (courant net faible) paraissait débranché. Le pilote
  batterie lit maintenant le MFD MAX77854 à `0x66` sur le même HSI2C1 :
  `CHG_INT_OK` (`0xB2`), bit 6 `CHGIN_OK`. `SweetBattery` publie
  `external`/`charging` depuis ce bit et le thread de puissance l'utilise
  pour les plafonds ; repli sur `ma` si la lecture échoue.
- **Log** : `HEROLTE PWR: src=ac|bat ac_raw=0x.. cap_cpu=… cap_gpu=…
  vbat=…mV`.
- **Image** : `build/xnu-boot-fix1.img`, sha256
  `c67cfdfb2f9acf368818927abe0dedb5aba5164a994f83bfff206d0db195b941`
  (build 43739381894, 10 min 35 s, 0 fallos).

---

## 21. Chargeur temps réel et Bluetooth BCM4359 (bt1, patches `0353`/`0354`)

### Mission 0 — le débranchement

- `herolte_fg_read` lit le chargeur **avant** la jauge : `CHG_INT` `0xB0`
  (read-clear), `CHG_INT_OK` `0xB2` bit 6 `CHGIN_OK`, `CHG_DETAILS_00`
  `0xB3` bits 6:5 `CHGIN_DTLS`. Les deux doivent être vrais ; tout échec
  repasse en batterie. Aucun latch ne survit au débranchement.
- `SweetBattery` et `hg_power_thread` utilisent cet état live ; log
  `HEROLTE PWR: plug=0|1 vbat=…mV ac_raw=… dtls=… cap_cpu=… cap_gpu=…`.

### Bluetooth, Phase 1 (matériel)

- **UART1 `0x14C20000`**, pas `0x13630000` : le nœud `uart@14C20000` est
  `okay` avec `pinctrl-names = "btdefault"/"btsleep"` ; `uart@13630000`
  est `disabled`. Broches **GPE0-0..3**, fonction 2, PUD 3, DRV 3.
- **BT_REG_ON** = **GPJ1-7**, PERIC1 `0x14CC0000` + `0x60`.
- **BT_WAKE** = **GPI2-3**, FSYS0 `0x10E60000` + `0x20`.
- **BT_HOST_WAKE** = **GPA2-3**, ALIVE `0x10580000` + `0x40`, entrée.
- Firmware **`bcm4359C0_semco.hcd`** (59 702 octets, sha256
  `97d839cb1a31bb2de8736817bb7fc7e9c4c944938000960bf2c710d3839df332`)
  depuis VENDOR, dans `drop/bt/bcm4359.hcd` → drop zone
  `/private/var/herolte/bt/bcm4359.hcd`.
- Driver `bsd/dev/herolte_bt.c` : 8N1 115200 RTS/CTS, HCI H4
  (`01 03 0C 00`, `01 2E FC 00`, `.hcd`, reset), BD_ADDR, version locale,
  puis polling RX 5/20 ms.

### Drop

- `drop/` est **gitignoré** : le firmware reste local, comme celui du
  Wi-Fi.
- `python3 tools/herolte-drop.py adb --generation 17 …`
  `bt/bcm4359.hcd=drop/bt/bcm4359.hcd` ; **deux redémarrages** après la
  drop.

### Image bt1

`build/xnu-boot-bt1.img`, sha256
`b89f9cadc184abd678b7aaf8eb289639f24b9735d1049cf1c5823f7c5c4569c0`
(build `xnu` 43743961070, 14 min 04 s, aucun patch sauté ; 0 fallos).
Flashée sur `sda5`, relecture identique.

Drop **génération 17** écrite et vérifiée (package
`2593881052831856299`, 726 421 504 octets, 22 472 entrées) avec
`bt/bcm4359.hcd` (59 702 octets) et les trois apps (PolySpeak, YouTube,
Discord). **Deux redémarrages** pour l'installer.

---

## 22. Journal complet de la session — 15 novembre 2026

### 22.1 Tout ce qui a été construit, dans l'ordre

| # | Chantier | Patch | Build `xnu` | Image | sha256 (début) | Résultat |
|---|---|---|---|---|---|---|
| 1 | Touches, lecture | `0347` | 43723723649 | `tk1` | `ac9e96a4` | broches justes, **aucun ACK** : LDO16 coupée |
| 2 | Touches, alimentation | `0348` | 43725950220 | `tk2` | `0297057c` | `LDO16 0x6c → 0xec`, touches `0x02/0x01/0x08/0x04` |
| 3 | Touches, injection | `0349` | 43727793429 | `tk3` | `09d5231c` | multitâche à gauche, retour à droite, LED 3 s : **validés** |
| 4 | Veille OLED + GPU | `0350` | 43733817607 | `pwr1` | `8efaa5de` | DCS `0x28/0x10/0x11/0x29`, GPU 338/650 |
| 5 | Batterie, plafonds | `0351` | 43736632527 | `pwr2` | `4dd0d8d1` | 1 976/546 MHz batterie, 1 976/419 sous 3 550 mV |
| 6 | Fantôme + charge | `0352` | 43739381894 | `fix1` | `c67cfdfb` | `0x82` filtré, `CHGIN_OK` lu ; la charge restait `ac` au débranchement |
| 7 | Charge live | `0353` | 43743961070 | `bt1` | `b89f9cad` | `CHG_INT` + `CHGIN_OK` + `CHGIN_DTLS` live, aucun latch |
| 8 | Bluetooth matériel | `0354` | 43743961070 | `bt1` | `b89f9cad` | UART1 `0x14C20000`, GPIO, HCI H4, `.hcd` |

### 22.2 Les mesures qui ont compté

- **tk1** : `gpe5 con=0x220000 dat=0xff pud=0x5005`, `gpa0 dat=0x3f`,
  `id … 0000`, `no ACK at 0x20 sda=1 scl=1`. Le mux et les broches étaient
  bons ; le contrôleur n'était pas alimenté.
- **tk2** : `ldo16=0x6c` (3,3 V, bit 7 à 0) → `0xec` relu ; le MC96FT16xx
  répond alors : gauche `0x02`/`0x01`, droite `0x08`/`0x04`, IRQ GPA0-0
  à 0. Les `0x40`/`0x80` sont des drapeaux internes.
- **tk3** : les gestes et la LED sont validés à l'usage.
- **pwr1/pwr2** : les transitions veille/réveil et les plafonds batterie
  sont en place ; l'usage a révélé deux régressions (voir ci-dessous).
- **fix1** : le faux appui `0x82` ouvrait le multitâche tout seul ; la
  lecture `CHGIN_OK` marchait au branchement mais restait bloquée à `ac`
  au débranchement.
- **bt1** : image flashée, drop 17 vérifiée ; les logs `HEROLTE BT` et le
  test branchement/débranchement restent à faire.

### 22.3 Images et sauvegardes

| Image | sha256 | État |
|---|---|---|
| `build/SAFE-xnu-boot-tk3.img` | `09d5231c…` | sauvegarde touches validées |
| `build/SAFE-xnu-boot-pwr1.img` | `8efaa5de…` | sauvegarde veille |
| `build/SAFE-xnu-boot-pwr2.img` | `4dd0d8d1…` | sauvegarde plafonds batterie |
| `build/SAFE-xnu-boot-fix1.img` | `c67cfdfb…` | sauvegarde fantôme/charge |
| **`build/xnu-boot-bt1.img`** | **`b89f9cad…`** | **courante, flashée** |

### 22.4 Index des patches

| Patch | Fichiers | Rôle |
|---|---|---|
| `0347-herolte-touchkey-read` | `herolte_touchkey.c` (nouveau), `conf/files`, `IOKitBSDInit.cpp` | lecture ABOV, bit-bang GPE5-2/3 |
| `0348-herolte-touchkey-power` | `herolte_touchkey.c` | S2MPB02 LDO16, HSI2C9 |
| `0349-herolte-touchkey-inject` | `herolte_touchkey.c` | double Home, swipe retour, LED |
| `0350-herolte-power-sleep` | `herolte_gpu.c`, `SweetFramebuffer.cpp` | DCS OLED, GPU 338/650, réveil CPU |
| `0351-herolte-battery-brownout-guard` | `herolte_gpu.c`, `herolte_battery.c` | plafonds batterie, `hc_go_rate` |
| `0352-herolte-ghost-touch-and-charger` | `herolte_touchkey.c`, `herolte_battery.c`, `herolte_gpu.c`, `SweetBattery.cpp` | filtre `0x82`, `CHGIN_OK` |
| `0353-herolte-plug-live` | `herolte_battery.c`, `herolte_gpu.c`, `SweetBattery.cpp` | charge live `0xB0/0xB2/0xB3` |
| `0354-herolte-bt-hw` | `herolte_bt.c` (nouveau), `conf/files`, `IOKitBSDInit.cpp` | BCM4359 UART1/GPIO/HCI/.hcd |

### 22.5 État exact à la passation

- Image courante : **`bt1`** (`b89f9cad…`), flashée sur `sda5`, relecture
  identique.
- Drop **génération 17** vérifiée : 726 421 504 octets, 22 472 entrées,
  `bt/bcm4359.hcd` 59 702 octets, apps PolySpeak/YouTube/Discord.
- Premier boot effectué ; le **second boot**, le test branchement/
  débranchement et la lecture des logs `HEROLTE BT`/`HEROLTE PWR` restent
  à faire.
- Le correctif charge live (`0353`) est **dans** `bt1`.
- La Phase 2 Bluetooth (`bluetoothd`) n'est **pas** commencée.

### 22.6 Prochaines étapes

1. Second boot, test câble, retour TWRP : vérifier
   `HEROLTE BT: reset ok=1`, `fw bytes=59702`, `bdaddr=…`, et
   `HEROLTE PWR: plug=1` → `plug=0`.
2. Si `reset ok=0` : activer l'horloge CMU de l'UART1 (le log
   `uart as found` dira si les registres sont muets). Si `fw bytes=0` :
   vérifier `HEROLTE DROP: package 17`.
3. Phase 2 : exposer `IOBluetoothHCIController`/`IOSerialBSDClient` et
   `/dev/herobt` (H4 complet), retirer `bluetoothd` de `heronoexec` en
   restant sous **608 octets**, puis tester l'interrupteur
   Réglages → Bluetooth et le scan.

---

*Fin du document. Il est délibérément exhaustif : son but est qu'aucune des
vingt heures de mesure de cette session ne soit à refaire.*

---

## 23. INCIDENT : le thème Liquid Glass a gelé le téléphone — RÈGLE ABSOLUE

### 23.1 Règle absolue (à ne jamais contourner)

> **Ne plus JAMAIS modifier ni hooker SpringBoard — ni aucune classe privée
> UIKit/CoreAnimation — depuis un .dylib userspace.** C'est interdit : pas de
> thèmes visuels, pas de retouches du Dock, de l'App Switcher, du Control
> Center, de la barre d'état, des coins arrondis, pas de `UIVisualEffectView`.
> **L'interface native d'iOS 16 reste strictement d'origine.**
>
> Raison : SpringBoard est le processus dont l'échec emporte toute l'interface.
> Le 16 novembre 2026, un thème injecté via `gpu/eagl/src/herosb.c` a fait
> boucler le téléphone sur `xnu mmu on` puis l'a figé écran noir (PMIC bloqué),
> nécessitant une décharge complète de la batterie pour réarmer le S2MPS16.

### 23.2 Chronologie exacte de l'incident (16 novembre 2026)

1. `df48eaa` ajoute à `herosb.c` un thème « Liquid Glass » : coins arrondis
   (Dock 28 pt, cartes 26 pt, panneaux 20 pt), liseré, ombre, et pourcentage
   de batterie — hooks sur `SBDockView`, `SBAppSwitcherPageView`,
   `_UIStatusBarBatteryView` (classes privées SpringBoard/UIKit).
2. Le code compile (vérifié avec clang + stubs, 0 erreur) et passe le build
   CI `mesa-ios` (run 43898573151).
3. Drop zone **génération 18** écrite sur USERDATA et **relue depuis le
   disque** : `HERODRP2`, proto 2, gen 18, 23 entrées, 17 968 680 octets,
   bloc 6195311, dans la zone drop de 1 Gio (marge restante 257 755 blocs —
   **aucun débordement**).
4. Au premier reboot : le téléphone affiche `xnu mmu on` en boucle puis
   s'éteint sur un écran noir. Les boutons (Vol bas + Home + Power) ne
   répondent plus : PMIC en boucle watchdog. Décharge batterie obligatoire
   (~23 h) pour réarmer.

### 23.3 Faits établis (à distinguer des hypothèses)

| Fait | Valeur |
|---|---|
| Le noyau BOOT n'a jamais été réécrit cette session | `bt9` = `ff4ec978254765e5…`, intact |
| La drop 18 n'a pas débordé | marge 257 755 blocs sur 262 144 |
| Le code a compilé | clang 0 erreur, CI vert |
| **Mais il a gelé le téléphone** | boucle `xnu mmu on` → écran noir → PMIC |
| Le code n'a pas été testé sur l'appareil avant le flash | **la seule règle qui compte** |

### 23.4 Cause

Le constructeur de `libherosb.dylib` a ajouté un parcours en profondeur de
toutes les vues de chaque fenêtre SpringBoard, avec `method_setImplementation`
sur des classes privées non documentées, exécuté au chargement de la lib dans
SpringBoard (pid 27). Les messages sont envoyés par `objc_msgSend` casté, sans
aucune protection contre une classe inattendue ; le bouclage `xnu mmu on` et
le gel viennent probablement d'un hook sur une classe privée dont l'ABI ne
correspond pas à la signature supposée. Le constructeur peut aussi tourner
avant la fin de l'init d'UIKit. **Quelle que soit la cause exacte, le verdict
est définitif : ne plus hooker SpringBoard.**

### 23.5 Ce qui a été fait après l'incident

- `86e9dc4` : **revert** de `df48eaa` — `herosb.c` est revenu à sa version
  saine de la génération 17.
- `M/lib/libherosb.dylib` : restauré au build gen 17 (`bf913b0d923d…`,
  69 584 o), vérifié par sha256.
- `build/drop-19-recover.sh` : bundle de récupération prêt — écrit la
  génération 19 avec le `libherosb` sain + `glass.off` vide (double garde),
  et **refuse d'écrire** si le sha du binaire n'est pas `bf913b0d…` (testé à
  blanc : il a refusé le mauvais binaire).
- `docs/HANDOVER-COMPLET.md` : cette note.

### 23.6 Restauration du téléphone (prête, ~10 min une fois en TWRP)

1. Téléphone en TWRP (après décharge + recharge), câble branché, écran allumé.
2. Vérifier que BOOT n'a pas bougé :
   ```
   adb shell "dd if=/dev/block/sda5 bs=1M count=40 2>/dev/null | sha256sum"
   # doit commencer par ff4ec978254765e5 (bt9). Sinon reflasher
   # build/SAFE-xnu-boot-bt9.img depuis TWRP.
   ```
3. Écrire la drop de récupération (uniquement USERDATA) :
   ```
   bash build/drop-19-recover.sh     # gen 19, libherosb sain + glass.off
   ```
4. **Deux redémarrages** : boot 1 installe, boot 2 charge la bibliothèque saine.
5. Confirmer dans le journal (`herolte-log-pull.py` depuis TWRP) :
   `HEROLTE DROP: package N ready for userland` (gen 19) et
   `HEROLTE GL: SpringBoard launch 1 (pid N): libherosb inserted`.

---

## 24. Clôture de la phase (16 novembre 2026)

Le développement est **en pause jusqu'aux vacances de décembre 2026**. Le
téléphone est repassé sous Android pour l'usage quotidien. Tout ce qui suit est
en état stable, rien ne doit être « fini en urgence ».

### 24.1 État livré

- Branche `herolte-full-advances`, prête pour une Pull Request.
- L'arbre est propre : aucun fichier modifié non commité, `build/` et `M/` hors
  suivi Git, aucun secret (le mot de passe Wi-Fi vit dans `drop/`, ignoré).
- Le thème Liquid Glass est annulé proprement (`86e9dc4`) : `git diff df48eaa^
  HEAD -- gpu/eagl/src/herosb.c` est vide, le fichier est identique à l'état sain.
- Les 277 patchs de `xnu/patches/` ont tous un en-tête diff et au moins un hunk ;
  aucun n'est appliqué hors séquence. Les numéros manquants sont intentionnels :
  `patches-pending/` porte les expérimentaux A5XELTE jamais validés (`0112`,
  `0113`, `0115`, `0116`, `0129`–`0134`), et `0106`/`0107` ont été réécrits en
  `0201`/`0202` pour valoir sur les deux machines.
- Quatre scripts sont sortis de `build/` (ignoré) vers `tools/` :
  `herolte-drop-package.sh`, `herolte-drop-recover.sh`, `herolte-diag-readonly.sh`,
  `herolte-reflash-bt9.sh`.

### 24.2 Ce que la reprise doit savoir

Le téléphone n'est pas cassé : il a seulement gelé sur un thème SpringBoard, et
le PMIC s'est réarmé après décharge complète. La reprise se fait en trois temps :
flasher `build/SAFE-xnu-boot-bt9.img` sur `sda5` si nécessaire, écrire un drop
avec `tools/herolte-drop-package.sh`, redémarrer deux fois. **Ne pas** réécrire
`glass.off` ni réintroduire de thème.

### 24.3 Ce qui reste ouvert (détail en §19 à §22 et dans HEROLTE-PROGRESS.md)

1. **Bluetooth BCM4359** — matériel entièrement cartographié et piloté
   (UART1 `0x14C20000`, GPE0-0..3, GPJ1-7, GPI2-3, GPA2-3, firmware
   `bcm4359C0_semco.hcd` de 59 702 octets chargée). Le handshake HCI (reset +
   téléchargement du `.hcd`) n'est pas prouvé.
2. **USB périphérique DWC3** — le bloc répond (`GSNPSID 0x5533280a`) mais
   `SETEPCONFIG` sur ep0 ne lève jamais `CMDACT`.
3. **Interruptions** — aucune `IOInterruptEventSource` dans tout le port :
   tout est en scrutation. C'est le plus gros levier de performance restant.
4. Modem, caméra, capteurs, Metal.

---

*Fin du document (mis à jour le 16 novembre 2026 : clôture de la phase, thème annulé, branche de Pull Request prête).*
