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
| Dotyk (multi-touch) | działa | spi-hid-duo + iptsd (budowany w obrazie) |
| Pióro | nie | jest w danych iptsd, nieskończone |
| GPU (Adreno 640) | działa | Mesa freedreno/turnip z backports |
| Wi-Fi, Bluetooth | działa | ath10k / hci_qca |
| Dane komórkowe (LTE) | działa | modem + IPA v4.1 + ModemManager |
| **Połączenia, SMS** | **działa (2G)** | SMS i połączenia przez 2G z [obejściem modemu](#modem-obejście-awarii-quick-offline); [dźwięk rozmowy](#dźwięk-rozmowy-q6voice) w obie strony (słuchawka + mikrofon), przychodzące i wychodzące, z usługą `duo-call-audio`, [dzwonek z wibracją i budzeniem ekranu](#dzwonek-wibracja-i-budzenie-ekranu). Bez głośnomówiącego |
| Głośnik, mikrofony | działa | WCD9340 + WSA8815, strojenie z ACDB telefonu. Na 7.3.5 głośnik milczał: [naprawione trasą w DT](#głośnik-cisza-na-735) |
| Słuchawka do rozmów | działa | wyjście EAR kodeka (`0076`–`0079`); w rozmowie gra przez DSP ([dźwięk rozmowy](#dźwięk-rozmowy-q6voice)), [ciszej niż głośnik](#słuchawka-do-rozmów-wyjście-ear) |
| Czujniki, zawias, obrót | działa | SLPI + hexagonrpcd + libssc (budowane w obrazie) |
| Aparat | częściowo | wymaga modułu imx351 zbudowanego z danych Twojego telefonu |
| Ładowanie, bateria | działa | qcom_smbx (SMB5) + 2x bq27742 |
| Usypianie | wyłączone | s2idle działa, ale telefon się nie budził; na razie bez usypiania |
| GPS | częściowo | pozycja z sieci tak, fix satelitarny niepotwierdzony |
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
- Przed `fastboot boot` zawsze `fastboot --set-active=a` (inaczej bootloader odmawia).
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
- przy odebraniu, a przy wychodzącym już przy sygnale dzwonienia, wyłącza tor głośnika i od razu otwiera PCM głosowy;
- po rozłączeniu zamyka PCM i przywraca głośnik. Pierwszy obiekt połączenia przychodzącego w ModemManagerze często
  znika po 1–3 s i pojawia się nowy, dlatego usługa czeka 6 s, zanim uzna nieodebrane połączenie za skończone.

W rozmowie usługa bierze mikrofon u dołu prawej połówki (`DMIC MUX7` = `DMIC1`, ten sam co Android) z `DEC7 Volume` 98
(+14 dB, najlepszy z poziomów 66–114 w dwóch rozmowach testowych). `DMIC2` to mikrofon u góry lewej połówki. `DMIC3`
czyta drugą krawędź tej samej linii danych co `DMIC2`, więc daje ten sam mikrofon (korelacja 0,99), a `DMIC0`, `DMIC4`
i `DMIC5` dają cyfrowe zero (wcześniejszy komentarz w UCM „DMIC1 nie jest podłączony” był błędny). Profil HiFi w UCM
(`DMIC3` + `DMIC2`) nagrywa więc na razie dwa razy górny mikrofon. W nagraniu HiFi kanał z `DEC7` wychodzi ok. 20 dB
głośniej niż z `DEC8` przy tym samym ustawieniu (ten sam mikrofon na `DMIC3` i `DMIC2`), więc poziomów z nagrania nie
da się przenieść wprost na rozmowę.

Kodek trzeba przełączyć na słuchawkę chwilę przed startem toru głosowego. Gdy przełączenie było tuż przed startem
(w `dmesg`: `overflow error on RX port 0`), słuchawka milczała aż do ponownego otwarcia PCM, przy dowolnym odstępie
od odebrania. Dlatego usługa przełącza kodek już przy dzwonieniu; dźwięk jest ok. 1,5 s po odebraniu. Ustawienia
(głośność słuchawki, mikrofon i jego czułość, start przy sygnale dzwonienia) są w `/etc/default/duo-call-audio`, opis
w nagłówku skryptu. Nie przeładowuj modułów q6voice w działającym systemie: karta się przebindowuje i porty AFE
nie działają aż do restartu telefonu. Komunikaty kodeka `overflow error on RX port 0` / `TX port 8` przy
przełączaniu tras poza startem toru głosowego są nieszkodliwe.

Telefon testowy z obrazem sprzed serii `0080`–`0084` działa tymczasowo z modułami q6voice z tymi poprawkami w wersji
diagnostycznej: `/lib/modules/$(uname -r)/updates/q6voice` (po `depmod`) i `/etc/modprobe.d/duo-q6voice.conf`
z `options q6voice cvs_unmute=1 tx_topo_override=0x10f70` oraz `options q6cvp fix_rx_map=1`. Jądro z tego repo
tego nie potrzebuje.

Do zrobienia: przełączanie na głośnik (tryb głośnomówiący), głośność rozmowy z suwaka, tłumienie echa i szumów,
VoLTE, drugi kanał mikrofonu w UCM na `DMIC1` zamiast `DMIC3`. Dzwonek i wibracja:
[niżej](#dzwonek-wibracja-i-budzenie-ekranu).

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
`DUO_CALL_VIBRATE=0` (bez wibracji), `DUO_CALL_WAKE=0` (bez budzenia).

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

## Wibracje: reset telefonu przy „sile wibracji”

Sterownik `da7280` (także w czystym 7.3.5) zgłasza obsługę `FF_GAIN`, ale nie ma funkcji `set_gain`. Jądro wywołuje
ją bez sprawdzenia, więc jedno zdarzenie `EV_FF`/`FF_GAIN` z przestrzeni użytkownika (np. `fftest`, gry, demony
haptyki) to wywołanie pustego wskaźnika: Duo od razu resetuje się do bootloadera, bez śladu w pstore. Zwykłe efekty
bez `FF_GAIN` (np. `FF_RUMBLE`, którego używa `hfd-service`) działają. Patch `0075` przestaje zgłaszać `FF_GAIN`, więc
jądro takie zdarzenie ignoruje. Test na telefonie: w toku.

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
