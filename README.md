# QemuHDAAudio.kext

Driver de áudio para Mac OS X **10.6 a 10.9** rodando no **QEMU/UTM**, feito para o controlador Intel HDA emulado (`intel-hda` / `ich9-intel-hda`) com o codec `hda-output`.

Foi pensado para convidados em **emulação x86 (TCG)**, por exemplo num Mac com Apple Silicon, onde o VoodooHDA e o `usb-audio` falham ou "martelam". Para absorver a latência irregular do convidado emulado, ele usa:

- um buffer DMA grande, de cerca de 340 ms;
- um *sample offset* grande e ajustável;
- timestamps calculados a partir da posição do DMA, sem depender de interrupções.

Saída 16 bits estéreo em **44,1 kHz** (padrão) ou 48 kHz. Só x86_64: o convidado precisa estar com o kernel 64-bit (`uname -m` → `x86_64`).

## Obter o kext

O GitHub Actions compila a cada push.

1. Abra a aba **Actions** do repositório, clique no último run do workflow `build` e baixe o artefato `QemuHDAAudio-…`.
2. O artefato é um `.zip` que contém outro `QemuHDAAudio-<versão>.zip`, com o kext e os scripts.
3. Em tags `v*` (por exemplo `v0.1.0`), o zip também é anexado a uma Release.

O build roda no GitHub Actions (`make deps && make zip` num runner macOS).

## Configurar a VM no UTM

Em *Som*, escolha **Nenhum**. Em *QEMU → Argumentos*, acrescente um argumento por linha:

```
-audiodev
coreaudio,id=snd0
-device
ich9-intel-hda,id=hda0
-device
hda-output,bus=hda0.0,audiodev=snd0
```

`intel-hda` (ICH6) também funciona no lugar de `ich9-intel-hda`. Desinstale o VoodooHDA antes.

### Taxa de amostragem

Por padrão, o QEMU abre a saída CoreAudio do host em **44100 Hz**. O kext também usa 44100 Hz por padrão, então nada é reamostrado no caminho.

Se preferir 48 kHz:

1. Use `coreaudio,id=snd0,out.frequency=48000` no `-audiodev`. O QEMU passa a abrir o dispositivo do host em 48 kHz.
2. No convidado, em *Configuração de Áudio MIDI*, escolha 48000 Hz para o QEMU HDA Audio.

As duas taxas precisam coincidir para evitar reamostragem.

## Instalar no convidado

Copie o zip para o convidado e rode:

```sh
unzip QemuHDAAudio-<versão>.zip
cd QemuHDAAudio-<versão>
sudo ./install.sh
```

Depois:

1. Em *Preferências do Sistema → Som → Saída*, escolha **QEMU HDA Audio**.
2. Teste com `afplay /System/Library/Sounds/Submarine.aiff`.

Para remover, rode `sudo ./uninstall.sh` e reinicie.

## Se não funcionar

```sh
sudo ./collect-logs.sh
```

Isso gera um relatório na Mesa com logs, estado do driver e do IORegistry.

Se o som ainda falhar, aumente `QemuHDASampleOffset` no `Info.plist` do kext instalado:

```
/System/Library/Extensions/QemuHDAAudio.kext/Contents/Info.plist
```

O padrão é 4096 frames (~93 ms a 44,1 kHz). Tente 8192 e reinicie. Os outros ajustes estão documentados no próprio `Info.plist` e no `CLAUDE.md`.

## Licença

Código deste projeto: MIT (veja `LICENSE`). Durante o build são baixados os headers do IOAudioFamily da Apple e o MacKernelSDK (acidanthera), ambos sob APSL 2.0. O binário final inclui a `libkmod.a` do MacKernelSDK.
