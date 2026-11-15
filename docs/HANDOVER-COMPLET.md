# Passation complète — iOS 16 sur Galaxy S7 (herolte, Exynos 8890)

Document destiné à une IA ou un développeur qui reprend le projet **de zéro**.
Il contient : ce qu'est le projet, comment il se construit, comment il s'installe,
comment il se mesure, ce qui marche, ce qui ne marche pas, et **tous les pièges
rencontrés** avec leurs symptômes exacts.

Dernière mise à jour : 15 novembre 2026, fin de session.

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
**267 patchs noyau** et de plusieurs bibliothèques userspace.

Le port fonctionne : il démarre, affiche SpringBoard, lance des applications
(iTunes Store, YouTube, Discord), se connecte au Wi-Fi, joue du son.

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
- **Extinction d'écran instantanée** (noir en un tick, plus de fondu).
- CPU Mongoose à **2 600 MHz**, GPU à 650 MHz.
- Trois Gio de RAM utilisables.

## Ce qui ne marche pas / limites connues

- **Le clavier saccade** : il dessine 150 fois par frame, chaque draw coûtant
  ~300 µs côté Mesa. Voir §11.
- **Pas d'interruptions** : tout le noyau est en polling (§11.4).
- USB non fonctionnel (endpoint 0 répond mais la configuration échoue).
- Pas de DVFS : fréquences fixes.
- Les touches capacitives latérales sont **inactives** dans l'interface
  (chantier ouvert, §12) ; le contrôleur est désormais **lu et journalisé**
  (étape 1 validée), l'injection reste à faire.
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
  relecture qui suit la désassertion de l'IRQ. Garde de 300 ms par touche.
- **ID** : 200 ms après la LDO, 3 essais par registre, cycle LDO off/on en
  dernier recours.

Image : `build/xnu-boot-tk3.img`, sha256
`09d5231c9030468d535377b2366f3e0d93fa9bd70b1afe7b09a505e6d5fa2321`
(build `xnu` 43727793429, 12 min 58 s). Test sur téléphone : à faire.

### Plan de travail recommandé

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

*Fin du document. Il est délibérément exhaustif : son but est qu'aucune des
vingt heures de mesure de cette session ne soit à refaire.*
