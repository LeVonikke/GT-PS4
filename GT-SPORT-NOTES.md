# GT Sport no shadPS4 — journal

## Origem do projeto

Pivô a partir do projeto GT7 (`GT7-PC-Recomp`, agora só no histórico do GitHub em
`LeVonikke/GT7-PC-Recomp`). GT7 ficou travado num loop real de rede (`pdinetwork` epoll,
código não-stub, provavelmente o backend do próprio jogo tentando falar com servidor
inacessível) sem caminho claro de correção, e o downgrade pra v1.18 (que tem debug menu
documentado) esbarrou em só ter o update-delta baixado, sem o jogo base v1.00.

Em 03/set/2026 o usuário decidiu apagar tudo do GT7 (instalação de 113GB, update PKG de 74MB,
repo local de 5.4GB — todos removidos, repo preservado no GitHub) e pivotar pra **Gran Turismo
Sport**.

Este repo (`GTSPORTHACKFIX/`) é um clone fresco de `LeVonikke/GT7-PC-Recomp` (commit base
`4a657bb3`) reaproveitado como ponto de partida — todas as correções genéricas de shadPS4 já
feitas lá (fix do pool de NIDs, throttle de stub, cherry-picks de upstream, opcodes de shader
F64/conversão) valem independente do jogo.

Remotes: `origin` = fork privado (`LeVonikke/GT7-PC-Recomp.git`, mesmo repo por enquanto — não
foi criado repo novo), `upstream` = `shadps4-emu/shadPS4.git` (pra cherry-picks futuros).

## Identificação do jogo

- **Title ID US**: `CUSA03220` (o que vamos usar — os runbooks encontrados também usam esse).
- Title ID EU: `CUSA02168`. Title ID Ásia: `CUSA03667`.
- Jogo ainda **não obtido** — usuário vai atrás por conta própria.

## Versão-alvo

- **Update 01.68**, e não mais que isso — confirmado em duas fontes independentes:
  - Team XLink Wiki: "Update 1.68 installed, but no newer!"
  - `illusion0001/console-game-patches` e `illusionyy/PS-Game-Patch` (repos reais do "illusion"
    citado no Reddit): patch YAML existe pras versões **01.00, 01.14 e 01.68** — não pra 01.69.
- **DLC: não precisa.** O debug menu do GoldHen tem opção "Unlock All Cars" nativa.
- ORBISPatches confirma 01.68 = firmware 8.52 (15/set/2021), pacote de ~53.7GB — grande,
  aparentemente atualização cumulativa e não delta pequena (diferente do caso do GT7 v1.18).

## Natureza do patch de debug menu (diferente do GT7!)

O patch de GT Sport (por "illusion") **não é um patch estático de eboot.bin** como o do GT7
v1.18 (que foi um backport completo por Opoisso893). É um **cheat do GoldHen** (framework de
cheat em runtime pra PS4 jailbreak real), com formato YAML/JSON próprio
(`app_ver`/`app_titleid`/`app_elf`), aplicado via GoldHen Cheats Manager em hardware real.

Pra portar pro shadPS4 (que não roda GoldHen), duas rotas possíveis, ainda não escolhida:
1. Achar uma versão "backported" do FPKG 1.68/1.69 com o patch já embutido — mais parecido
   com o fluxo do GT7, dá pra comparar eboot original vs. patched direto.
2. Ler o cheat YAML/JSON do illusion (endereços de memória) e portar manualmente pro nosso
   fork — mais trabalhoso, precisa mapear endereço de memória real pra offset de arquivo.

## Achado importante: GT Sport não bootava no shadPS4 (até v0.12.5)

Via runbook real de terceiro (`akitaonrails/distrobox-gaming`, `docs/shadps4-pkg-install.md`,
que documenta a própria tentativa da pessoa de instalar `CUSA03220` no shadPS4): o jogo **não
chegava a bootar**, com três bugs conhecidos do recompilador de shader:

1. `liverpool_to_vk.cpp` — `StencilOp: Unreachable code`
2. `vector_memory.cpp:197` — `Non immediate offset not supported`
3. `spirv_emit_context.cpp:861` — assertion (função `GetFormat()`)

**Checado contra o shadPS4 upstream atual (main) em 04/set/2026:**

1. **Parece corrigido.** O switch de `AmdGpu::StencilFunc` em `liverpool_to_vk.cpp` hoje
   cobre todos os casos do enum (inclusive `Ones` e os bitwise `And/Or/Xor/Nand/Nor/Xnor`,
   que provavelmente eram os que faltavam), com fallback de `LOG_WARNING` + valor seguro em
   vez de cair no antigo `UNREACHABLE()`. Só sobra `UNREACHABLE()` pra valor de enum
   realmente inválido.
2. **Provavelmente corrigido/mudou de forma.** A string "Non immediate offset not supported"
   não existe mais em `vector_memory.cpp` (716 linhas hoje). A linha 197 atual é outra coisa
   (dispatch de `IMAGE_GATHER4`). Indício forte, não prova — só testando de verdade confirma.
3. **Ainda existe, mesma classe de bug, linha diferente.** A função `GetFormat()` em
   `spirv_emit_context.cpp` (mapeia formato de textura AMD → SPIR-V) ainda termina com
   `UNREACHABLE_MSG("Unknown storage format data_format={}, num_format={}", ...)` — hoje na
   linha 934 (arquivo cresceu). Catch-all pra qualquer combinação de formato de textura que o
   código não reconheça. Alvo concreto e bem delimitado se o boot travar aí — mesmo padrão dos
   opcodes F64 de shader que já resolvemos no GT7.

**Conclusão prática**: vale tentar bootar antes de assumir que precisa de trabalho grande —
pode já passar dos 3 bugs originais, ou travar só no terceiro (`GetFormat`), que é um alvo
pequeno e conhecido.

## Estado do build

Clone feito em 04/set/2026 (commit base `4a657bb3`, herdado do GT7-PC-Recomp).

**Build concluído com sucesso** em 04/set/2026. Binário em
`build/shadps4` (106MB, roda — confirmado com `./shadps4 --version`). Todas as edições
herdadas do GT7 (resolver override em `net_util.cpp`, cache de NID + throttle + fix do
DeviceService em `stubs.cpp`, os 7 cherry-picks upstream) compilaram limpo, sem regressão.

Dois problemas de submodule encontrados e corrigidos no processo (specific ao clone fresco,
não afetam o código em si):
- `externals/protobuf` e mais 9 submodules ficaram com checkout incompleto (só o gitlink,
  sem arquivos) — resolvido com `git submodule update --init --force --depth 1 <path>` em
  cada um. Provável efeito colateral de uma tentativa anterior de rodar o `git submodule
  update` em background de forma errada (nohup dentro de `run_in_background`, que mascarou
  o processo real terminando cedo demais).
- `externals/discord-rpc/thirdparty/rapidjson` (submodule aninhado dentro de outro submodule)
  não inicializava a partir do diretório raiz do repo — precisou `cd` pra dentro de
  `externals/discord-rpc` e rodar `git submodule update --init` de lá.

**Nota sobre recursos da máquina**: `-j40` (usar todos os núcleos) causou swap thrashing
severo (RAM disponível na sessão: ~15GB, bem menos do que se esperaria da 7920) — cada unidade
de compilação C++ pesada (protobuf, abseil) usa bem mais que 1GB. `-j8` funcionou sem
travar, com uso de memória entre 60% e 95% mas sem swap crescendo de forma sustentada. Pra
builds futuros nesta máquina, começar direto com `-j8`.

## Jogo obtido, extraído e BOOTOU — 04/set/2026

Usuário tinha a base já baixada: `Gran Turismo Sport [BASE].pkg` em `/mnt/jogos`, 41GB,
**CUSA02168 (EU)**, APP_VER 01.00. Content ID `EP9001-CUSA02168_00-GTSPORT000000000` confirmado
via header e via `sfo-info`.

Checagem de integridade: tamanho bate quase exato com o dump de referência do archive.org
(item `PS4GranTurismoSportCUSA0216801GameFull`, upload de 2018) — 42.994.761.728 bytes local vs
42.994.696.192 bytes de referência, 64KB de diferença. **MD5 não bateu** (`17bf9cc4...` local vs
`d978caea...` referência) — não é lixo (header/content ID válidos, extração funcionou sem
erros), mas é uma cópia diferente da testada pelo archive.org, não bit-a-bit idêntica. Sem
sinal de problema até agora.

Patch v1.14 (MorpheusGames, mesmo item do archive.org) que o usuário tinha baixado estava
**incompleto**: 9,4GB local vs 21.260.664.832 bytes (19,8GB) de referência — MD5 de referência
`15feb55436ba78fff0b5032f343c8bc8`. Rebaixando de
`https://archive.org/download/PS4GranTurismoSportCUSA0216801GameFull/Gran%20Turismo%20Sport%20v1.14%20by%20MorpheusGames/PS4_Gran_Turismo_Sport_CUSA02168_02_Patch_v1.14.pkg`.

**Extração**: usando `seregonwar/shadPKG` (https://github.com/seregonwar/shadPKG), compilado
localmente em `/mnt/dados/Ferramentas/DevTools/shadPKG` (build Linux via Conan — não é
suportado oficialmente, só Windows/MSVC no README, mas o `build_linux.sh` do próprio repo
funciona). Extraído pra `/mnt/jogos/ShadPS4 Games/CUSA02168/` (41GB, ~1h de extração,
`shadpkg extract`). Confirmado o quirk de backport documentado antes: só veio
`app_param.sfo` no nível raiz, sem `sce_sys/param.sfo` — corrigido com `cp`.

**Teste de boot** (`./shadps4 --game "/mnt/jogos/ShadPS4 Games/CUSA02168"`, fork
GTSPORTHACKFIX): **funcionou muito além do esperado**. Vulkan inicializou completo (GPU real
desta máquina: AMD Radeon Pro VII via RADV, driver 26.2.1, Vulkan 1.4.354). Nenhum dos três
bugs de shader do relatório antigo (`StencilOp`, `vector_memory` offset, `GetFormat`
unreachable) apareceu — shaders compute/vertex/fragment e pipelines gráficos compilaram e
linkaram normalmente (`vk_pipeline_cache.cpp CompileModule`/`GetGraphicsPipeline`). Nosso
próprio fix do GT7 (`DeviceServiceGetEventStateStub`) disparou e funcionou certo aqui também.

**O jogo renderizou a UI real** — chegou ao diálogo de aviso de save online do GT Sport
("Games will only be saved in Gran Turismo Sport while online...") esperando clique em OK.
Screenshot salva em
`/tmp/claude-1000/.../scratchpad/gtsport-screenshot1.png` (sessão efêmera, não persiste).

Conclusão: **o trabalho de engenharia do shader recompiler que temíamos precisar não é mais
necessário** — pelo menos até este ponto do boot, tudo já funciona no shadPS4 upstream atual.

## Crash real encontrado e resolvido (save corrompido) — 04/set/2026

Na 2ª tentativa de boot (com DualSense já conectado via Bluetooth, reconhecido certo pelo
shadPS4 — `TryOpenSDLControllers: Gamepad registered for slot 0! Handle: 5`), o processo
avançou de verdade (várias alocações de memória direta, carregamento de assets) e então
**crashou** com:

    [stderr] TreeAllocator: node pool exhausted. can't continue.
    Core PatchesIllegalInstructionHandler: Failed to patch address f838ca -- mnemonic: ud2

"TreeAllocator" é print do próprio jogo (via passthrough de stderr), não do shadPS4 — algum
alocador interno em árvore do próprio GT Sport esgotou seu pool de nós, e o código do jogo
reagiu executando `ud2` (trap de abort deliberado). O `PatchesIllegalInstructionHandler` do
shadPS4 (`src/core/cpu_patches.cpp:2159`) tentou interceptar essa instrução ilegal pra tratar
como abort recuperável, mas falhou nesse endereço específico — e aí o processo morreu de
verdade (confirmado: exit code 133, sinal de instrução ilegal).

**Causa provável**: a tentativa 1 (parada no diálogo de aviso de save online) tinha sido
encerrada com `kill -TERM` no meio do processo, gerando um save em
`~/.local/share/shadPS4/home/1000/savedata/CUSA02168` potencialmente pela metade/corrompido.

**Fix**: apagar esse save (`rm -rf ~/.local/share/shadPS4/home/1000/savedata/CUSA02168`) e
relançar. **Resultado: sem crash, o jogo avançou até a tela de calibração de
display/exposição do primeiro boot, renderizando uma cena 3D completa e real** (pista de
Nürburgring, carros, árvores, iluminação) — bem além de qualquer menu, gameplay renderizado
de verdade. Screenshot em `gtsport-screenshot4.png`.

**Atualização: NÃO era save corrompido.** Apagamos o save e relançamos — o jogo avançou até a
tela de calibração de exposição, renderizando cena 3D completa (Nürburgring, carros, ver
`gtsport-screenshot4.png`), mas **crashou de novo** pouco depois, dessa vez sem deixar
mensagem de erro no log (stdout bufferizado, perdido na morte abrupta do processo).

**`coredumpctl` revelou que os dois crashes bateram no mesmo lugar exato**, backtrace
idêntico nas duas vezes:

    #0 unreachable_impl()
    #1 Core::SignalHandler(int, siginfo_t*, void*)

Ou seja: **não é aleatório, é 100% determinístico** — mesmo caminho de código toda vez.
Rastreado até `src/core/signals.cpp` (`Core::SignalHandler`): quando o `ud2` do jogo dispara
`SIGILL`, o shadPS4 tenta `DispatchIllegalInstruction` (o patch que já vimos falhando) e, se
falhar, tenta `thread->DispatchSignal(...)` (entregar o sinal pro handler do próprio jogo
PS4). Se as duas falharem, cai de propósito num `UNREACHABLE_MSG("Unhandled signal...")` —
fail-fast intencional do shadPS4, não bug aleatório de memória.

**Por que o patch falha**: `TryPatchJit`/`TryPatch` (`cpu_patches.cpp:613`) só sabe
"consertar" um conjunto específico de instruções x86 conhecidas por se comportar diferente
sob emulação (ex. `extrq`/`insertq`). `ud2` nunca esteve nessa lista — e não devia estar: é
um trap **intencional** que o próprio jogo colocou ali como abort deliberado, não uma
instrução "problemática" pra reescrever. A falha de patch é esperada e correta.

**Onde a bola cai de verdade**: em `thread->DispatchSignal(...)`, que deveria entregar esse
`SIGILL` pro handler de sinal do próprio jogo (se GT Sport registrou um, como jogos PS4
costumam fazer pra crashes tratados) ou, na falta de handler, terminar o jogo de forma
controlada — não derrubar o processo inteiro do shadPS4 com `UNREACHABLE_MSG`. Duas
possibilidades, não excludentes:

1. **Causa raiz é nossa**: alguma lacuna de emulação faz o `TreeAllocator` do próprio GT
   Sport esgotar nós quando não deveria (real hardware não crasharia aqui) — os dois crashes
   aconteceram durante trechos com bastante alocação/streaming de recurso repetindo em loop
   no log.
2. **Robustez do shadPS4 é insuficiente aqui**: mesmo que o jogo realmente aborte por conta
   própria (o que aconteceria em hardware real também), o shadPS4 deveria tratar isso como
   "o jogo crashou" e não como "o emulador crashou" — `DispatchSignal` falhando é o gap.

Ainda não decidido se vale a pena perseguir (1), (2), ou nenhum dos dois por enquanto — é
engenharia real, mais fundo que os fixes pontuais do GT7. Reproduzível de forma consistente,
então dá pra investigar com calma quando fizer sentido.

## Fix de (2) implementado e validado — 04/set/2026

Fomos atrás de (2): `src/core/libraries/kernel/threads/pthread.cpp`, função `SigDflHandler`.

**Tentativa 1 (incompleta)**: só trocar `return false` por `return true` no caso
`POSIX_SIGILL`, com log de aviso. Rebuild, testou: **suprimiu o crash** (confirmado: o
processo sobreviveu, mensagem de log apareceu), mas sem avançar o RIP a mesma instrução
`ud2` re-executa imediatamente — thread entrou num **loop de SIGILL em altíssima
velocidade**, ~1GB de log em menos de um minuto. Matamos o processo, limpamos o log.

**Tentativa 2 (funcionou)**: `ud2` é sempre 2 bytes (`0F 0B`). Mudamos a assinatura de
`SigDflHandler` pra receber o `Ucontext*`, e no caso `SIGILL` avançamos
`context->uc_mcontext.mc_rip += 2` e chamamos `context->SyncHostFromGuest()` antes de
retornar `true` — sem isso o `DispatchSignal` retorna cedo demais e nunca sincroniza o RIP
corrigido de volta pro contexto real do SO (só a rota do handler customizado do jogo fazia
esse sync). Call site também atualizado (`SigDflHandler(sig, context)`).

**Resultado**: rebuild limpo, relançado com o mesmo save que antes crashava consistentemente
(2 crashes confirmados, backtrace idêntico via `coredumpctl`). **Passou do ponto de crash
sem cair nenhuma vez em `SigDflHandler`'s SIGILL branch** (zero ocorrências da mensagem de
log), processo estável por 5+ minutos, sem coredump novo, sem loop. Chegou a um estado
parado esperando interação (tela preta com o mesmo padrão de polling idle que já tínhamos
visto na primeira tentativa de boot bem-sucedida) — devolvido pro usuário pra continuar com
os inputs.

**Nota (04/set, mais tarde)**: o teste que "passou sem crash" (attempt5) rodava sobre um save
que tinha sido corrompido por um `kill -KILL` anterior — o jogo ficou preso numa tela preta
por 5+ minutos sem renderizar nada (não era só "esperando input", era estado ruim). Com save
100% limpo de novo (attempt6), **o crash voltou, de forma totalmente determinística**:

1. `TreeAllocator` esgota de novo (confirma: não era corrupção de save, é sempre nesse ponto).
2. Primeiro `ud2` (endereço `f838ca`) — nosso fix suprime e avança RIP em 2 bytes, funciona
   como projetado.
3. **Tem um SEGUNDO `ud2` logo em seguida**, exatamente no próximo endereço (`f838cc` = 
   `f838ca` + 2) — suprimido também.
4. Poucos bytes depois disso, o código do jogo — agora rodando num estado que nunca deveria
   ter continuado, já que seu abort foi "enganado" duas vezes — bate numa violação de acesso
   de verdade: `Unhandled access violation at code address 0xf838fe: Read from address 0x14`
   (leitura de ponteiro praticamente nulo). Essa é OUTRO caminho do mesmo `UNREACHABLE_MSG`
   em `signals.cpp` (o de `SIGSEGV`/`SIGBUS`, não coberto pelo nosso fix — e não devia ser:
   suprimir uma violação de acesso de verdade às cegas seria bem mais arriscado que suprimir
   um `ud2` intencional). O processo inteiro crasha (confirmado via `coredumpctl`, novo PID).

**Conclusão honesta**: o fix de avançar RIP funciona exatamente como projetado — mas o
problema real (o `TreeAllocator` do próprio jogo esgotando) é uma condição genuinamente fatal
pra essa thread, não algo que dá pra "levar na esportiva". Continuar a execução depois de
suprimir só adia o crash por poucas instruções antes de virar um ponteiro inválido. Ainda
assim, o fix em si (avançar RIP após suprimir SIGILL sem handler guest, em vez de ficar
re-executando o mesmo `ud2` pra sempre) é uma correção de robustez genérica válida do
shadPS4 — só que sozinha não resolve esse crash específico do GT Sport.

O que resta pra investigar de verdade, se quisermos ir além:
- **Rota (1) de antes**: achar por que o `TreeAllocator` do jogo esgota nesse ponto — precisa
  investigação mais funda (o que exatamente esse alocador guarda, e o que a nossa emulação
  pode estar fazendo diferente do hardware real que causa esgotamento).
- Ou aceitar que esse caminho específico (tela de calibração de exposição no primeiro boot)
  não vai adiante por enquanto, e testar se o jogo tem algum outro fluxo que evite esse ponto
  (ex.: pular a calibração, ou ver se com o patch v1.14/debug menu aplicado o comportamento
  muda).

## Pesquisa externa (canal emudev2225) + engenharia reversa completa + patch de binário — 04/set/2026

Por pedido do usuário, pesquisei o canal do YouTube **emudev2225**, que tem um build
customizado do shadPS4 rodando GT Sport (progressão real: "Finally on track" dez/2025 →
"Debug Menu unlock all cars" jan/2026 → "complete circuit experience All Gold" jul/2026 →
"SPORT MODE ONLINE" — chegou a construir um **servidor privado próprio** simulando o backend
online morto, mesmo problema do GT7). **Sem documentação técnica pública** (sem descrições
úteis, sem transcrição, sem fork no GitHub) — só prova de que é possível chegar num nível
jogável, e mesmo assim "it currently crashes" nas palavras da própria pessoa, em qualquer
build. Não é um atalho, é confirmação de que o alvo é genuinamente difícil.

**Engenharia reversa própria (Ghidra) da função exata do crash**: usando `--dump-elf` do
nosso shadPS4 (mesmo fluxo do GT7) + Ghidra headless (~20min de análise real), achei via xref
da string do erro a função exata: `FUN_00b837d0` (eboot-runtime `0xb837d0`), um alocador de
árvore por tamanho com **limite fixo de 0x81 (129) unidades** antes de abortar:

    CMP RAX,0x81      ; 0xf837ff (48 3D 81 00 00 00) - eboot-runtime = Ghidra-vaddr + 0x400000
    ja   <abort>      ; aborta se RAX > 0x80

Só 4 funções chamadoras, 12 pontos de chamada no total — bem delimitado.

**Patch de binário via mecanismo nativo do shadPS4** (não editei o eboot.bin do usuário
diretamente — usei `MemoryPatcher`/`--patch <xml>`, o mesmo sistema de patches por
offset+valor que já existe no shadPS4 pra correções de compatibilidade por jogo). Arquivo em
`patches/CUSA02168_raise_treeallocator_cap.xml`: reescreve os 4 bytes do imediato em
`0xf83801` de `81 00 00 00` (0x81) pra `00 20 00 00` (0x2000, ~64x mais margem).

**Resultado do teste**: patch aplicado com sucesso (confirmado no log:
`"Applied patch: ... Offset: 0xf83801, Value: 00200000"`), e o abort do TreeAllocator
**realmente sumiu** (zero ocorrências em 239 mil linhas de log, contra sempre-determinístico
antes). Mas o jogo **trava logo depois**, mostrando "Saving" na tela pra sempre, sem reagir a
nenhum input do usuário (confirmado: apertou botão, nada mudou). Investigação confirmou que
os arquivos de save (`local-data`, `sce_sdmemory`) **não foram tocados** durante essa rodada
(timestamps de uma sessão anterior) — ou seja, o indicador "Saving" é um estado de UI
travado, não uma gravação real em andamento. Conclusão: **removemos um abort e expusemos um
travamento silencioso logo em seguida** — não é uma correção completa, só empurrou o mesmo
problema de lugar (crash rápido e diagnosticável → travamento silencioso e pior de detectar).

**Estado atual**: patch mantido no repo (`patches/CUSA02168_raise_treeallocator_cap.xml`,
`isEnabled="true"`) pra referência, mas **não recomendado usar por padrão** — troca um
problema conhecido por outro pior. Precisa decidir: investigar o próximo travamento (mesmo
padrão de trabalho, mais uma rodada de Ghidra), reverter e aceitar o crash do TreeAllocator
como bloqueio conhecido, ou testar se a v1.14/outra versão do jogo evita esse trecho de
código inteiramente.

## Pendências

- Interagir com o diálogo de confirmação e ver até onde o jogo vai (menu principal? consegue
  entrar numa corrida offline/Arcade?) — deixando os inputs por conta do usuário.
- Terminar de rebaixar o patch v1.14 corretamente (MD5 `15feb55436ba78fff0b5032f343c8bc8`) e
  aplicar por cima da base, pra então tentar o patch de debug menu do illusion.
- Decidir rota do patch de debug menu (backport FPKG do illusion vs. portar cheat
  YAML/GoldHen manualmente) — agora que o jogo básico já roda, isso volta a ser prioridade.
