# CLAUDE.md — QemuHDAAudio.kext

Contexto para o Claude Code continuar este projeto. **Fale com o usuário em inglês** (pedido do usuário em 2026-10-22; antes era português). Código, comentários e mensagens de commit em inglês.

## Objetivo

Driver de áudio (kext IOAudioFamily) para o controlador Intel HDA emulado pelo QEMU (`intel-hda` / `ich9-intel-hda` + codec `hda-output`), para convidados **Mac OS X 10.6.8, 10.7, 10.8 e 10.9** rodando no **UTM em emulação x86_64 (TCG)** num **Mac M4 Pro** (host macOS 27). Meta: áudio limpo, sem "martelada", tolerando a latência alta e irregular do convidado emulado.

## Como chegamos aqui (resumo do diagnóstico)

- **VoodooHDA 2.8.8** (10.6): carrega e cria `VoodooHDAEngine` ("Line-out (Green N/A)"), mas não sai som nenhum, em nenhum backend (CoreAudio/SPICE), nem com ICH6/ICH9, `msi=off`, `hda-duplex`. Nunca confirmamos se o DMA dele chegava a rodar. Já foi desinstalado do convidado.
- **usb-audio** (10.6, driver USB Audio da Apple): toca, mas "martelando"; até sons de alerta ficam irreconhecíveis. CoreAudio soa um pouco melhor que SPICE.
  - Com `-device usb-audio,...,debug=2` e Debug Logging do UTM: **nenhum** `usb-audio: output overrun` → nenhum pacote foi descartado pelo QEMU (a hipótese de pacotes ≠ 192 bytes foi **descartada**). Volume/mute ok (`vol 240` = 0 dB).
  - Buffers maiores (`out.buffer-count`, `buffer=`) e `timer-period` não resolveram. CPU do host folgada.
  - Conclusão: **latência do convidado** — o driver USB da Apple agenda poucos ms de áudio adiantado; sob TCG qualquer atraso vira buraco.
- Decisão: escrever um driver HDA próprio com **ring DMA grande** e **sample offset grande**, sem depender de interrupções.

## Ambiente do usuário

- Host: Mac M4 Pro, macOS 27 ("Golden Gate"), UTM com QEMU **10.0.12** (visto no QMP). Alto-falantes do host em 44,1 kHz (definido pelo próprio QEMU).
- VM "Mac OS X 1068": `q35`, `-cpu Penryn,+ssse3,+sse4.1,+sse4.2`, 2 cores, `-accel tcg,thread=multi`, OVMF + `EFI-LEGACY` (OpenCore), ICH9 EHCI + UHCI, `-audio none` + audiodevs próprios.
- Kernel do convidado 10.6: **x86_64** (`uname -m`). O kext é só x86_64.
- O usuário não tem Xcode; **compila via GitHub Actions** e copia o zip para o convidado.
- O UTM é sandboxed: arquivos escritos pelo QEMU (ex.: `-audiodev wav`) não apareceram onde esperávamos; não insista nesse caminho.

## Arquitetura

```
src/
  QemuHDAAudioDevice.{h,cpp}  IOAudioDevice: PCI/BAR0, reset do controlador, descoberta do codec,
                              buffers DMA (ring 64 KiB + BDL), start/stop do stream, leitura do LPIB.
  QemuHDAAudioEngine.{h,cpp}  IOAudioEngine: 1 stream de saída 16 bit/estéreo (44,1 ou 48 kHz), controles de
                              volume/mute (software), clip float→SInt16, timer que faz polling do LPIB.
  QemuHDARegs.h               registradores HDA e verbos de codec usados.
  QemuHDAGainTable.h          curva de volume pré-calculada (0,75 dB/passo) — sem pow() no kernel.
  kmod_info.c                 equivalente ao <produto>_info.c que o Xcode gera para kexts.
Resources/Info.plist.in       personality (IOPCIMatch 0x26688086 0x293e8086, IOProbeScore 6000) + ajustes.
guest/                        install.sh / uninstall.sh / collect-logs.sh (bash 3.2, rodam no convidado).
scripts/fetch-deps.sh         MacKernelSDK (pinado) + headers IOAudioFamily-183.4.11 (era 10.6).
scripts/macho-compat.py       remove load commands que o kxld do 10.6 não aceita.
```

Decisões importantes:

- **Sem interrupções.** O DMA roda continuamente; um `IOTimerEventSource` (padrão 5 ms) lê o LPIB. Ao detectar a volta do buffer, o timestamp é **retroativado** (`agora − LPIB/taxa`), então atraso no polling não vira jitter de timestamp. Polling atrasado mais que um ring inteiro estima as voltas pelo tempo decorrido.
- **Interface de comando imediato** (IC/IR/IRS) em vez de CORB/RIRB. Observação: o QEMU deixa `ICB` preso se um verbo for para um codec inexistente; `sendImmediate()` limpa à força.
- **Volume em software** (no clip). O amp do DAC fica no máximo (só existe com `mixer=on` no QEMU).
- O codec do QEMU tem vendor `0x1af4`; com `mixer=on` ID `0x1af40011`, AFG nid 1, DAC nid 2, pin nid 3 — mas o código descobre os nós genericamente (funciona com `hda-duplex`/`hda-micro`).
- **Taxa: 44,1 kHz por padrão**, 48 kHz opcional (`QemuHDASampleRate` ou Configuração de Áudio MIDI). Motivo: o `-audiodev` do QEMU usa `frequency=44100` por padrão e o backend CoreAudio (`audio/coreaudio.m`) muda o formato do dispositivo do host para essa taxa (por isso os alto-falantes do usuário ficaram em 44,1 kHz). Com convidado e audiodev na mesma taxa, o QEMU não reamostra. Troca de taxa em `performFormatChange`: para o DMA, troca FMT + verbo de formato do codec, reinicia e zera o rastreamento de posição.
- Lado QEMU (lido no fonte `hw/audio/intel-hda.c` / `hda-codec.c`): streams de saída são SD4–7 (`GCAP=0x4401`), o QEMU puxa os dados num timer de 1 ms no ritmo exato da taxa do stream (`use-timer=on`, padrão), o LPIB avança em blocos de ~1 ms.

## Build

Requer macOS com Xcode (CI usa `macos-15`). Não há `.xcodeproj` de propósito.

**REGRA: nunca compile nada no Mac do usuário.** Nada de `make`, `make deps`, `make objects`, Xcode ou download de dependências localmente. Toda compilação acontece **só no GitHub Actions** (repo privado `brunocastello/qemu-hda-kext`): commit + push, acompanhe com `gh run watch`, leia falhas com `gh run view --log-failed`, baixe o artefato com `gh run download`. Os comandos abaixo são o que o workflow executa.

```sh
make deps     # clona deps/ (MacKernelSDK @ 3f750085…, IOAudioFamily-183.4.11)
make          # build/QemuHDAAudio.kext
make check    # load commands, segmentos, símbolos indefinidos (falha se houver __cxa_atexit etc.)
make zip      # build/QemuHDAAudio-<VERSION>.zip (kext + scripts do convidado)
make objects  # só compila
```

Versão em `VERSION`. Pontos sensíveis das flags (não remova sem motivo):

- `-isystem deps/ioaudio-overlay` **antes** do MacKernelSDK: compilar contra o IOAudioFamily **mais antigo** suportado (1.8.3) garante que o kext só referencia símbolos/slots de vtable que existem no 10.6; versões novas do IOAudioFamily continuam compatíveis com binários antigos (o kxld corrige os slots reservados).
- `-target x86_64-apple-macos10.6` + `-mkernel -fapple-kext -fno-use-cxa-atexit -fno-rtti -fno-exceptions -fno-stack-protector`.
- `-mllvm -disable-atexit-based-global-dtor-lowering` (condicional): LLVM recente transforma destrutores estáticos (ex.: o `MetaClass` do `OSDefineMetaClassAndStructors`) em chamadas a `__cxa_atexit`, que nenhum kernel exporta. Sem isso o kext não carrega. O `make check` falha se isso voltar.
- `scripts/macho-compat.py`: o kxld do 10.6 (xnu-1504) rejeita `LC_VERSION_MIN_MACOSX`, `LC_FUNCTION_STARTS`, `LC_DATA_IN_CODE`, `LC_SOURCE_VERSION`, `LC_BUILD_VERSION`. O script remove esses comandos (só metadados) e **falha** em qualquer load command desconhecido (ex.: chained fixups) em vez de remover algo funcional.
- `OSBundleLibraries` com versões baixas (`IOAudioFamily 1.1fc9`, KPIs `10.0`) para carregar do 10.6 ao 10.9.

### Situação do build (importante)

O código **compila** (verificado com clang 18 no Linux, sem warnings, sem `__cxa_atexit`). O **link ainda não foi testado num Mac real** — o primeiro run do GitHub Actions é o teste. Se falhar no link:

1. Leia o log do passo "Build". Suspeitos: o `ld` novo (ld-prime) com `-kext` e alvo 10.6; tente `-Xlinker -ld_classic` em `LDFLAGS` (se a versão do Xcode ainda tiver), ou fixe outra versão do Xcode no workflow (`sudo xcode-select -s /Applications/Xcode_XX.app`).
2. `libclang_rt.cc_kext.a` é opcional; se faltar símbolo de builtin (`___udivti3` etc.), ache o caminho com `xcrun clang -print-file-name=lib/darwin/libclang_rt.cc_kext.a`.
3. Se o `macho-compat.py` acusar load command desconhecido, investigue antes de adicioná-lo à lista `STRIP`.

## Teste no convidado

Argumentos QEMU recomendados no UTM (VM → QEMU → Arguments, um por linha; Som do UTM = **Nenhum**):

```
-audiodev
coreaudio,id=snd0
-device
ich9-intel-hda,id=hda0
-device
hda-output,bus=hda0.0,audiodev=snd0
```

No convidado:

```sh
unzip QemuHDAAudio-x.y.z.zip && cd QemuHDAAudio-x.y.z
sudo ./install.sh            # copia p/ /System/Library/Extensions, kextutil -t, kextload
# Preferências do Sistema → Som → Saída → "QEMU HDA Audio"
afplay /System/Library/Sounds/Submarine.aiff
sudo ./collect-logs.sh       # gera ~/Desktop/qemuhda-report-*.txt para colar aqui
```

Logs: 10.6/10.7 em `/var/log/kernel.log`; 10.8/10.9 em `/var/log/system.log`. Prefixo `QemuHDAAudio:`.

### Ajustes (Info.plist do kext instalado)

| Chave | Padrão | Efeito |
|---|---|---|
| `QemuHDASampleRate` | 44100 | taxa inicial (44100 ou 48000); deve bater com o `out.frequency` do `-audiodev` (padrão do QEMU: 44100) |
| `QemuHDADebug` | 1 | 0 erros; 1 startup; 2 eventos do stream + estatísticas a cada 10 s (voltas, polls atrasados, maior intervalo de poll); 3 cada verbo |
| `QemuHDASampleOffset` | 4096 | frames que o CoreAudio escreve à frente do DMA (~93 ms a 44,1 kHz). **Principal botão contra a martelada.** Faixa 64–8192 |
| `QemuHDASampleLatency` | 32 | latência reportada (só sincronia A/V) |
| `QemuHDAPollIntervalMs` | 5 | intervalo do polling do LPIB |
| `QemuHDAAllowAnyCodec` | 0 | aceitar codec não-QEMU (só teste) |

Depois de editar: `sudo touch /System/Library/Extensions` e reiniciar (ou `sudo kextunload -b org.qemuhda.driver.QemuHDAAudio && sudo kextload …`).

## Riscos / o que olhar primeiro se não funcionar

1. **Não carrega no 10.6** → `kextutil -n -t` mostra o motivo: símbolo faltando (ver `make check`), load command inválido, dependência.
2. **Carrega mas não aparece no Som** → `ioreg -l -w0 -r -c QemuHDAAudioDevice`; log com `QemuHDADebug=2` mostra GCAP, STATESTS, vendor do codec, widgets.
3. **Aparece mas silêncio** → com `QemuHDADebug=2`, ver se "output stream started" aparece e se `stats: wraps` cresce. Se wraps fica 0, o DMA não anda (checar Bus Master, BDL, tag do stream 1 no codec).
4. **Ainda martela** → subir `QemuHDASampleOffset` (8192); ver nas stats o maior intervalo de poll — mede a latência do convidado.
5. A versão exata do IOAudioFamily no 10.6.8 do usuário: `kextstat | grep IOAudioFamily` (esperado 1.8.x). Se for menor que 1.8.3, trocar `IOAF_TAG` em `scripts/fetch-deps.sh` para a tag correspondente.

## Próximos passos

- Primeiro build no Actions → corrigir link se preciso.
- Testar no 10.6.8; depois 10.7, 10.8, 10.9 (mesmo binário).
- Se ficar bom: gerenciamento de energia (sleep/wake), entrada (`hda-duplex`).

## Convenções

- Não reformatar arquivos inteiros; mudanças pequenas e focadas.
- Não compile localmente (ver regra em Build); valide mudanças pelo GitHub Actions.
- Scripts em `guest/` precisam rodar no bash 3.2 do 10.6 (sem `[[ =~ ]]` complexo, sem `mapfile`, sem `${var,,}`).
