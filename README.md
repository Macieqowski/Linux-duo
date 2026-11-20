# Linux-duo: Debian + KDE Plasma Mobile na Surface Duo 1

Debian 13 „trixie” (arm64) z KDE Plasma Mobile na **Microsoft Surface Duo 1** (Snapdragon 855 / SM8150,
nazwa kodowa „epsilon”), na **stabilnym jądrze mainline 7.3.5** z łatkami dla tego telefonu.

> **Stan: wczesna wersja, NIETESTOWANA na telefonie.** Wszystko tutaj jest zbudowane i skompilowane, ale nic nie
> zostało jeszcze uruchomione na Duo. Pierwszy start to test. Przeczytaj „Bezpieczeństwo” przed czymkolwiek.

Projekt bazuje na [linuxduobench](https://github.com/brysonwf/linuxduobench) (GPL-2.0-or-later): jego łatki
jądra, usługi sprzętowe i skrypty są w `tools/` i `docs-linuxduobench/`, przeniesione z jądra 6.19 na 7.3.5.
Commit źródłowy: `upstream/linuxduobench.commit`.

## Co ma działać

Stan z linuxduobench (jądro 6.19, jeden telefon), który ta wersja ma powtórzyć na 7.3.5:

| Funkcja | Stan w linuxduobench | Uwagi |
|---|---|---|
| Oba ekrany | działa | własny sterownik panelu, DSC |
| Dotyk (multi-touch) | działa | spi-hid-duo + iptsd (budowany w obrazie); obejście [zacinającego się gestu w KWin](#dotyk-przestaje-działać-zacięty-gest-w-kwin) |
| Pióro | nie | jest w danych iptsd, nieskończone |
| GPU (Adreno 640) | działa | Mesa freedreno/turnip z backports |
| Wi-Fi, Bluetooth | działa | ath10k / hci_qca |
| Dane komórkowe (LTE) | działa | modem + IPA v4.1 + ModemManager |
| **Połączenia, SMS** | **działa (2G)** | SMS i połączenia przez 2G z [obejściem modemu](#modem-obejście-awarii-quick-offline); [dźwięk rozmowy](#dźwięk-rozmowy-q6voice) w obie strony (słuchawka + mikrofon), przychodzące i wychodzące, z usługą `duo-call-audio`, [dzwonek z wibracją i budzeniem ekranu](#dzwonek-wibracja-i-budzenie-ekranu), głośnik i wyciszenie (sprawdzone, przyciski w dialerze pokazują swój stan), głośność rozmowy. Bez tłumienia echa |
| Głośnik, mikrofony | działa | WCD9340 + WSA8815, strojenie z ACDB telefonu. Na 7.3.5 głośnik milczał: [naprawione trasą w DT](#głośnik-cisza-na-735) |
| Słuchawka do rozmów | działa | wyjście EAR kodeka (`0076`–`0079`); w rozmowie gra przez DSP ([dźwięk rozmowy](#dźwięk-rozmowy-q6voice)), [ciszej niż głośnik](#słuchawka-do-rozmów-wyjście-ear) |
| Czujniki, zawias, obrót | działa | SLPI + hexagonrpcd + libssc (budowane w obrazie); [co działa w aplikacjach](#czujniki) |
| Aparat | częściowo | wymaga modułu imx351 zbudowanego z danych Twojego telefonu |
| Ładowanie, bateria | działa | qcom_smbx (SMB5) + 2x bq27742 |
| Usypianie | wyłączone | s2idle działa, ale telefon się nie budził; na razie bez usypiania |
| GPS | częściowo | pozycja z sieci tak, fix satelitarny niepotwierdzony; [pomoc XTRA](#gps) |
| Czytnik linii papilarnych | nie | siedzi w TrustZone |

## Jak to się uruchamia

- Jądro startuje **z RAM-u** poleceniem `fastboot boot` z komputera. Nic nie jest zapisywane na partycje startowe
  telefonu (`boot_*`, `vbmeta_*`, `dtbo_*`), więc wyłączenie telefonu = jego własny łańcuch startowy.
- Debian leży w katalogu `/debian` na partycji `userdata` (sformatowanej jako ext4).
- Jeśli Debiana tam nie ma, ten sam obraz startuje minimalną powłokę z SSH po USB (169.254.66.1), z której instaluje się system.

Wada: telefon trzeba uruchamiać z komputera po każdym wyłączeniu. To celowe: samodzielny start wymagałby zapisu
partycji startowych, a na Duo 1 to dokładnie ta klasa zmian, która zbrickowała telefon autorów linuxduobench.

## Bezpieczeństwo (Duo 1)

- **Nigdy nie blokuj bootloadera z powrotem** (`fastboot flashing lock` / `lock_critical`). Na Duo 1 nie ma
  publicznego narzędzia EDL do odratowania. Zablokowany zmodyfikowany telefon = cegła.
- Nie zapisuj `boot_*`, `vbmeta_*`, `dtbo_*` ani tablicy partycji. Nic w tym repo tego nie robi.
- Odblokowanie bootloadera (`fastboot flashing unlock`) kasuje dane Androida.
- Po instalacji nie uruchamiaj Androida normalnie: zobaczy obcy `userdata` i może zaproponować przywrócenie
  ustawień fabrycznych, a to skasuje Debiana.
- Przed `fastboot boot` zawsze `fastboot --set-active=a` (inaczej bootloader odmawia). Bootloader wraca na slot b po
  każdym restarcie (2026-11-20 trzy razy z rzędu), więc sprawdzaj `fastboot getvar current-slot` za każdym razem.
- Restart z Linuxa: `devmem 0x0c264000 w 0` (zwykły `reboot` zawiesza ten telefon).

## Modem: obejście awarii (quick offline)

Na jądrze mainline modem SM8150 pada z asercją `rflm_qlnk_ls_retry_cnt < 2` (łącze QLink między modemem a
nadajnikiem RF) przy wybudzeniu z najgłębszego snu „quick offline”: najczęściej przy szukaniu sieci, zawsze na 2G
i 3G, czasem sam w pierwszej minucie po starcie. Ten sam firmware działa na Androidzie. Przyczyny po stronie Linuksa
na razie nie znaleźliśmy (wykluczone m.in.: głosy RPMh na zegary rfclka/lnbclka w obie strony, memshare, szyny
S5A/S6A/S8C, głosy proxy przy PAS, bity 0x02/0x80 tego samego wpisu). Tej awarii nie rozwiązał też nikt w innych
projektach SM8150 (Poco X3 Pro, OnePlus 7T Pro).

**Obejście:** wpis EFS modemu `/nv/item_files/modem/utils/mcpm/mcpm_quick_offline_cfg` = `01` wyłącza tylko ten
tryb snu (wynika z kodu MCPM w firmware). Zwykły sen modemu działa dalej. Z wpisem działają LTE, 2G, SMS i
połączenia (CSFB do 2G; sieć Play nie ma już 3G).

**Gdzie jest zapisany:** Debian nie pisze do partycji modemu. `duo-rmtfs` (`tools/linux/system/libexec/duo-rmtfs`,
uruchamiany przez `rmtfs.service.d/duo-efs-copy.conf`) przy pierwszym starcie kopiuje partycje `modemst1`, `modemst2`,
`fsg` i `fsc` (tylko odczyt) do `/var/lib/duo-efs` i podaje modemowi tę kopię. To, co modem zapisze w EFS pod
Linuksem, zostaje w kopii. Android dalej używa swoich partycji, bez wpisu.

**Włączenie** (raz, na świeżo uruchomionym telefonie: po każdym restarcie modemu DIAG odrzuca komendy EFS, błąd
0x13, aż do restartu telefonu). Potrzebne `duo-efs` i `duo-efs-write` z `tools/linux/modem/` (statycznie dla
aarch64, np. `aarch64-linux-gnu-gcc -static -O2 -o duo-efs-write duo-efs-write.c`) i działający `diag-router`
(usługa `duo-diag`).

Bez wpisu modem często pada sam w pierwszej minucie po starcie, zanim zdąży się go zapisać. Dlatego na ten jeden
start wyłącz ModemManager (`systemctl mask ModemManager`, sync, restart telefonu): modem zostaje wtedy w trybie
niskiego poboru, nie szuka sieci i nie pada. Potem:

```sh
duo-efs-write mkdir /nv/item_files/modem/utils
duo-efs-write mkdir /nv/item_files/modem/utils/mcpm
duo-efs-write put /nv/item_files/modem/utils/mcpm/mcpm_quick_offline_cfg 01
duo-efs cat /nv/item_files/modem/utils/mcpm/mcpm_quick_offline_cfg     # ma pokazać 01
# czysty restart modemu: modem zapisuje EFS przy zatrzymaniu i czyta wpis przy starcie
M=$(echo /sys/bus/platform/devices/4080000.remoteproc/remoteproc/remoteproc*)
echo stop > $M/state; sleep 2; echo start > $M/state
systemctl unmask ModemManager; systemctl start ModemManager
sync
```

Na koniec `mkdir -p /etc/duo && touch /etc/duo/modem-all-rats`: bez tego `duo-modem` przy każdym starcie ustawia
modemowi samo LTE (zabezpieczenie sprzed obejścia), a wtedy połączenia przychodzące (CSFB do 2G) nie działają.

Nie restartuj modemu po jego awarii (wtedy zawiesza się TrustZone i telefon się resetuje); po awarii tylko reset
telefonu (power + głośność w dół).

**Cofnięcie:** `duo-efs-write rm /nv/item_files/modem/utils/mcpm/mcpm_quick_offline_cfg` i czysty restart modemu,
albo usunięcie `/var/lib/duo-efs` (przy następnym starcie powstanie nowa kopia z partycji), albo
`touch /etc/duo/efs-partitions` (stary tryb: partycje tylko do odczytu, zmiany EFS tylko w RAM do restartu).

## Głośnik: cisza na 7.3.5

Objaw: mikrofony działają, głośnik milczy, a przy starcie w `dmesg` są błędy
`wsa881x ... ASoC error (-16)` dla rejestrów 0x311c (RDAC) i 0x3103 (Bandgap).

Przyczyna: w węźle `sound` nie było trasy do wejścia wzmacniacza `SpkrLeft IN`. Ścieżka DAPM wzmacniacza
(IN → RDAC → SPKR PGA → SPKR) nie ma wtedy podłączonego wejścia i jest włączana tylko raz, przy podłączeniu karty
dźwiękowej. Sterownik karty (`snd_soc_sdm845`) jest modułem i ładuje się ok. 7 s po wzmacniaczu, który po 3 s
usypia (runtime PM). Zapisy do uśpionego wzmacniacza kończą się błędem -EBUSY, DAPM i tak uznaje je za wykonane
i już ich nie powtarza, więc przetwornik DAC wzmacniacza zostaje wyłączony (0x311c = 0x42 zamiast 0xc2).
Na 6.19 (linuxduobench) działało tylko dzięki innej kolejności ładowania.

Poprawka: patch `0073` dodaje do `audio-routing` trasę `"SpkrLeft IN", "SPK1 OUT"` (jak na db845c). Wtedy
wzmacniacz włącza się razem z odtwarzaniem, gdy jest już wybudzony. Te same dwa błędy -16 zostają przy starcie, ale
to już tylko odczyty stanu przy tworzeniu karty: przy graniu 0x311c = 0xc2.

Głośność: sekwencja włączenia wzmacniacza w jądrze ustawiała przy każdym starcie dźwięku +12 dB, więc ustawione
w UCM `PA Volume` 12 (+18 dB) działało tylko do pierwszego zamknięcia PCM. Patch `0074` pamięta to ustawienie.
Tor jest czysty do ok. +20 dB w `RX7 Digital Volume` (zmierzone mikrofonem telefonu, tonem -3 dBFS), wyżej trzeszczy.
Tryb DRE wzmacniacza (jak w sterowniku Androida, parametr `snd_soc_wsa881x.dre=1`) jest o ok. 4,5 dB cichszy, więc
domyślnie jest wyłączony. Duo 1 ma jeden głośnik do mediów (lewa połowa) i słuchawkę do rozmów (prawa).

Nie przeładowuj modułu karty (`modprobe -r snd_soc_sdm845`) jako obejścia: psuje port AFE w DSP
(`SLIMBUS_0_RX` start -22) aż do restartu telefonu. Nie czytaj też `/sys/kernel/debug/soundwire/*/registers` ani
zbiorczo `/sys/kernel/debug/regmap/*/registers`: odczyt rejestrów kontrolera zegarów zresetował telefon.

## Słuchawka do rozmów (wyjście EAR)

Słuchawka (górna krawędź prawej połowy) wisi na wyjściu EAR kodeka WCD9340, tak jak w Androidzie (ścieżka `handset`
w `mixer_paths_tavil.xml`: `RX INT0_1 MIX1 INP0` = `RX0`, `RX INT0 DEM MUX` = `CLSH_DSM_OUT`, `EAR PA Gain` +6 dB,
w jądrze głównym `EAR PA Volume` 4).

- Gdy grała sama słuchawka, kodek nie miał zegara MCLK: w `dmesg` było `overflow error on RX port 0` i cisza. Grało
  tylko wtedy, gdy zegar trzymał akurat tor głośnika. Patch `0076` dodaje trasę `"RX_BIAS", "MCLK"` (jak w DT
  Microsoftu i na db845c). Zegar kodeka jest przez to włączony cały czas.
- Patche `0077` i `0079` ustawiają przy starcie te same wartości, co sterownik Androida (`tavil_codec_reg_defaults`,
  `tavil_codec_reg_init_common_val`, `tavil_codec_reg_init_1_1_val`). `0079` oznacza też rejestry stanu jako ulotne,
  więc debugfs pokazuje ich prawdziwą wartość.
- Patch `0078` poprawia błąd w `wcd-clsh-v2`: prąd bufora VNEG był zapisywany jako 0 zamiast 0xA0.

Po tych zmianach 672 rejestry analogowe i toru RX0 w trakcie grania na słuchawce mają te same wartości, co ustawia
Android. Sterownik kodeka w Androidzie Duo to standardowy kod Qualcomma (Microsoft zmienił tylko czas startu
mikrofonu). Słuchawka gra czysto i liniowo do pełnej skali, ale przy tym samym sygnale jest o ok. 20 dB cichsza od
głośnika (głośnik +18 dB, słuchawka +6 dB). W rozmowie Android dodaje jeszcze przetwarzanie mowy w DSP, więc
docelową głośność trzeba ustawić na torze głosowym.

Przy testach wyjść: nie zmieniaj tras `RX INTx_1 MIX1 INP0`, gdy strumień jest otwarty, bo tor milknie aż do
ponownego otwarcia. Najpierw `pactl suspend-sink <sink> 1`, potem mikser, potem `pactl suspend-sink <sink> 0`.
Wyjścia LINEOUT i HPH (Duo nie ma gniazda słuchawek) na razie kończą się błędem overflow. Po takiej próbie głośnik
może milczeć mimo dobrego miksera: pomaga jeszcze jedno uśpienie i wybudzenie sinka.

## Dźwięk rozmowy (q6voice)

Dźwięk połączenia nie przechodzi przez PipeWire: modem i DSP audio (ADSP) mieszają go same. Linux otwiera tylko PCM
`VoiceMMode1` (w obu kierunkach) i trzyma go otwartego, a ADSP łączy wtedy mikrofony (`SLIMBUS_0_TX`) przez obróbkę
mowy (vocproc) z modemem i modem ze słuchawką (`SLIMBUS_0_RX` -> EAR). Sterownik to seria „hack!” q6voice Stephana
Gerholda z dodatkami dla CVD 2.3 (`0056`–`0072`). Żeby na SM8150 dźwięk przechodził w obie strony, potrzebne były
jeszcze poprawki `0080`–`0084` (2026-11-20, sprawdzone w 13 rozmowach testowych):

- `0081`, `0082`: tak jak sterownik Androida, q6voice dołącza do strumienia głosowego modemu (pasywna sesja CVS)
  i przed startem rozmowy wyłącza jego wyciszenie (`VSS_IVOLUME_CMD_MUTE_V2`) w obu kierunkach. Bez tego rozmówca
  nie słyszał nic z mikrofonu Duo, mimo że wszystkie polecenia DSP kończyły się sukcesem.
- `0083`: mapa kanałów słuchawki FL, FR zamiast FL, FL (z FL, FL na słuchawce była cisza).
- `0084`: `qcom,cvd-v2.3` w DT i łącze `VoiceMMode1` na końcu karty, więc numery PCM się nie przesuwają
  (0 MultiMedia1, 2 MultiMedia2 = mikrofon w UCM, 4 VoiceMMode1).

Topologia obróbki mikrofonu to `NONE` (0x10F70, kontrolka `VoiceMMode1 TX Topology`): domyślna `SM_ECNS` bez
kalibracji z ACDB wycisza mikrofon, a dwa kanały mikrofonu (fluence) DSP odrzuca. Echo i szumy nie są więc na razie
tłumione.

Rozmowy włącza usługa `duo-call-audio` (`tools/linux/system`), bo ModemManager nie steruje dźwiękiem, a `callaudiod`
nie znajduje w UCM profilu do rozmów. Usługa ładuje moduły q6voice (karta dźwiękowa czeka na nie, gdy DT ma łącze
głosowe), śledzi połączenia w ModemManagerze i:

- przy dzwonieniu (przychodzącym albo wybieraniu numeru) dokłada w kodeku tor słuchawki, tor głośnika zostaje (dzwonek);
- przy odebraniu, a przy wychodzącym już przy sygnale dzwonienia, wycisza tor głośnika (`RX7` na -84 dB, trasa
  zostaje dla trybu głośnomówiącego) i od razu otwiera PCM głosowy;
- po rozłączeniu zamyka PCM i przywraca głośnik. Pierwszy obiekt połączenia przychodzącego w ModemManagerze często
  znika po 1–3 s i pojawia się nowy, dlatego usługa czeka 6 s, zanim uzna nieodebrane połączenie za skończone.

W rozmowie usługa bierze mikrofon u dołu prawej połówki (`DMIC MUX7` = `DMIC1`, ten sam co Android) z `DEC7 Volume` 98
(+14 dB, najlepszy z poziomów 66–114 w dwóch rozmowach testowych). `DMIC2` to mikrofon u góry lewej połówki. `DMIC3`
czyta drugą krawędź tej samej linii danych co `DMIC2`, więc daje ten sam mikrofon (korelacja 0,99), a `DMIC0`, `DMIC4`
i `DMIC5` dają cyfrowe zero (wcześniejszy komentarz w UCM „DMIC1 nie jest podłączony” był błędny). Nagrywanie poza
rozmową: [mikrofony](#mikrofony-nagrywanie-i-dec8).

Kodek trzeba przełączyć na słuchawkę chwilę przed startem toru głosowego. Gdy przełączenie było tuż przed startem
(w `dmesg`: `overflow error on RX port 0`), słuchawka milczała aż do ponownego otwarcia PCM, przy dowolnym odstępie
od odebrania. Dlatego usługa przełącza kodek już przy dzwonieniu; dźwięk jest ok. 1,5 s po odebraniu. Ustawienia
(głośność słuchawki, mikrofon i jego czułość, start przy sygnale dzwonienia) są w `/etc/default/duo-call-audio`, opis
w nagłówku skryptu. Nie przeładowuj modułów q6voice w działającym systemie: karta się przebindowuje i porty AFE
nie działają aż do restartu telefonu. Komunikaty kodeka `overflow error on RX port 0` / `TX port 8` przy
przełączaniu tras poza startem toru głosowego są nieszkodliwe.

Telefon testowy ma jądro zbudowane przed serią `0080`–`0084`, a moduły q6voice z tymi poprawkami ładuje z
`/lib/modules/$(uname -r)/updates/q6voice` (po `depmod`). Do 2026-11-20 były to moduły w wersji diagnostycznej z
`/etc/modprobe.d/duo-q6voice.conf` (`options q6voice cvs_unmute=1 tx_topo_override=0x10f70`,
`options q6cvp fix_rx_map=1`). Od 16:17 tego dnia są tam moduły zbudowane z serii, bez tego pliku, a jądro ma już
`0085`. Jądro z tego repo nie potrzebuje żadnego z tych dodatków.

Przyciski rozmowy w Plasma Dialer: dialer wysyła je do `callaudiod`, który nie ma profilu rozmowy dla tej karty.
`duo-call-audio` śledzi więc sygnały `speakerModeChanged` i `muteChanged` usługi `org.kde.telephony.DialerUtils`
(kde-telephony-daemon, szyna sesji) i sam ustawia kodek. Plasma Dialer 6.3.5 nie zna jednak stanu swoich przycisków:
wtyczka QML przekazuje te sygnały bez argumentu, więc strona rozmowy (`CallPage.qml`: „Cannot assign [undefined] to
bool”) nigdy nie zaznacza przycisku i każde naciśnięcie wysyła `setSpeakerMode(true)` / `setMute(true)`. Drugie
naciśnięcie niczego w demonie nie zmieniało (sprawdzone w zapisie szyny D-Bus z rozmowy testowej). Usługa śledzi
więc też same wywołania i `true` przy już włączonym stanie traktuje jako drugie naciśnięcie: wysyła do demona
`false` (w dzienniku „speaker button pressed again: off”).

Ikony i podświetlenie przycisków (od 2026-11-20): `CallPage.qml` czyta teraz stan z właściwości `DialerUtils.mute` i
`DialerUtils.speakerMode` (wtyczka pyta o nie kde-telephony-daemon) zamiast z argumentu sygnału, a przy starcie każdej
rozmowy ustawia oba przyciski według stanu demona. Strona jest zasobem rcc wewnątrz programu `/usr/bin/plasma-dialer`,
a przebudowa dialera wymaga całego środowiska KF6, więc `tools/linux/debian/dialer/duo-dialer-callpage-patch.py`
podmienia tylko ten jeden zasób w kopii programu: znajduje skompresowany blok (zstd albo zlib), zmienia tekst,
kompresuje go z powrotem i wpisuje w to samo miejsce (nowy blok nie może być dłuższy od starego). Reszta programu się
nie zmienia. Obraz systemu robi to przy budowaniu (`--install`, oryginał w `/var/lib/duo/plasma-dialer.orig`), a hak apt
`99-duo-dialer-patch` powtarza to po aktualizacji pakietu. Na telefonie (Plasma Dialer 1:6.3.5-1): blok 1799 -> 1781
bajtów. Napis „Mute” zostaje ten sam, bo dialer go nie zmienia. Pierwsza próba nie zadziałała: dialer dalej używał
starej strony z pamięci podręcznej QML (`~/.cache/KDE/dialer/qmlcache`). Qt sprawdza ją dla plików qrc tylko po dacie
zasobu, a poprawka tej daty nie zmienia. `--install` kasuje więc tę pamięć we wszystkich katalogach domowych. Na
telefonie przeniesiona ręcznie 2026-11-20 o 17:52. Sprawdzone w rozmowie tego dnia (ok. 18:10): Głośnik i Mute się
podświetlają, ikona mikrofonu się zmienia, wyciszenie działa (Maciej: „działa super”).

Wyciszenie nie działało (rozmowy testowe 2026-11-20 o 17:07 i 17:46). Zapis dotyku, okien i D-Bus z drugiej rozmowy:
dotknięcia trafiały w przycisk Mute aktywnego okna dialera, a dialer za każdym razem wysyłał `setMute(true)`.
kde-telephony-daemon miał jednak wyciszenie zapamiętane jako włączone (od restartów telefonu), więc nic się nie
zmieniało: bez `muteChanged` i bez `MuteMic`, a `duo-call-audio` reaguje na sygnał. Głośnik działał, bo jego stan w
demonie był wyłączony. Poprawka w `duo-call-audio`: oba przyciski są ustawiane na wyłączone w demonie na początku
(`preroute`) i zawsze na końcu rozmowy, a 0,4 s po każdym naciśnięciu usługa czyta stan demona (`mute`,
`speakerMode`) i idzie za nim, gdy sygnał nie przyszedł (w dzienniku „following the daemon”). Blokowanie KWin, dialera
albo iio-sensor-proxy przez czujniki sprawdzone wcześniej i wykluczone (odpowiedzi poniżej 55 ms).

- Głośnik: w rozmowie obie trasy zostają podłączone (słuchawka `RX INT0`, głośnik `RX INT7` -> `SpkrLeft`), a
  przełączenie zmienia tylko głośności cyfrowe (`RX0`/`RX7 Digital Volume`, 0 = -84 dB). Zmiana trasy przy otwartym
  PCM głosowym mogłaby wyciszyć tor do ponownego otwarcia, zmiana głośności nie. Gdyby mimo to tor milkł,
  `DUO_CALL_SWITCH_REOPEN=1` otwiera PCM głosowy ponownie po przełączeniu (ok. 3 s przerwy).
- Wyciszenie: `DEC7 Volume` na minimum.
- Głośność: w rozmowie głośność sinka głośnika (suwak, klawisze głośności) jest głośnością rozmowy. Rozmowa zaczyna
  się z głośnością poprzedniej (`/var/lib/duo/call-volume`, domyślnie 80 %), a po rozmowie wraca głośność
  multimediów. 80 % daje słuchawce `RX0` 84 (0 dB), a głośnikowi `RX7` 94 (+10 dB); zależność jest sześcienna jak w
  PipeWire (100 % = +6 dB, 50 % = -12 dB).
- Każda rozmowa zaczyna się na słuchawce i z włączonym mikrofonem: po rozmowie usługa wyłącza oba przyciski w
  dialerze.

Ustawienia (`/etc/default/duo-call-audio`): `DUO_CALL_SPK_VOLUME`, `DUO_CALL_SPK_DMIC`, `DUO_CALL_SPK_MIC_VOLUME`,
`DUO_CALL_VOLUME`, `DUO_CALL_SWITCH_REOPEN`. Echo nie jest tłumione (topologia `NONE`), więc na głośniku rozmówca
może słyszeć siebie. Sprawdzone w rozmowie 2026-11-20: Głośnik włącza i wyłącza głośnik (powrót na słuchawkę),
Wyciszenie wycisza i włącza mikrofon. W pierwszej rozmowie testowej nie działał żaden dotyk
([zacięty gest w KWin](#dotyk-przestaje-działać-zacięty-gest-w-kwin)).

Czujnik zbliżeniowy: w rozmowie na słuchawce (nie na głośniku) twarz przy czujniku obok słuchawki gasi oba ekrany i
wyłącza ich dotyk, jak w Androidzie, więc policzek nie naciska przycisków rozmowy. Po odsunięciu telefonu ekrany
wracają tak, jak każe zawias (`duo-posture`: oba, sam prawy albo żaden przy zamkniętym). `duo-call-audio` gasi je
podświetleniem (`bl_power`) i urządzeniami wejścia KWin, tak jak `duo-posture`, nigdy przez DPMS. Czujnik czyta
`monitor-sensor --proximity` (iio-sensor-proxy) tylko w czasie rozmowy. Póki twarz jest przy czujniku, istnieje
`/run/duo-call-near` i `duo-posture` przy zmianie ułożenia tylko zapamiętuje stan. Po odsunięciu usługa prosi też
`duo-wake --touch-reset` o wyczyszczenie stanu dotyku w KWin (policzek mógł dotykać ekranu, gdy dotyk się wyłączał;
patrz [zacięty gest](#dotyk-przestaje-działać-zacięty-gest-w-kwin)). Wyłączenie: `DUO_CALL_PROXIMITY=0`. Test w
rozmowie: w toku.

Do zrobienia: tłumienie echa i szumów, VoLTE. Dzwonek i wibracja: [niżej](#dzwonek-wibracja-i-budzenie-ekranu).

## Mikrofony: nagrywanie i DEC8

Profil HiFi w UCM nagrywa dwa kanały: lewy z `DMIC1` (dół prawej połówki, ten sam co w rozmowie) przez `DEC7`, prawy z
`DMIC2` (góra lewej połówki) przez `DEC8`. Wcześniej brał `DMIC3` + `DMIC2`, czyli dwa razy ten sam górny mikrofon.

Kanał `DEC8` był o 20 dB cichszy przez błąd w sterowniku kodeka (także w czystym 7.3.5). Kontrolka `ADC MUX8` wskazuje
w `wcd934x.c` pole rejestru `ADC MUX10` (`ADC_MUX1_CFG1` zamiast `ADC_MUX0_CFG1`, jak w sterowniku dostawcy). Przez to
bit 7 rejestru `CDC_TX8_TX_PATH_CFG0` (wybór mikrofonu analogowego albo cyfrowego) nigdy nie jest ustawiany i `DEC8`
przetwarza mikrofon cyfrowy w trybie analogowym. Pomiar 2026-11-20: ten sam mikrofon na `DEC7` i `DEC8` naraz
(`arecord`, S16, korelacja 0,99) dał na `DEC8` 22,5 dB mniej. Patch `0085` poprawia pole. Sprawdzone na telefonie z
jądrem z `0085` (2026-11-20, 16:20): ten sam mikrofon na `DEC7` i `DEC8` dał na `DEC8` 2,3 dB więcej (korelacja 0,96)
zamiast 22,5 dB mniej, a komunikaty „overflow error on TX port 8” zniknęły. Ton z głośnika nagrany przez PipeWire był
czysty na obu kanałach (49 i 47 dB nad sąsiednimi pasmami). `DMIC2` (góra lewej połówki) słyszy głośnik ok. 19 dB
głośniej niż `DMIC1`, bo leży bliżej niego.

W trybie cyfrowym dekymator jest „gorący”: przy `DEC Volume` 84 (0 dB) cichy pokój dawał -53 dBFS, a ton 1500 Hz z
głośnika telefonu się przesterowywał. Przy 62 ton był czysty (-21 dBFS przy tle -73 dBFS), przy 72 też (-11 dBFS).
UCM ustawia więc oba kanały na 72 (-12 dB). Na rozmowę to nie wpływa, bo `duo-call-audio` ustawia `DEC7` po swojemu.

Uszkodzony kanał po ponownym otwarciu strumienia: 2026-11-20 o 14:52 PipeWire otworzył nagrywanie na nowo (w `dmesg`
„underflow error on TX port 7” tuż przed startem) i od tej chwili kanał `DEC7` dawał sam szum z przesterowaniem: ton z
głośnika był tylko 11 dB nad tłem, a na `DEC8` 51 dB. To ten sam port, którego używa mikrofon w rozmowie. Przełączenie
profilu karty na `off` i z powrotem na `HiFi` (`pactl set-card-profile alsa_card.platform-sound off`, potem `HiFi`)
naprawiło kanał (ton czysty, 52 dB nad tłem). Ten sam stan był po uruchomieniu telefonu 2026-11-20 o 16:17: strumień
otwarty przy starcie sesji nie nagrywał tonu na żadnym kanale, po przełączeniu profilu oba były czyste. Przyczyna
jest jeszcze nieznana. Gdyby w rozmowie zamiast głosu był szum, pomoże to samo przełączenie profilu.

Dlatego od 2026-11-20 przełączenie robi się samo raz po zalogowaniu: `duo-volume-nudge` (autostart XDG), gdy pojawi
się głośnik i minie 5 s, wywołuje `duo-audio-reopen`. Ten przełącza profil karty na `off` i z powrotem na `HiFi`
(tylko gdy był `HiFi`), przywraca domyślne wyjście i wejście, jeśli się zmieniły, i zapisuje w dzienniku
„microphone stream reopened”. Dopiero potem idzie poprawka suwaka głośności. Wyłączenie: plik
`/etc/duo/audio-reopen-off`. Sprawdzone w dwóch uruchomieniach telefonu (16:30 i 16:40): jedna linia w dzienniku
ok. 43 s po starcie, domyślne urządzenia bez zmian, głośność 100 %, ton z głośnika czysty na obu kanałach (50 i 44 dB
nad sąsiednimi pasmami), bez żadnego ręcznego kroku.

`pactl suspend-source` nie zwalnia mikrofonu, bo plasmashell (plasma-pa) cały czas trzyma na nim i na monitorze
głośnika strumienie mierników głośności („PlasmaPA-VolumeMeter”). Przez to nagrywanie i odtwarzanie przez PipeWire
działa bez przerwy, także przy wygaszonych ekranach, co kosztuje baterię. Do zbadania.

## Dzwonek, wibracja i budzenie ekranu

Plasma Dialer przy połączeniu przychodzącym tylko raz odtwarza dźwięk powiadomienia, nie wibruje i nie włącza
wygaszonych ekranów. Dlatego od sygnału dzwonienia do odebrania, odrzucenia albo końca połączenia robi to
`duo-call-audio`:

- budzi ekrany (`duo-wake`, niżej);
- odtwarza dzwonek w pętli jako użytkownik sesji (`paplay`,
  `/usr/share/sounds/ocean/stereo/phone-incoming-call.oga`);
- wibruje 1 s co 2 s przez `hfd-service` (`com.lomiri.hfd.Vibrator`, efekt `FF_RUMBLE`).

Gdy obiekt połączenia w ModemManagerze znika, dzwonek gra jeszcze 2,5 s, bo zwykle od razu pojawia się nowy. Obraz
systemu wyłącza dźwięk zdarzenia `ringing` w `~/.config/plasma-dialer.notifyrc` (okienko zostaje), żeby dzwonek nie
grał podwójnie. Ustawienia w `/etc/default/duo-call-audio`: `DUO_CALL_RINGTONE` (plik dzwonka, pusty = bez dzwonka),
`DUO_CALL_VIBRATE=0` (bez wibracji), `DUO_CALL_WAKE=0` (bez budzenia), `DUO_CALL_RAISE=0` (bez wyciągania dialera).

Dialer w tle: gdy Plasma Dialer już działa w tle, przy połączeniu przygotowuje stronę rozmowy, ale jego okno zostaje
z tyłu, także po odebraniu z powiadomienia. Na Waylandzie program w tle może wyciągnąć swoje okno tylko z ważnym
tokenem aktywacji, a KWin blokuje go jako kradzież fokusu (plasmashell: „Got invalid activation app_id:
org.kde.plasma.dialer.desktop”). Dlatego `duo-call-audio` przy dzwonieniu i po starcie rozmowy wyciąga okno dialera
jednorazowym skryptem KWin przez D-Bus (`org.kde.kwin.Scripting`, teraz i jeszcze raz po 1,5 s, bo dialer
uruchomiony dla rozmowy pokazuje okno później). Test w rozmowie: w toku.

Budzenie: KWin włącza wyłączone ekrany (DPMS, po bezczynności albo przyciskiem zasilania) przy naciśnięciu dowolnego
klawisza, a `org.freedesktop.ScreenSaver.SimulateUserActivity` ich nie włącza (sprawdzone). `duo-wake` naciska więc
raz klawisz `KEY_WAKEUP` na klawiaturze uinput, która ma tylko ten klawisz (KWin nie traktuje jej jak sprzętowej
klawiatury). Nowe urządzenie uinput KWin widzi dopiero po ok. 1 s, co opóźniało zapalanie ekranów przy otwieraniu
telefonu, dlatego usługa `duo-wake` trzyma tę klawiaturę cały czas, a `duo-call-audio` i `duo-posture` proszą ją o
budzenie przez `/run/duo-wake.sock` (bez usługi `duo-wake` tworzy klawiaturę na chwilę, jak wcześniej). Ekrany budzą
się:

- przy połączeniu przychodzącym (`duo-call-audio`);
- przy powiadomieniu o zwykłej albo krytycznej pilności: usługa `duo-wake` śledzi
  `org.freedesktop.Notifications.Notify` na szynie sesji, budzi najwyżej raz na 3 s;
  wyłączenie: `touch /etc/duo/wake-notify-off`;
- przy otwarciu telefonu albo zmianie ułożenia (`duo-posture`, tryb jednego lub dwóch ekranów).

Gdy telefon jest zamknięty (`/run/duo-posture.state`), połączenia i powiadomienia nie budzą ekranów. Sprawdzone na
telefonie 2026-11-20: połączenie przychodzące przy wygaszonym ekranie (dzwonek, wibracja, ekran się włącza),
powiadomienie (ekrany włączone po ok. 1 s) i otwarcie zablokowanego telefonu.

## Dotyk przestaje działać (zacięty gest w KWin)

Objaw (2026-11-20, po rozmowie i zamknięciu telefonu): ekrany świecą, ale żaden dotyk nic nie robi, ani na ekranie
blokady, ani po odblokowaniu. Podwójne stuknięcie przy wygaszonych ekranach nadal je zapala. Dotyk dociera do KWin
(iptsd i oba ekrany dotykowe działają, żaden palec nie wisi), ale KWin nie przekazuje go dalej.

Przyczyna, w kodzie KWin 6.3.6 (`src/input.cpp`, `GlobalShortcutFilter`): filtr gestów ekranu dotykowego (3 i więcej
palców) przy takim geście bierze dla siebie każdy następny dotyk i każdą ramkę dotyku (`touchFrame`). Bez ramek
żaden program Qt nie reaguje na dotyk, także ekran blokady. Swoje punkty filtr zapomina dopiero po ich podniesieniu
albo po anulowaniu dotyku. Gdy palce, które zaczęły gest, zostaną podniesione przy wygaszonych ekranach, podniesienia
zjada stojący przed nim filtr DPMS i gest zostaje „w toku” na zawsze. Tak może się zdarzyć przy zamykaniu telefonu, gdy
ekrany dotykają się nawzajem, albo gdy dłoń trzymająca telefon dotyka ekranu przy naciskaniu zasilania.
Odblokowanie, zablokowanie, wyłączenie i włączenie urządzeń dotyku nie pomagają.

Doraźnie: dotknij ekranu trzema palcami naraz i je podnieś (numery punktów, które filtr pamięta, się zwalniają).

Poprawka: usługa `duo-wake` ma własny mały ekran dotykowy uinput (`duo-touch-reset`, jeden punkt). Za każdym razem,
gdy ekrany się zapalą, kładzie na nim palec na środku ekranu i zamienia go w dłoń (`MT_TOOL_PALM`). libinput wysyła
wtedy anulowanie dotyku, KWin anuluje wszystkie dotyki i filtr gestów zaczyna od zera. Robi to dwa razy, bo zacięty
filtr zatrzymuje pierwsze anulowanie u siebie. Program pod tym punktem dostaje tylko anulowany dotyk. Usługa czeka,
aż żaden prawdziwy palec nie dotyka ekranów, i pisze w dzienniku „touch reset (screens on)”. Ręcznie:
`duo-wake --touch-reset`, wyłączenie: `touch /etc/duo/touch-reset-off`.

Sprawdzone na telefonie 2026-11-20:
- reset po zapaleniu ekranów przyciskiem, bez przypadkowych kliknięć;
- odtworzone zacięcie bez udziału człowieka, z wyłączonym resetem: 3 palce testowego urządzenia uinput w dół,
  ekrany zgasły po bezczynności przy odblokowanej sesji, palce podniesione przy wygaszonych ekranach. Stuknięcie
  próbne nie dotarło do okna testowego, a po `duo-wake --touch-reset` dotarło;
- ten sam przebieg z włączonym resetem (16:05): ekrany zgasły po bezczynności, palce podniesione przy wygaszonych
  ekranach, po włączeniu ekranów usługa zrobiła reset sama i następne stuknięcie dotarło do okna, bez dotyku trzema
  palcami.
Zmiana stanu blokady, gdy palce są jeszcze na ekranie, sama czyści gest (KWin anuluje wtedy dotyki). Dlatego
przycisk zasilania, który od razu blokuje telefon, zacina dotyk tylko wtedy, gdy palce zejdą z ekranu między
wygaszeniem a blokadą. Prawdopodobnie tak było 2026-11-20 o 12:01 (dłoń na ekranie przy naciskaniu zasilania).
`tools/linux/system/test/touch-stuck-test.py` (zasilanie zamiast bezczynności) zacięcia nie odtwarza, bo podnosi
palce dopiero po blokadzie.

## Jasność: lewy ekran jaśniejszy

Lewy ekran (DSI-1, `ae94000.dsi.0`) wyglądał na jaśniejszy od prawego (DSI-2, `ae96000.dsi.0`), najbardziej po
przygaszeniu po bezczynności. Panele nie są winne. Android na Duo (biblioteka `lights.msmnile.so`) przelicza jasność
z suwaka na nity, a dla każdego panelu ma fabryczną tabelę nity -> wartość podświetlenia (partycja `sfpd`,
`display/FactoryOs_LUT2_0.txt` i `_1.txt`). Na testowanym egzemplarzu obie tabele różnią się najwyżej o ok. 1 % (30
nitów: 164,1 i 163,9), a polecenia startowe obu paneli w DT Androida są takie same jak w sterowniku
`panel-microsoft-surface-duo`.

Przyczyna była w KWin 6.3. Jasność ekranu (suwak razy przygaszenie po bezczynności) KWin ustawia przez urządzenie
jasności przypisane do ekranu, a gdy ekran go nie ma i w `~/.config/kwinoutputconfig.json` jest
`allowSdrSoftwareBrightness: true`, przyciemnia sam obraz. PowerDevil daje KWin tylko jedno podświetlenie i KWin
przypisuje je pierwszemu wewnętrznemu ekranowi (DSI-1, lewy). Prawy ekran był więc przyciemniany dwa razy: w obrazie
i podświetleniem, które `duo-autobrightness` kopiuje z lewego. Najbardziej było to widać po przygaszeniu: przy suwaku
na 52 % i przygaszeniu do 30 % obraz prawego ekranu miał ok. 1/5 jasności. `duo-kwin-outputs` wyłącza
`allowSdrSoftwareBrightness` na obu ekranach DSI. Uruchamia go usługa `duo-kwin-outputs` przed menedżerem logowania,
bo KWin czyta ten plik tylko przy starcie (nie zatrzymuj KWin ani SDDM w działającym systemie: wyłączenie paneli
zawiesza wyświetlanie do restartu). Test po poprawce (2026-11-20: lewy ekran stały, prawy w 7 krokach od 112 do 76 %
przy wartościach 900, 400 i 150): przy każdej jasności oba ekrany wyglądały tak samo przy tej samej wartości.

Gdyby inny egzemplarz miał naprawdę różne panele, `duo-autobrightness` może mnożyć jasność ekranu przez współczynnik
z `/etc/duo/brightness-balance`, także gdy kopiuje wartość suwaka albo przygaszenia na drugi ekran. Jedna linia na
ekran: stały współczynnik (`ae96000.dsi.0=0.95`) albo krzywa `współczynnik@wartość` (wartość podświetlenia 0–1023
ekranu bez korekty, liniowo pomiędzy punktami, poza nimi stała). Usługa czyta plik przy każdej zmianie jasności.
Obraz systemu tego pliku nie tworzy. Wcześniejsza krzywa `0.86@400,0.64@160` była zmierzona, gdy KWin jeszcze
przyciemniał prawy ekran, i jest nieaktualna.

### Automatyczna jasność i PowerDevil

Po każdym włączeniu ekranów oba świeciły na 100 %, a suwak jasności Plasmy zawsze stał na maksimum (2026-11-20).
Suwakiem, przygaszaniem po 30 s bezczynności i rozjaśnianiem przy włączeniu ekranu rządzi PowerDevil. Steruje on
jednym podświetleniem: DSI-1, w D-Bus `org.kde.ScreenBrightness`, wyświetlacz `display0`, zakres 0–10000.
`duo-autobrightness` pisał poziom automatyczny wprost do pliku jasności DSI-1, z pominięciem PowerDevil. Przy każdym
włączeniu ekranów PowerDevil wpisywał więc z powrotem swoją wartość (1023, bo nikt jej u niego nie ustawił), a 30 s
później przygaszenie do 30 % (308). Usługa brała obie zmiany za ruch suwaka i kopiowała je na DSI-2. Przy
przygaszaniu i rozjaśnianiu PowerDevil nie wysyła żadnego sygnału D-Bus, a przy ruchu suwaka wysyła
`BrightnessChanged` (sprawdzone `gdbus monitor`).

Teraz, gdy PowerDevil działa:
- poziom automatyczny idzie przez PowerDevil (`SetBrightnessWithContext` z kontekstem `duo-autobrightness`, bez
  okienka na ekranie), więc suwak go pokazuje, a przygaszanie i rozjaśnianie zaczynają od niego zamiast od 1023;
- DSI-2 co 300 ms kopiuje wartość DSI-1, kto by jej nie ustawił (automatyka, suwak, przygaszenie, rozjaśnienie), z
  korektą z `/etc/duo/brightness-balance`, jeśli ten plik istnieje;
- ruch suwaka usługa poznaje po sygnale `BrightnessChanged` z innym kontekstem (proces `gdbus monitor` uruchomiony
  jako użytkownik sesji). Automatyka czeka wtedy, aż światło zmieni się mniej więcej trzykrotnie;
- gdy PowerDevil pojawi się po starcie sesji, usługa od razu ustawia przez niego poziom automatyczny, więc po
  uruchomieniu telefonu suwak i ekrany pokazują to samo.

Sprawdzone na telefonie 2026-11-20: po restarcie usługi PowerDevil pokazywał 3084 z 10000 (poziom automatyczny), a
oba ekrany 314 z 1023. Po wybudzeniu ekranów oba miały 316, czyli poziom automatyczny, nie 1023. Po 30 s oba zeszły
do 96 (30 % z 316), a DSI-2 nadążał w ciągu jednej próbki (2 s). Przygaszanie i rozjaśnianie nie liczyły się już jako
ruch suwaka.

Bez PowerDevil (inna powłoka albo plik `/etc/duo/autobrightness-no-powerdevil`) usługa pisze oba podświetlenia sama
i kopiuje ruch suwaka z jednego ekranu na drugi, jak wcześniej.

Czujnik światła (TXC PA22A w SLPI) wysyła odczyt tylko przy zmianie, a iio-sensor-proxy do pierwszego odczytu
pokazuje `LightLevel` 0. Po dwóch uruchomieniach 2026-11-20 wieczorem żaden odczyt nie przyszedł: raz proxy wystartowało,
gdy procesor czujników jeszcze się ustawiał („driver_set_polling: assertion 'sensor_device' failed”), i trzymało 0,
choć `ssccli` czytał 35 lx; dwa razy czujnik w ciemnym, nieruchomym pokoju nie wysłał nic, także do `ssccli` (tak samo
w nocy 2026-11-07, zob. `docs-linuxduobench/drivers/sensors.md`). Usługa brała to 0 za ciemność i ustawiała minimum
krzywej (ekrany na 4 %). Teraz 0 odczytane tuż po zajęciu czujnika nigdy nie jest stosowane: ekrany zostają na
poziomie z PowerDevil, aż przyjdzie prawdziwy odczyt (sygnał `PropertiesChanged` albo wartość powyżej 0). W tym
czasie usługa zwalnia czujnik i zajmuje go ponownie po 6, 12, 24, 48, 96 i 192 s (czujnik wyłącza się i włącza, a
zacięty sterownik w proxy ustawia odpytywanie od nowa). Gdy iio-sensor-proxy się zrestartuje, usługa zajmuje czujnik
od nowa. PowerDevil pamięta jasność między uruchomieniami, więc bez odczytu ekrany startują z ostatnią wartością
(po dwóch restartach 394 z 10000, czyli 13 z 1023: bardzo ciemno, ruch suwaka to poprawia).

Sprawdzone z lampą 2026-11-20 (17:04–17:07): po zapaleniu światła czujnik podał 38–64 lx, przy zasłonięciu 0, przy
lampie z bliska do 4031 lx, a jasność obu ekranów szła za tym razem (28 %, przy lampie 77 %, potem 32 %). Uśrednianie
odczytów liczyło się wcześniej przy każdym odczycie (0,35 drogi do nowej wartości). Czujnik wysyła odczyty tylko przy
zmianie, więc średnia zostawała tam, gdzie zostawił ją ostatni odczyt: usługa liczyła 27,8 lx w pokoju z 38–41 lx i
53,5 lx przy 62 lx, a lampa zapalona jednym skokiem zostałaby daleko w tyle. Teraz średnia co 0,5 s idzie dalej do
ostatniego odczytu (stała czasowa 1,2 s). Ręka przesunięta nad czujnikiem przez sekundę nadal niczego nie zmienia, bo
ściemnienie musi trwać 8 s.

Przy wygaszonych ekranach (bezczynność, przycisk zasilania, zamknięty telefon) sterownik podświetlenia DSI odrzuca
zapis jasności (`EINVAL`). Jądro i tak zapamiętuje wartość, a sterownik panelu wysyła ją po włączeniu ekranu, więc
odrzucony zapis liczy się jako zapisany. Wcześniej usługa pisała przez `open().write()` i błąd wychodził dopiero przy
zamykaniu pliku, poza obsługą błędów (w dzienniku 421 razy „OSError: [Errno 22] Invalid argument” od 12:01 do 14:52
2026-11-20).

## Czujniki

Czujniki siedzą za procesorem czujników SLPI (Qualcomm SSC): SLPI -> FastRPC -> `hexagonrpcd` -> libssc 0.4.4 ->
iio-sensor-proxy 3.9. Sprawdzone na telefonie 2026-11-20 (`ssccli`, `monitor-sensor`, `duo-hinge <typ>`):

| Czujnik | Do aplikacji (iio-sensor-proxy) | Uwagi |
|---|---|---|
| Akcelerometr (LSM6DSO) | tak, orientacja ekranu | macierz montażu z `81-duo-ssc.rules`; KWin: tryb tabletu włączony, `autoRotation` „InTabletMode” na obu ekranach, wtyczka QtSensors `iio-sensor-proxy` jest. Obracanie z telefonem w ręku: niesprawdzone na 7.3.5 |
| Światło (TXC PA22A) | tak | automatyczna jasność obu ekranów (`duo-autobrightness`) |
| Zbliżeniowy (TXC PA22A) | tak | gaszenie ekranów przy uchu w rozmowie (`duo-call-audio`) |
| Kompas | tak (kierunek) | libssc liczy kierunek z wektora obrotu SSC (`rotv`), nie z surowego magnetometru |
| Kąt zawiasu, ułożenie | nie (brak typu w iio-sensor-proxy) | `duo-posture` przez `duo-hinge hinge_angle` |
| Żyroskop, magnetometr (AK0991x), grawitacja, wektory obrotu, temperatura | nie | `duo-hinge gyro`, `mag`, `gravity`, `rotv`, `game_rv`, `geomag_rv`, `sensor_temperature` |
| Hall, krokomierz, przechylenie, wykrywanie ruchu | nie | zdarzenia tylko przy zmianie (`hall`, `pedometer`, `tilt`, `psmd`) |

Surowy magnetometr ma ok. 390 µT, ok. 8 razy więcej niż pole Ziemi (bez kalibracji, przesunięcie od magnesów i
metalu), ale kompas z niego bezpośrednio nie korzysta. `mag_cal` i `gyro_cal` otwierają się, lecz bez kalibracji w
ruchu nie wysyłają danych. `sig_motion` (`sns_smd`) wywraca libssc (asercja w `sensor_open`), więc go nie otwieraj.
Poza SSC Linux widzi tylko przetworniki ADC w PMIC-ach (temperatury, napięcia), żadnych innych czujników IIO.

`duo-sensors` po uruchomieniu SLPI i `hexagonrpcd` czeka, aż czujniki odpowiedzą, i dopiero wtedy kończy (iio-sensor-proxy
startuje po nim). Sprawdzał to czujnikiem światła, ale ten w ciemności milczy, więc po zmroku start czekał 40 s na
darmo. Od 2026-11-20 pyta akcelerometr (`ssccli --sensor accelerometer`, do 10 prób), który odpowiada zawsze,
i daje procesorowi czujników jeszcze 2 s. W dzienniku: „sensors answer (accelerometer) after N tries”.

## Wibracje: reset telefonu przy „sile wibracji”

Sterownik `da7280` (także w czystym 7.3.5) zgłasza obsługę `FF_GAIN`, ale nie ma funkcji `set_gain`. Jądro wywołuje
ją bez sprawdzenia, więc jedno zdarzenie `EV_FF`/`FF_GAIN` z przestrzeni użytkownika (np. `fftest`, gry, demony
haptyki) to wywołanie pustego wskaźnika: Duo od razu resetuje się do bootloadera, bez śladu w pstore. Zwykłe efekty
bez `FF_GAIN` (np. `FF_RUMBLE`, którego używa `hfd-service`) działają. Patch `0075` przestaje zgłaszać `FF_GAIN`, więc
jądro takie zdarzenie ignoruje. Test na telefonie: w toku.

## GPS

GNSS jest częścią modemu: `duo-gps` (`tools/linux/modem/duo-gps.c`) to klient QMI LOC, który trzyma jedną sesję i
wypisuje raporty satelitów i pozycji. Współrzędnych nigdy nie wypisuje (tylko „fix=yes (coordinates withheld)”,
niepewność w metrach i maskę technologii: `0x2` = pozycja z masztów sieci, nie z satelitów). Usługa `duo-gps-assist`
po starcie modemu zdejmuje blokadę silnika, wyłącza sprawdzanie wersji XTRA, wstrzykuje czas UTC i dane orbit XTRA
(`xtra3grcej.bin` z serwerów samego modemu, `path1..3.xtracloud.net`, w `/var/cache/duo-gps`, odświeżane po 24 h).
Bez tych danych silnik szuka satelitów od zera.

Poprawka 2026-11-20: w obrazie Debiana nie ma `wget`, a skrypt czekał na sieć przez `wget --spider` albo `ping
1.1.1.1` (ICMP do internetu bywa blokowany, choć sieć działa). Pętla nigdy się nie kończyła i XTRA nie zostało
pobrane ani razu (`/var/cache/duo-gps` był pusty). Teraz skrypt czeka na odpowiedź DNS dla `path1.xtracloud.net`
i pobiera plik przez `curl` (zapasowo `wget` albo `python3`).

ModemManager zgłasza źródła lokalizacji `3gpp-lac-ci`, `gps-raw`, `gps-nmea`, `agps-msa`, `agps-msb`; domyślnie
włączone jest tylko `3gpp-lac-ci`. Geoclue i aplikacji z mapą w obrazie jeszcze nie ma.

## Budowanie

Najprościej w GitHub Actions: **Actions → build → Run workflow**. Powstają artefakty:

- `duo-kernel-and-boot`: `duo-debian.img`, `duo-modules-*.tar.zst`, `Image.gz`, dtb, config
- `duo-debian-rootfs`: `duo-debian-rootfs.tar.zst`

Lokalnie (Ubuntu/Debian x86-64, jako root):

```sh
sudo scripts/build-kernel-and-boot.sh "$PWD" out           # jądro, moduły, obraz startowy
sudo rootfs/build-debian-rootfs.sh "$PWD" out/duo-debian-rootfs.tar.zst   # Debian + Plasma Mobile (qemu-user-static)
```

Obraz startowy domyślnie używa standardowych adresów Qualcomm w nagłówku. Jeśli bootloader go odrzuci, zbuduj go
z oryginalnym `boot.img` Microsoftu jako wzorcem: `scripts/build-kernel-and-boot.sh "$PWD" out /ścieżka/boot.img`.

## Instalacja

Szczegóły: [docs/INSTALL.pl.md](docs/INSTALL.pl.md). W skrócie, na komputerze z Linuxem:

```sh
scripts/install-debian.sh duo-debian.img duo-debian-rootfs.tar.zst duo-modules-*.tar.zst
# potem przy każdym starcie:
fastboot --set-active=a && fastboot boot duo-debian-key.img
```

## Struktura

- `tools/linux/kernel/series/`: łatki na Linux v7.3.5 (53 z linuxduobench + 1 z przeniesieniem i poprawkami; opis w ostatniej)
- `tools/linux/`: reszta narzędzi linuxduobench (initramfs, usługi `duo-*`, obraz ABL, audio, modem)
- `rootfs/`: budowanie Debiana z Plasma Mobile, iptsd i czujników
- `scripts/`: budowanie jądra i obrazu, instalacja na telefon
- `docs-linuxduobench/`: notatki sterowników z linuxduobench (po angielsku)

## Licencja

GPL-2.0-or-later (jak linuxduobench); kod jądra GPL-2.0-only, device tree BSD-3-Clause, zgodnie z nagłówkami SPDX.
Firmware telefonu nie jest dołączone: system czyta je z partycji Twojego telefonu.
