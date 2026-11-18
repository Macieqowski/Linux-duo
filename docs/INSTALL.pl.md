# Instalacja na Surface Duo 1

Wszystko robi się z komputera z Linuxem (albo Windows + WSL2 z `usbipd`). Na telefonie zapisywana jest **tylko
partycja `userdata`**.

## 0. Potrzebne

- Surface Duo 1 z **odblokowanym bootloaderem**. W Androidzie: Ustawienia → Opcje programisty → Odblokowanie OEM,
  potem w bootloaderze `fastboot flashing unlock`. To kasuje dane Androida. **Nigdy nie blokuj go z powrotem.**
- Kabel USB-C z transmisją danych, telefon naładowany powyżej 50%.
- Na komputerze: `fastboot` (Android platform-tools), `ssh`, `python3`, `zstd`.
- Pliki z GitHub Actions (artefakty `duo-kernel-and-boot` i `duo-debian-rootfs`): `duo-debian.img`,
  `duo-modules-*.tar.zst`, `duo-debian-rootfs.tar.zst`. Sprawdź sumy: `sha256sum -c SHA256SUMS`.

## 1. Menu bootloadera

Telefon wyłączony: przytrzymaj **głośność w dół** i naciśnij **zasilanie**. Na komputerze `fastboot devices` ma
pokazać telefon.

## 2. Najpierw sama próba (nic nie zapisuje)

```sh
python3 tools/linux/abl/add-ssh-key.py duo-debian.img ~/.ssh/id_ed25519.pub duo-debian-key.img
fastboot --set-active=a
fastboot boot duo-debian-key.img
```

Oczekiwane: oba ekrany się zapalają, na komputerze pojawia się nowa sieć USB, a
`ssh root@169.254.66.1 uname -r` odpowiada `7.3.5-...`. Debiana jeszcze nie ma, więc startuje powłoka instalacyjna.
Jeśli ekran zostaje na logo Microsoftu: przytrzymaj zasilanie, telefon jest bez zmian. Wtedy obraz trzeba zbudować
z oryginalnym `boot.img` Microsoftu jako wzorcem (README, „Budowanie”). Zgłoś, co się stało, wraz ze zdjęciem ekranu.

Jeśli komputer nie dostaje adresu w sieci USB, nadaj mu ręcznie `169.254.66.2/16` na nowym interfejsie.

## 3. Instalacja Debiana

```sh
scripts/install-debian.sh duo-debian.img duo-debian-rootfs.tar.zst duo-modules-*.tar.zst
```

Skrypt uruchamia obraz z RAM-u, pyta o potwierdzenie (wpisz `YES`), formatuje `userdata`, kopiuje Debiana do
`/debian` i resetuje telefon.

## 4. Każdy kolejny start

Telefon w menu bootloadera, kabel do komputera:

```sh
fastboot --set-active=a && fastboot boot duo-debian-key.img
```

Plasma Mobile startuje na obu ekranach, użytkownik `user` loguje się sam (bez hasła; ustaw je poleceniem `passwd`).
SSH po USB: `ssh -i ~/.ssh/duo_ed25519 root@169.254.66.1`.

## Powrót do Androida

Android nie był ruszany poza `userdata`. Uruchom telefon normalnie i zgódź się na przywrócenie ustawień fabrycznych,
jeśli o nie zapyta (to skasuje Debiana). Jeśli Android nie startuje, można go przywrócić pakietem odzyskiwania
Microsoftu, dopóki działa Android Recovery.

## Jeśli coś nie działa

- Logi jądra z działającego systemu: `ssh root@169.254.66.1 dmesg > dmesg.txt`.
- Stan usług Duo: `ssh root@169.254.66.1 'systemctl status "duo-*"'` i pliki `/var/log/duo-*.log`.
- Zawieszony telefon: przytrzymaj zasilanie ok. 15 s, potem znowu menu bootloadera.
