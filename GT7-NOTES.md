# GT7 PC compat — notas do projeto

Leia isto antes de mexer no repo em qualquer máquina nova. Espelha o padrão do
`CONTEXTO7920.md` da 7920 (que este projeto não faz parte — repo separado por design,
para poder ser clonado em qualquer máquina, Windows incluído).

## O que é isto

Fork privado do [shadPS4](https://github.com/shadps4-emu/shadPS4) (`origin` continua
apontando pro upstream público) para trabalhar em compatibilidade do **Gran Turismo 7**
(CUSA24767), jogo próprio, cópia legal. Estado em 2026-09-01: **jogo instalado, bootando e
jogável** — primeira modificação de código real é o fix experimental de memória descrito
abaixo (`src/core/memory.cpp`), ainda sem commit próprio no fork.

## Bloqueio do boot — RESOLVIDO em 2026-09-01

**GT7 chega no menu e é jogável** (confirmado, controle DualSense por cabo USB — Bluetooth
não funcionava por um kernel desatualizado sem módulos, `hid-playstation` não carregava; ver
seção de pendências). Fix em `src/core/memory.cpp`, função `UnmapBytesFromEntry` (dentro de
`MemoryManager::Free`): quando `sceKernelReleaseDirectMemory` libera memória do tipo
`VMAType::Direct`, o código original chamava `impl.Unmap(...)`, que remapeia a região como
`PROT_NONE` (`mmap(..., MAP_FIXED)` em `address_space.cpp`). O GT7 tem o hábito de, na mesma
thread, escrever alguns bytes dentro da região que acabou de liberar logo em seguida — trava
imediato com `SIGSEGV`. Real hardware PS4 aparentemente tolera isso (já tinha um comentário
parecido — "on PS4, protecting freed memory does nothing" — em `ProtectBytes`, mesmo
arquivo). **Fix**: depois do `impl.Unmap`, chamar `impl.Protect(virtual_addr, size_in_vma,
Core::MemoryPermission::ReadWrite)` só para `VMAType::Direct` — mantém a região válida
(legível/gravável) em vez de vira guard page, sem quebrar remapeamento futuro (um
`MAP_FIXED` posterior substitui limpo). Marcado como EXPERIMENTAL no código-fonte — não é
correção "de verdade" da causa raiz (ainda não sabemos por que o GT7/shadPS4 deixa isso
acontecer), é um contorno de compatibilidade, do jeito que emuladores fazem esse tipo de
coisa o tempo todo. Funcionou na prática: sem esse fix, sempre trava antes de qualquer
janela abrir; com ele, o jogo roda.

**Histórico da investigação** (contexto de como chegamos aqui, útil se o bug voltar de outra
forma):

O bloqueio antigo (`libSceAudio3d` não implementada — issues
[#528](https://github.com/shadps4-compatibility/shadps4-game-compatibility/issues/528),
[#2290](https://github.com/shadps4-compatibility/shadps4-game-compatibility/issues/2290),
[#3004](https://github.com/shadps4-emu/shadPS4/issues/3004)) já foi resolvido upstream —
não é mais o que trava.

**Bloqueio atual (achado em 2026-09-01, build local do `main`, rev `94b40b88`, GPU AMD
Radeon Pro VII via RADV):** o jogo carrega o `eboot.bin`, inicializa a instância Vulkan e o
device, começa a chamar libs HLE (Pad, Font, SaveData, GameLiveStreaming/SharePlay — essas
stubadas, esperado) e mapeia/desmapeia memória direta normalmente — **nenhuma janela chega
a abrir**. Trava com:

    [Debug] <Critical> signals.cpp:279 SignalHandler: Unreachable code!
    Unhandled access violation at code address 0x809ad132: Write to address 0xf419a44020

Ou seja, código guest (dentro do próprio binário do GT7, não HLE) tenta escrever num
endereço de memória guest não mapeado. Depois desse erro o handler de crash do shadPS4 não
finaliza limpo — o processo fica girando (~90% CPU, 176 threads) e precisa `kill -9`.

**Pista forte, não só "endereço inválido" genérico:** o endereço da escrita (`0xf419a44020`)
cai *dentro* da faixa que o próprio log mostra sendo desmapeada um instante antes:

    Free: Unmapping direct mapping 0xf419200000 with size 0xc00000   [-> 0xf419200000-0xf419e00000]
    Unhandled access violation ... Write to address 0xf419a44020      [dentro dessa faixa]

Cheira a **use-after-free entre threads** na memória direta (`sceKernelReleaseDirectMemory`
seguido de escrita de outra thread na mesma região) — não "lib faltando". A lógica de
liberação está em `MemoryManager::Free` (`src/core/memory.cpp:286`), já usa
`unmap_mutex`/`mutex` (não é óbvio de cadê a race só de ler). O timing do crash bate com um
`sceFontMemoryInit` disparado em várias threads de job (`Job#10`, `Job#6`, `Job#60`,
`Job#14`) quase simultâneas, junto com o ciclo alocar→mapear→liberar→desmapear de memória
direta — pode ser um HLE de sincronização (mutex/semáforo) implementado errado, fazendo o
próprio jogo liberar memória cedo demais.

**ThreadSanitizer tentado em 2026-09-01 e não serve pra este projeto.** Build separado em
`build-tsan/` (`-fsanitize=thread -O2`, precisou `-O2` em vez de `-O1` por causa de um bug
do GCC 16 com função `always_inline` chamada por ponteiro no `externals/aacdec` — erro
"indirect function call with a yet undetermined callee"). Rodando o jogo sob TSan, o
`Core::AddressSpace::Impl::Impl()` (`address_space.cpp:682`) lança exceção **antes mesmo de
carregar o eboot** — a faixa de endereço virtual gigante que o emulador reserva pro espaço
guest (`0x1000000000`-`0x54ffffffffff`, ~93TB, visto no log normal) colide com a shadow
memory que o próprio TSan reserva. A exceção não tratada cai no terminate handler do
shadPS4, que por sua vez tenta resetar a luz dos controles (`ResetLightbarColors`,
`controller.cpp:248`) antes do subsistema de input existir — crash dentro do crash. **Não é
contornável por flag de build** — precisaria reescrever como o `AddressSpace` reserva
memória. Não tentar TSan de novo nesse projeto sem resolver isso primeiro.

Diante do TSan não servir, o fix que resolveu foi o pragmático (`impl.Protect` em vez de
`impl.Unmap`, descrito no topo desta seção) em vez de achar a causa raiz exata — se quiser
mesmo entender o "porquê", symbolizar `0x809ad132` contra o `eboot.bin` (offset de módulo ≈
`0x59ad132`) ou logar por thread em `MemoryManager::Free` continuam válidos como próximo
passo, mas não são mais bloqueio pra jogar.

(`build-tsan/`, ~3,8GB, já foi apagado — não servia pra mais nada depois da descoberta
acima.)

Log completo dessa tentativa (útil pra não repetir do zero): `gt7-run-2026-09-01.log`,
nesta mesma pasta.

## Armadilha de clone: submódulo `mesa-kosmickrisp` quebra o clone recursivo inteiro

`git clone --recursive` (ou `git submodule update --init --recursive` na raiz) **falha
silenciosamente no meio** por causa de `externals/mesa-kosmickrisp/externals/mesa` — um
submódulo aninhado fixado num commit específico do GitLab (`shallow = true` no
`.gitmodules` dele). O git tenta um shallow-fetch desse SHA e erra:

    fatal: Unable to find current revision in submodule path 'externals/mesa-kosmickrisp/externals/mesa'
    fatal: Failed to recurse into submodule path 'externals/mesa-kosmickrisp'

Isso **aborta o resto da recursão**, deixando dezenas de outros submódulos (viu-se:
`protobuf`, `zstd`, `zlib-ng`, `zarchive`, `spdlog`, `sdl3`, `vulkan-headers`, `vma`,
`robin-map`, `xbyak`, `toml11`, `zydis`, `sirit`, `tracy`, `pugixml`, `miniz`,
`openal-soft`, entre outros) como diretórios vazios — e cada um quebra o `cmake configure`
por vez, um erro de cada vez, parecendo um problema diferente a cada tentativa.

**Correção**: inicializar tudo *exceto* `mesa-kosmickrisp` primeiro, depois tratar esse à
parte, buscando o commit fixo manualmente (GitLab aceita fetch por SHA arbitrário; o que
falha é a lógica de shallow-submodule do git, não o servidor):

    git submodule status | awk '{print $2}' | grep -v mesa-kosmickrisp | \
        xargs git submodule update --init --recursive --

    cd externals/mesa-kosmickrisp
    git submodule init externals/mesa
    rm -rf externals/mesa/.git; mkdir -p externals/mesa
    cd externals/mesa
    git init -q
    git remote add origin https://gitlab.freedesktop.org/mesa/mesa.git
    git fetch --depth 1 origin <SHA_fixado_no_.gitmodules_do_mesa-kosmickrisp>
    git checkout FETCH_HEAD

(Pegar o SHA atual com `git ls-tree HEAD externals/mesa-kosmickrisp` dentro de
`externals/mesa-kosmickrisp` — ou já vem resolvido no `git submodule status` daquele
diretório, coluna 1.)

Depois disso, `git submodule status` na raiz não deve ter nenhuma linha começando com
espaço-menos (`-`, não inicializado) nem `+` (commit divergente) — só linhas em branco no
início (inicializado e correto).

## shadNet: login funciona, mas o GT7 online não — servidores da Polyphony, não da PSN

Ativado `shad_net_enabled` + `connected_to_network` em `config.json`, e credenciais reais
(`shadnet_npid`/`shadnet_password`, conta criada em https://shadps4.net/) em
`~/.local/share/shadPS4/users.json` pro usuário `1000`. **O login na PSN genérica funciona**
— log confirma `Logged in npid='...' accountId=...` contra o servidor público
`srv.shadps4.net:31313`. Mas o GT7 mostra `Failed: An unexpected error has occurred:
CE-210716` (duas vezes, uma por serviço) e volta pro loading infinito depois de fechar o
diálogo.

**Causa: são coisas diferentes.** O shadNet emula a camada genérica da PSN (login, lista de
amigos, troféus — essa parte funciona). O GT7 também fala com os **servidores próprios da
Polyphony Digital** (matchmaking de corrida, garagem online, GT Sophy etc.) — infraestrutura
exclusiva do jogo, shadNet não tem/não emula isso, é um sistema totalmente separado. Não é
config faltando; é um serviço que não existe pra conectar. Bater a cabeça em mais ajuste de
shadNet não resolve isso — precisaria de um servidor dedicado pro protocolo do GT7
especificamente (bem provavelmente o que o nome do projeto, GT7-PC-Recomp, sempre quis dizer
— não tentado, escopo bem maior que uma sessão).

**Bug real encontrado e corrigido no caminho**: `sys_socketclose` (`src/core/libraries/
network/sys_net.cpp`) só sabia fechar sockets, não epoll — cada tentativa de conexão que
falhava vazava um epoll inteiro (o jogo chama fechar genérico nos dois tipos, igual BSD/PS4
real faz, mas só checávamos a tabela de socket). Sem esse fix, cada retry de conexão vazava
memória sem limite (visto: ids de epoll subindo de 12401 até 12406+ em segundos, RAM
caindo rápido). Com o fix, os retries reusam os mesmos ids de socket (42/43) sem vazar — o
jogo ainda não consegue conectar (motivo acima), mas pelo menos não derruba a máquina
tentando. Fix aplicado, funcionando, não commitado ainda.

**Resultado prático**: pra jogar agora, deixar `shad_net_enabled = false` e
`connected_to_network = false` de novo — evita o diálogo de erro e o loop de retry. Só
ligar se for investigar servidor dedicado do GT7 especificamente.

## Music Rally trava a GPU — CUIDADO, já derrubou o Claude Desktop por OOM duas vezes

Com o jogo offline (shadNet desligado), o GT7 manda pro **Music Rally** — isso é
comportamento real do jogo em hardware de verdade também, não bug nosso: a Polyphony
adicionou esse modo em 2022 especificamente pra quando não dá pra conectar aos servidores
(era a única coisa jogável no lançamento sem internet).

Duas travadas distintas encontradas tentando abrir uma pista do Music Rally, cada uma **com
o processo comendo memória sem limite até o OOM killer intervir** (uma vez chegou a derrubar
o Claude Desktop inteiro, não só o jogo — os dois processos compartilhavam cgroup por eu ter
lançado o shadps4 através do Claude Code):

1. **Opcodes de shader de dupla precisão faltando** — `V_MIN_F64` e `V_TRUNC_F64`
   (categoria VectorALU) não estavam implementados no recompilador
   (`src/shader_recompiler/frontend/translate/vector_alu.cpp`), travando a compilação do
   compute shader do Music Rally. **Corrigido** — os dois já tinham irmãos implementados do
   lado (`V_MAX_F64`, `V_FLOOR_F64`) que serviram de modelo exato, foi só espelhar com
   `ir.FPMin`/`ir.FPTrunc` (ambos já aceitam F64 no builder, não precisou mexer no IR).
   Commitado.

2. **Modo de tiling PRT faltando — CORRIGIDO** —

       [Debug] <Critical> image_info.cpp:202 UpdateSize: Unreachable code!
       Unknown array mode ArrayPrt2DTiledThin1

   `image_info.cpp` (`ImageInfo::UpdateSize`) só tratava `Array1DTiledThin1/Thick` e
   `Array2DTiledThin1/Thick` — nenhuma das 6 variantes `ArrayPrt*` (Partially Resident
   Texture) do enum `AmdGpu::ArrayMode` (`src/video_core/amdgpu/tiling.h`). PRT só muda como
   o driver comita páginas pra residência esparsa — o cálculo de pitch/height/size do texel é
   idêntico ao modo não-PRT correspondente (confirmado: `tiling.cpp`, que já trata os 16
   valores do enum sem lacuna, usa a mesma lógica de endereçamento pros dois). Adicionado
   `ArrayPrtTiledThin1`/`ArrayPrtTiledThick` ao lado de `Array1DTiledThin1`/`Thick`
   (micro-tiled) e `ArrayPrt2DTiledThin1`/`ArrayPrt2DTiledThick` ao lado de
   `Array2DTiledThin1`/`Thick` (macro-tiled). **Não** mexi nas variantes 3D/XThick/Prt3D —
   sem caso confirmado forçando elas ainda, e menos confiança de que mapeiam limpo nos
   helpers existentes; deixei cair no `UNREACHABLE_MSG` se aparecer, que pelo menos avisa
   claro em vez de arriscar tamanho de imagem errado silenciosamente. Commitado.

3. **Tentado, NÃO VERIFICADO — commit `86e85cd7` marca isso explicitamente**:

       [Debug] <Critical> vector_interpolation.cpp:101 V_INTERP_MOV_F32: Assertion Failed!

   `ASSERT(attr.is_flat || inst.src[0].code == 2);` em
   `src/shader_recompiler/frontend/translate/vector_interpolation.cpp:101` — o GT7 chama
   `V_INTERP_MOV_F32` com `src[0].code` 0 ou 1 (P10/P20, os parâmetros de interpolação bruta)
   numa attribute que não é flat, e o assert só permitia isso pra code==2 (P0). O código
   *depois* do assert (`ir.GetAttribute(attrib, chan, (code+1) % 3)`) já trata os três
   valores de code corretamente via rotação **quando** `profile.supports_amd_
   shader_explicit_vertex_parameter` ou `supports_fragment_shader_barycentric` está
   disponível — confirmado que a primeira está habilitada nessa GPU (RADV, via
   `VK_AMD_shader_explicit_vertex_parameter` na lista de extensões do boot). Relaxei o
   assert pra só bloquear quando *nenhuma* das duas está disponível (é aí que o fallback
   `Flat` ignora `code` e ficaria errado de verdade).

   **Resultado do teste**: passou do assert, mas travou de novo mais à frente — **sem
   nenhuma mensagem de erro dessa vez**, silencioso, no meio de `Compiling vs shader ...
   (permutation)` / `GetGraphicsPipeline`, com aviso de `page_manager.cpp` sobre memória
   "not fully GPU mapped". Não dá pra saber com certeza se é um bloqueio novo genuíno (mais
   provável — chegamos muito mais longe, de "não compila o shader" pra "compila e tenta
   montar o pipeline gráfico") ou se o fix do assert causou isso. Não investigado mais a
   fundo ainda — precisaria comparar rodando com e sem o fix pra isolar, ou adicionar
   logging no meio da compilação de pipeline.

**Nas três vezes que travou, o processo comeu memória sem limite até precisar `kill -9`**
— confirmar RAM livre e swap baixo antes de testar de novo, e não deixar rodando sem
monitorar. Ver seção seguinte sobre isolamento por cgroup, que resolve o *sintoma* (derrubar
o sistema inteiro) independente de achar a causa raiz do crescimento.

## Isolamento de memória: cgroup + zram/swap extra (2026-09-01)

Depois do processo do jogo derrubar o **Claude Desktop inteiro** via OOM (não só o jogo —
os dois compartilhavam cgroup, porque eu tinha lançado o shadps4 através do Claude Code),
duas mitigações:

1. **Sempre lançar o jogo dentro de um cgroup com limite próprio**, assim se ele vazar
   memória de novo, só ele morre:

       systemd-run --user --scope -p MemoryMax=8G -p MemoryHigh=6G -- \
         env SDL_VIDEODRIVER=x11 ./shadps4 -g "<caminho do eboot.bin>"

   `MemoryHigh` faz o kernel jogar esse cgroup específico pro swap agressivamente acima de
   6G (throttling suave); `MemoryMax` é o teto duro — passou disso, o cgroup recebe SIGKILL
   sozinho, sem acionar o OOM killer global. Testado (`systemd-run --user --scope -p
   MemoryMax=8G ... echo` funciona), mas **ainda não testado rodando o jogo de verdade**
   dentro dele — próxima sessão, usar sempre esse wrapper em vez de `nohup ... &` direto.

2. **zram configurado, mas só ativa após reboot** — kernel rodando (`7.1.9-arch1-2`) não
   tem mais `/usr/lib/modules/` (já trocado pra `7.2.2-arch1-1`/`7.2.2-zen1-1-zen` num
   `pacman -Syu` anterior — mesmo motivo do Bluetooth do DualSense não parear, ver seção
   abaixo). `zram-generator` instalado, config em `/etc/systemd/zram-generator.conf`
   (`zram0`, `min(ram/2, 8192)`, zstd, prioridade 200 — mais alta que o swap do Optane, pra
   ser usado primeiro). Enquanto isso, criei um swapfile temporário de 6G em `/swap/swapfile`
   (raiz, disco único — **não** deu em `/mnt/dados`, btrfs RAID-0 rejeita swapfile
   multi-disco: `swapon falhou: Argumento inválido`). Esse swapfile não está no `/etc/fstab`
   de propósito (é só ponte até o reboot); depois que o zram estiver confirmado funcionando,
   pode apagar (`swapoff /swap/swapfile && rm /swap/swapfile`).

**Reboot decidido em 2026-09-01** pra resolver os dois de uma vez (zram real + módulo
`hid-playstation` do DualSense). O usuário pediu pra eu reiniciar a máquina; a sessão do
Claude Code **não volta sozinha** (sem autostart configurado) — precisa reabrir o Claude
Desktop manualmente depois, a conversa deve retomar via `--resume` mas não é automático.

## DualSense por Bluetooth não pareia — pendente reboot

Tentar parear o DualSense (`44:46:48:EA:FF:CB`) por Bluetooth falhava
(`org.bluez.Error.Failed br-connection-create-socket`). Causa: a 7920 tinha atualizado o
kernel (`pacman -Syu`) mas não tinha reiniciado — `uname -r` reportava `7.1.9-arch1-2`, mas
`/usr/lib/modules/` só tinha `7.2.2-arch1-1` e `7.2.2-zen1-1-zen` (o pacman já tinha
apagado os módulos do kernel antigo, que é o que ainda estava rodando). Sem o diretório de
módulos do kernel corrente, `hid-playstation` não carrega — o DualSense aparece no `lsusb`
mas não vira dispositivo de input (`/proc/bus/input/devices` não lista ele). Só resolve com
reboot. Contorno usado: **DualSense por cabo USB funciona normal** (não depende de
`hid-playstation` da mesma forma — ou usa outro driver de fallback, não confirmado). Mapa de
teclado padrão do shadPS4 pra esse jogo fica em
`~/.local/share/shadPS4/input_config/default.ini` (por jogo) e `global.ini` (atalhos
gerais), se precisar jogar sem controle nenhum.

## Build no Windows

Dependências (ver `documents/building-windows.md` no repo): Visual Studio 2022 com C++ e
Clang/LLVM (o projeto recomenda Clang, não MSVC puro), CMake, Qt6, Vulkan SDK. Devem ser
as mesmas etapas que no Linux (`cmake -S . -B build/`, depois `cmake --build build/`) —
mas o `.gitmodules` da armadilha acima independe de SO, então espere o mesmo problema lá.

## Notas da build na 7920 (Arch) — para referência, não se aplicam ao Windows

- Pacotes que faltavam: `vulkan-validation-layers` (sem conflito). **Não** instalar
  `jack2` sem checar antes — conflita com `pipewire-jack`, que a 7920 usa pro áudio do
  desktop.
- A 7920 tem só ~15GB RAM e estava com Overwatch aberto durante o teste de build — build
  paralela (`-j40`, todos os núcleos) quase certamente estoura RAM/swap nessa máquina.
  Usar `-j` baixo (2-4) ou fechar outros programas antes. Isso é específico da 7920, não
  do Windows — ver RAM disponível lá antes de escolher `--parallel`.

## Estado do fork

`origin` = `https://github.com/shadps4-emu/shadPS4.git` (upstream público, só leitura por
enquanto). `github` (ou o remote que você configurar) = este repo privado. Sem commits
próprios ainda — HEAD = upstream `main` no momento da cópia.
