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
| **Połączenia, SMS** | **częściowo** | na tym repo: SMS i połączenia (przez 2G) działają z [obejściem modemu](#modem-obejście-awarii-quick-offline), ale bez dźwięku rozmowy, dzwonka i wibracji. Do zrobienia |
| Głośnik, mikrofony | działa | WCD9340 + WSA8815, strojenie z ACDB telefonu |
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
(usługa `duo-diag`):

```sh
duo-efs-write mkdir /nv/item_files/modem/utils
duo-efs-write mkdir /nv/item_files/modem/utils/mcpm
duo-efs-write put /nv/item_files/modem/utils/mcpm/mcpm_quick_offline_cfg 01
duo-efs cat /nv/item_files/modem/utils/mcpm/mcpm_quick_offline_cfg     # ma pokazać 01
# czysty restart modemu: modem zapisuje EFS przy zatrzymaniu i czyta wpis przy starcie
M=$(echo /sys/bus/platform/devices/4080000.remoteproc/remoteproc/remoteproc*)
echo stop > $M/state; sleep 2; echo start > $M/state
sync
```

Nie restartuj modemu po jego awarii (wtedy zawiesza się TrustZone i telefon się resetuje); po awarii tylko reset
telefonu (power + głośność w dół).

**Cofnięcie:** `duo-efs-write rm /nv/item_files/modem/utils/mcpm/mcpm_quick_offline_cfg` i czysty restart modemu,
albo usunięcie `/var/lib/duo-efs` (przy następnym starcie powstanie nowa kopia z partycji), albo
`touch /etc/duo/efs-partitions` (stary tryb: partycje tylko do odczytu, zmiany EFS tylko w RAM do restartu).

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
