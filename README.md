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
| **Połączenia, SMS** | **nie** | modem działa, brakuje dźwięku rozmowy i obsługi w ModemManager. Do zrobienia |
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
