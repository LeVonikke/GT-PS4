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

## O fix experimental (`impl.Protect` após `impl.Unmap`) é um no-op no Windows — tentativa de corrigir, revertida

**No Windows, o fix acima não faz nada de verdade.** `Unmap()` no Windows (`address_space.cpp`,
`AddressSpace::Impl::Unmap`, ramo `#ifdef _WIN32`) marca a região como `is_mapped = false`
antes de retornar. O `Protect()` do Windows, logo depois, **pula silenciosamente qualquer
região com `is_mapped == false`** (`if (!it->second.is_mapped) { continue; }`) — ou seja,
`impl.Protect(virtual_addr, size_in_vma, ReadWrite)` chamado logo após o `Unmap` nunca
protege nada ali, porque o próprio `Unmap` já desmarcou a região um instante antes. No
Linux/Mac, `mprotect()` não olha pra essa flag de tracking, por isso o fix funciona lá mas é
inerte no Windows — testado em 2026-09-02: 3 de 4 lançamentos no Windows crasharam no mesmo
`0x809ad132`, contra "funciona na prática" reportado do lado Linux.

**Tentativa de corrigir (revertida)**: criei `AddressSpace::UnmapAndKeepAccessible()`, uma
função nova que, no Windows, comita memória anônima real (`VirtualAlloc2` com
`MEM_REPLACE_PLACEHOLDER | MEM_COMMIT`, reaproveitando `MapRegion`) em vez de deixar a
região como placeholder inacessível — e no Linux/Mac só chama `Unmap()`+`Protect()` (que já
funciona lá). **Quebrou pior**: o Windows gerencia VA como *placeholders* que precisam ficar
consistentes (tamanho exato, sem "ilhas" de memória comitada permanentemente no meio de uma
região que o resto do código espera continuar como placeholder livre). Deixar aquela região
comitada pra sempre fragmenta esse sistema — quando o jogo pede uma alocação maior que
precisa fazer `SplitRegion` atravessando essa "ilha", bate um assert determinístico:

    [Debug] <Critical> address_space.cpp:320 SplitRegion: Assertion Failed!
    Cannot fit region into one placeholder

Isso trava **antes** até da tela "Sony Presents" (pior que o bug original, que pelo menos
às vezes deixava rodar). **Revertido** (`git checkout -- src/core/address_space.cpp
src/core/address_space.h src/core/memory.cpp`) — o fix original (inerte no Windows, mas não
piora nada) é o mal menor por enquanto.

**Se for tentar de novo**: a versão comitada-pra-sempre não serve; precisaria de algo tipo
recomitar a região por um período curto (não indefinido) e reverter pra placeholder de novo
depois — talvez via um work item assíncrono adiado, ou investigar se dá pra interceptar só a
falha de acesso (um handler de exceção específico pra essa faixa de endereço, tipo trap
handler, em vez de mexer no estado da região). Nenhuma das duas foi tentada ainda. Rodar o
jogo pelo lado Linux/Arch continua sendo a opção mais estável enquanto isso não é resolvido.

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

   **Resultado do teste (na hora)**: passou do assert, mas travou de novo mais à frente —
   sem nenhuma mensagem de erro naquele momento, silencioso, no meio de `Compiling vs
   shader ... (permutation)` / `GetGraphicsPipeline`. **Confirmado depois do reboot (ver
   item 4): era mesmo um bloqueio novo genuíno, não efeito colateral do fix** — rodando de
   novo chegou bem mais longe (compilando pipeline gráfico completo, várias texturas/vertex/
   fragment/hull/local shaders) antes de bater no bloqueio seguinte. O fix do assert fica
   confirmado como correto.

4. **Parede de verdade, bem maior que os fixes anteriores — não tentado**:

       [Debug] <Critical> resource_tracking_pass.cpp:410 FindSharpSources: Unreachable code!
       Bindless sharp access detected pc=0x0

   Diferente dos 3 anteriores (sempre "falta um caso no switch, tem irmão pra copiar"),
   esse é **acesso bindless a recurso** — o shader carrega o descritor de textura/buffer
   (`ReadConstBuffer`) dinamicamente em tempo de execução em vez de referenciar um slot fixo
   conhecido em tempo de compilação. `FindSharpSources` (`src/shader_recompiler/ir/passes/
   resource_tracking_pass.cpp`) tenta rastrear a origem estática do descritor andando pelos
   nós Phi do IR; quando não acha nenhuma e viu um `ReadConstBuffer` no caminho, conclui
   bindless e desiste — porque **não existe nenhuma infraestrutura de bindless no shadPS4**
   (`grep -rl bindless src/` só acha esse arquivo, o resto do projeto não tem nada). Resolver
   de verdade precisaria de indexação dinâmica de descritor (`VK_EXT_descriptor_indexing`),
   mudança no IR pra representar "índice de recurso desconhecido", e geração de SPIR-V
   correspondente — trabalho de dias de um dev de emulador de verdade, não uma tarde. **Não
   tentei mexer nisso.**

**Music Rally provavelmente está travado em cima disso especificamente** — vale testar se
**Arcade Mode** (também jogável offline em hardware real, confirmado por pesquisa web) usa
os mesmos shaders/pipeline ou se escapa desse bloqueio.

**Nas travadas repetidas, o processo comeu memória sem limite até precisar `kill -9`** —
confirmar RAM livre e swap baixo antes de testar de novo, e não deixar rodando sem
monitorar. Ver seção seguinte sobre isolamento por cgroup, que resolve o *sintoma* (derrubar
o sistema inteiro) independente de achar a causa raiz do crescimento — **testado e
funcionando** depois do reboot: com `systemd-run --user --scope -p MemoryMax=8G -p
MemoryHigh=6G`, a trava do bindless empurrou memória pro swap (zram) em vez de estourar o
sistema, RAM disponível ficou em ~3-4GB o tempo todo. Sempre lançar assim.

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

## GT7 offline = só Music Rally, sem exceção — e o menu de debug como saída possível

Confirmado navegando de verdade (2026-09-03, pós-reboot): **offline, o GT7 não tem menu
principal nenhum** — vai direto pro Music Rally e não tem botão de voltar. Isso bate com o
que a Polyphony fez de propósito (matéria do lançamento de 2022), não é falha da nossa
emulação. Login genérico de PSN via shadNet não muda isso — o GT7 checa os servidores
*dele*, não a PSN genérica.

Duas saídas possíveis, nenhuma tentada até o fim:

1. **Servidor falso pro protocolo específico do GT7** — não é o shadNet (isso já
   funciona, é outra coisa: os servidores da Polyphony, protocolo proprietário deles).
   Escopo enorme, exigiria capturar/deduzir o protocolo real do zero.

2. **Menu de desenvolvedor** — existe de verdade, compilado no binário (não removido pro
   retail). `strings` no `eboot.bin` acha **637 ocorrências** de "debug", incluindo nomes
   muito promissores: `IsDebugVersion`, `IsDebugSettingAvailable`,
   `IsDebugSettingEnable`, `DebugFeatures`, `CourseDebugMode` (essas soam exatamente como
   o gate que decide se o menu de debug aparece). Guias públicos de destravar isso
   existem, mas são pra build v1.18/firmware 9.00 — a nossa é **01.55**, bem mais antiga,
   offset diferente, precisa ser redescoberto.

   **Ferramenta nova criada pra isso**: `--dump-elf <saída>` no `shadps4` (junto com
   `-g <eboot>`) extrai o ELF plano de dentro do wrapper SELF, pronto pra `readelf`/
   `objdump`/Ghidra. Funciona porque os segmentos desse dump específico **não estão
   criptografados nem comprimidos** (`Elf::LoadSegment` do próprio shadPS4 já não
   descriptografa nada — só lê bytes crus — o que confirmou isso). Testado, gera ELF
   válido (`readelf -h` reconhece: ELF64 x86-64 FreeBSD, 12 program headers, 2 PT_LOAD
   extraídos, ~82MB). Commitado, reaproveitável pra qualquer sessão futura:

       ./shadps4 --dump-elf gt7-eboot-plain.elf -g "<caminho>/eboot.bin"

   **Update 2026-09-03, à mão sem Ghidra:** localizei de verdade 3 das 5 strings de debug
   via busca por instrução `lea reg64, [rip+disp32]` cujo alvo calculado bate com o offset
   da string (script python, varre os ~517k candidatos `lea`, filtra por ModRM
   rip-relative). Confirmado com `objdump -D -b binary -m i386:x86-64 -M intel` no entorno
   de cada acerto — são instruções reais, não coincidência de bytes em dados:

   | string | vaddr (no ELF extraído) | endereço da instrução `lea` que referencia |
   |---|---|---|
   | `IsDebugVersion`     | `0x3eded44` | `0x1d61550` |
   | `DebugFeatures`      | `0x3d10840` | `0x10ec0d7` |
   | `CourseDebugMode`    | `0x3efa3d9` | `0x236e0ba` |
   | `IsDebugSettingAvailable` | `0x3b0ce5f` | não achado (não referenciada por `lea` direto — talvez só via `mov`/tabela de ponteiros em `.data`, não procurei isso ainda) |
   | `IsDebugSettingEnable`    | `0x3b0b9f9` | idem |

   Em `0x1d61550` o padrão é bem claro: uma sequência repetida de
   `lea rsi,[string]; lea rdx,[outra string vizinha]; mov rdi,r14; call 0xb49700`, uma vez
   por string, para várias strings de debug seguidas (`0x3eded34`, `0x3eded44` =
   `IsDebugVersion`, `0x3eded53`, ...). Isso tem cara de **registro de uma tabela de
   propriedades/itens de debug** no construtor de algum objeto de configuração — não é a
   função `IsDebugVersion()` em si, é o código que a *cadastra* em algo (uma tabela de
   settings, possivelmente o que alimentaria um debug menu). `0x236e0ba` (`CourseDebugMode`)
   tem o mesmo padrão.

   **Update:** desmontei `0xb49700` à mão (real offset no eboot.bin: `0xb6ffa0`, ~350
   bytes). Reconheço o padrão com bastante confiança: é um **"magic static"** clássico do
   Itanium C++ ABI — guarda de inicialização de variável estática local (`test cl,cl` num
   byte-flag em `0x60674a8`, `call __cxa_guard_acquire/release/abort` nos endereços
   `0x3ac5360/0x3ac5330/0x3ac5340/0x3ac5350`), guardando um `weak_ptr`/`shared_ptr` estático
   em `0x60673b0` com contagem de referência atômica (`lock xadd`/`lock inc`). Ou seja:
   `0xb49700` é uma função `GetOrCreate(nome, valor)` tipo *lazy singleton registry* — pega
   (ou cria na primeira chamada) uma propriedade nomeada num registro global e devolve um
   ponteiro compartilhado pra ela. **Não é a checagem `IsDebugVersion()` em si** — é só o
   cadastro. A checagem de verdade (onde alguém lê o valor dessa propriedade e decide
   habilitar o menu de debug) fica em outro lugar, ainda não localizado — provavelmente
   longe do site de registro, chamado só quando o menu tentaria abrir. Achar isso por
   desmontagem crua deixou de compensar (sem tipos/grafo de chamadas vira sopa); é o motivo
   de ter partido pro Ghidra (rodando em background enquanto escrevo isto).

   **Limite adicional achado hoje:** mesmo se a gente achar e decidir o byte certo pra
   virar, patchear esse ELF extraído **não adianta nada sozinho** — o shadPS4 carrega o
   `eboot.bin` original (SELF), não esse dump. Qualquer patch precisa ir no arquivo real
   em `/mnt/jogos/ShadPS4 Games/CUSA24767/eboot.bin`, no offset correspondente dentro do
   `self_segment_header` daquele segmento (não é o mesmo offset do ELF reconstruído — a
   ferramenta `--dump-elf` monta o ELF do zero, os offsets de arquivo não coincidem, só os
   vaddr coincidem). **Resolvido**: `--dump-elf` agora também escreve um `.segmap` (índice,
   vaddr, tamanho, offset real no arquivo original) — verificado byte a byte que os 7 bytes
   no offset real batem com os 7 bytes no dump pro mesmo vaddr. Então já dá pra traduzir
   qualquer endereço achado no ELF extraído pro lugar certo no `eboot.bin` de verdade, sem
   suposição.

   **Atalho tentado em paralelo, via config em vez de patch binário:** o shadPS4 já tem um
   `dev_kit_mode` (`EmulatorSettings.IsDevKit()`, chave `dev_kit_mode` no `config.json`) que
   controla `sceKernelIsDevkit()` e o tamanho de memória reportado ao jogo (devkit real tem
   mais RAM liberada). Muitos jogos de PS4 checam isso em runtime pra decidir se carregam
   telas/menus de desenvolvedor — se o `IsDebugVersion()` do GT7 usar esse mesmo caminho,
   ligar isso destravaria sem tocar em nenhum byte do jogo. **Testado, ligado no
   `config.json` (`"dev_kit_mode": true`), jogo rodou** (log confirma
   `General isDevKit: true`) — carregou o menu de sign-in (`libSceSigninDialog`), passou da
   splash do GT. **Resultado do teste: não muda nada visível.** Com rede desligada, cai no
   mesmo aviso "sign in to PlayStation Network" de sempre; com shadNet ligado (mesma config
   que já funcionava antes), cai no mesmo `CE-210716` de sempre (servidor da Polyphony
   inalcançável, comportamento real confirmado). Fiquei preso num loop desse diálogo
   reaparecendo a cada OK — não cheguei a confirmar se eventualmente cai no Music Rally como
   sem o devkit, porque a memória apertou (GT7 + Ghidra rodando junto, disponível caiu pra
   2,2GB) e priorizei matar o processo do jogo pra proteger o sistema, já documentado como
   regra desta máquina. **Conclusão parcial:** `dev_kit_mode` sozinho não muda o
   comportamento até esse ponto — não decide nada sobre `IsDebugVersion()`/menu de debug por
   si só (ou decide, mas mais adiante, depois do ponto onde consegui testar). Deixei
   `dev_kit_mode: true` no `config.json` (reversível, não atrapalha nada visto até agora) —
   quem retestar deve tentar chegar até o Music Rally de novo com ele ligado e comparar.

   **Achado incidental útil:** `spectacle -b -n -o <arquivo.png>` tira screenshot da tela
   inteira **sem diálogo/confirmação**, mesmo em sessão Wayland — dá pra automatizar
   "screenshot + ler com Read tool" sem precisar que o usuário mande print manualmente.
   Cliques de mouse via `xdotool` não funcionam nessa UI (SDL/gamepad-only, confirmado de
   novo hoje); teclado via `xdotool key` depois de `xdotool search --name ... windowfocus`
   continua funcionando.

   **Próximo passo real: instalar Ghidra** (`pacman -S ghidra`, está nos repos oficiais,
   versão 12.1.2). Desmontagem crua por regex encontra xrefs pontuais mas não dá controle
   de fluxo, não decompila condicionais, e não escala pra entender o que `0xb49700` faz.
   Sem isso, continuar por aqui é apostar em sorte. Pedido `sudo -v` ao usuário pra
   instalar.

   **Update: Ghidra instalado e rodado headless, análise completa (38min, binário de
   80MB).** Comando reproduzível (projeto fica salvo, próxima vez é instantâneo — não
   precisa reanalisar):

       /opt/ghidra/support/analyzeHeadless <dir_projeto> GT7Project \
         -import gt7-eboot-plain.elf -processor x86:LE:64:default \
         -scriptPath <dir_com_script> -postScript DumpDebugXrefs.java \
         -log ghidra_import.log

   O script post-analysis (`DumpDebugXrefs.java`, no histórico do commit) decompila as
   funções de interesse e despeja xrefs num `.txt`. **Confirmou com decompilação de verdade
   (não só palpite por bytes) exatamente o que a análise manual já suspeitava:** `0xb49700`
   é uma função `GetOrCreate(param_1, param_2)` — guarda de "magic static" (`if
   (DAT_060674a8 == '\0') { ...__cxa_guard... }`), lock/incremento atômico de refcount,
   devolve um singleton compartilhado. **Não decide nada sozinha** — é só cadastro. E é
   usada em escala: a lista de quem chama essa função tem **dezenas de sites só nos
   primeiros 50 mostrados** (`0x2ce3f1b`, `0x2ce4475`, `0x2ce4497`, ... de 0x22 em 0x22
   bytes — uma tabela grande e repetitiva de registro de propriedades, bate com os ~600
   nomes de debug achados por `strings`). Ghidra **não conseguiu** achar os limites de
   função nos outros 3 endereços (`0x1d61550`, `0x10ec0d7`, `0x236e0ba` — os `lea` que
   carregam `IsDebugVersion`/`DebugFeatures`/`CourseDebugMode`): "No function contains this
   address" — a análise automática não criou um `Function` ali (o binário sem section
   headers/símbolos confunde o detector de limite de função em regiões grandes). Dá pra
   forçar manualmente no Ghidra interativo (não tentei — precisaria abrir a GUI, que é
   trabalho de sessão longa, não headless).

   **Onde isso deixa a investigação:** confirmado com alta confiança o que `0xb49700` é;
   ainda **não achamos onde o valor registrado é lido de volta** pra decidir se um menu de
   debug abre. Esse "read-back" site é outro lugar no binário, desconhecido, e não há atalho
   óbvio pra achá-lo sem navegar o grafo de chamadas manualmente na GUI do Ghidra (que
   funciona pro projeto já salvo em `ghidra_proj/GT7Project`, é só abrir
   `ghidra_proj/GT7Project/gt7-eboot-plain.elf` na GUI normal — não tentei, é interativo).
   Isso é o ponto onde a investigação por patch binário deixa de compensar em custo/retorno
   pra continuar sem ferramenta interativa de verdade: já foram ~2h desta sessão só nessa
   frente. Alternativa que ainda não foi tentada: servidor falso específico do protocolo da
   Polyphony (nunca começado, escopo grande, ver seção acima).

   **Update — forcei o Ghidra a criar função nos 3 endereços que ele tinha pulado**
   (`disassemble()` + `createFunction()` manual num script novo, `ForceDecompile.java`,
   reusando o projeto já salvo — não precisou reanalisar). Resultado **muda a conclusão de
   forma importante:**

   - `0x10ec0d7` (perto de `DebugFeatures`): **tem xref de entrada de verdade**, um `jne`
     condicional vindo de `0x10ebfdc` — é um branch real de código, não indireto. Mas o
     corpo decompilado é gigante (71.832 bytes só desse fragmento) e monta um structs cheio
     de campos nomeados: `StartType`, `Difficulty`, `RaceLimitLaps`, `Entrymax`,
     `WeatherID`, `CourseLabel`, `PlayerGrid`, `ProjectKey`, `PlayerCarClass`,
     `OnlineGameMode`, `NatAvailable`, `updatedEntries`, `messages`... e cita literalmente
     `GTParameter::GameParameterUtility::Options` e
     `GTParameter::GameParameterValidator::InitializeResult` no meio do código. **Isso é
     schema de parâmetro de sessão/corrida pra comunicação com o servidor** (matchmaking,
     config de race), não um menu de UI.
   - `0x236e0ba` (`CourseDebugMode`): corpo pequeno (596 bytes), mas também é claramente
     construção de struct — um monte de floats (`0x3f800000` = 1.0f, `0x43960000` = 300.0f,
     `0x42340000` = 45.0f, ...) ao lado do nome `CourseDebugMode`. Cara de parâmetro
     numérico de configuração de pista/câmera de debug, não de flag booleana de UI.

   **Conclusão revisada, com evidência de decompilação (não só suspeita):** as strings
   `IsDebugVersion`/`DebugFeatures`/`CourseDebugMode`/etc. batem com o padrão
   `N6PDISTD16PseudoReflection...` achado antes por `strings` — são **nomes de campo dentro
   do sistema de reflexão/serialização interno da Polyphony** (`PDISTD` = provavelmente
   "Polyphony Digital I/O Standard" ou similar), usado pra descrever parâmetros de
   sessão/corrida trocados com o backend, não flags de UI que abrem um menu de debug local.
   **A hipótese original — achar e virar um byte pra destravar um menu de debug — não se
   sustenta mais com o que foi encontrado.** Não dá pra descartar 100% sem achar o
   read-back de `IsDebugVersion()` especificamente (ela não apareceu como branch real nos
   dois testes acima, só as outras duas), mas o padrão das outras duas jogou a probabilidade
   pra baixo o suficiente pra não valer continuar cavando esse caminho sem um motivo novo.
   **Recomendação: pausar a via de patch binário aqui.** As opções que sobram são as já
   listadas (servidor falso da Polyphony, ou aceitar o Music Rally como único conteúdo
   offline) — nenhuma delas foi tentada ainda nesta sessão.

## Servidor falso — achei o hostname real, mas parei numa fronteira ética

Pedido do usuário: investigar viabilidade de um servidor falso pro protocolo próprio da
Polyphony (não o shadNet genérico), pra ver se destrava o CE-210716/Music-Rally-only.

**O shadPS4 já tem infraestrutura pronta pra isso — `host_overrides.json`.** Existe desde o
PR oficial "First ShadNet integration" (upstream, não fomos nós que criamos):
`~/.local/share/shadPS4/host_overrides.json` (ou `$SHADPS4_HTTP_HOST_OVERRIDES_JSON`),
formato `{"host:porta ou host ou scheme://host:porta ou *": "http(s)://novo_host:porta"}`,
lido por `src/core/libraries/network/http.cpp` (`ApplyHostOverride`, ~linha 350). **Só que
só vale pra requisições que passam pelo `sceHttp`** (a parte "PSN" via shadNet — friends,
blocks, restriction status, tudo pra `srv.shadps4.net:31315`). Não cobre o que viria a
seguir.

**Achamos o hostname real do backend do próprio GT7** filtrando o log ao vivo por
`Lib.Net`/`Lib.Http` (em vez de gravar o log inteiro — ver nota de rodapé sobre `/tmp`
abaixo). GT7 abre socket cru (`sys_socketex: name = SimpleTcpClient`, TCP, não HTTP) e
resolve via `sceNetResolverStartNtoa`:

    hostname = api.develop-stable.vegas.granturismo-online.net

Resolve de verdade em DNS público: `54.249.170.74` e `18.178.102.7`. Uma busca rápida achou
inclusive `admin.develop-stable.vegas.granturismo-online.net` e
`admin.preview3.vegas.granturismo-online.net` indexados — ou seja, **isso não é um hostname
interno/fictício, é infraestrutura de desenvolvimento/staging de verdade da Polyphony**,
publicamente resolvível (por engano ou não).

**Parei aqui de propósito.** `host_overrides.json` não cobre esse socket cru (é HTTP-only),
então cobrir essa conexão exigiria redirecionar via DNS (`/etc/hosts` local ou patch no
resolver do shadPS4) pra um servidor **nosso**, não interagir com o servidor real deles. Eu
não tentei conectar nos IPs acima nem abri o painel de admin — seria acessar infraestrutura
de terceiro sem autorização, fora do escopo de "reverse engineering do meu próprio jogo
rodando no meu emulador". O objetivo de um servidor falso é *substituir* essa conexão por
uma nossa, nunca *sondar* a deles.

**Onde isso deixa o projeto:** sabemos o hostname, sabemos que é um socket TCP cru chamado
"SimpleTcpClient" (não é HTTP nem HTTP/2), não sabemos o protocolo de fio (framing,
handshake, criptografia se houver). Sem uma captura de tráfego real de autêntico PS4↔servidor
(que não temos — não há PS4 físico nesta sessão) ou documentação pública do protocolo
"vegas"/"SimpleTcpClient", escrever um servidor falso que responda de forma que o GT7 aceite
é engenharia reversa de protocolo binário proprietário de uma empresa real, do zero, sem
referência — escopo de semanas, não desta sessão. Também acharam-se ~160 mil linhas de log
`sys_connect ... error code: 37 (EALREADY)` em segundos — o jogo fica re-chamando
`connect()` sem esperar a tentativa anterior terminar.

**Nota lateral sobre `/tmp`:** dois logs sem filtro (`gt7-devkit-test2.log` 3,6GB,
`gt7-httplog.log` 1,9GB) encheram o tmpfs de `/tmp` (7,7G) a 81%, quebrando a saída de
qualquer comando neste shell até o usuário rodar `rm` nos arquivos pedidos. Causa: exatamente
esse loop de `sys_connect`/EALREADY gerando log em alta taxa sem limite de tamanho. Pra
qualquer captura futura de log de rede do GT7, **usar `timeout` + filtro `grep --line-buffered`
ao vivo** (não gravar a saída bruta inteira), como feito na segunda tentativa.

### Investigação do loop EALREADY — NÃO é bug do shadPS4, e achou-se um fix mesmo assim

Confirmado com `ss -tnp` durante uma tentativa real: os dois sockets ficam em **`SYN-SENT`**
pra `18.178.102.7:443` e `54.249.170.74:443` — o SYN nunca recebe resposta (provavelmente um
firewall da AWS descartando silenciosamente pacote de IP não autorizado, sem RST). Isso
**não é bug do shadPS4** — é o estado correto de um `connect()` não-bloqueante numa rede que
não responde. O "loop" de 160 mil `EALREADY`/segundo é o próprio código do GT7 rechamando
`connect()` sem usar `epoll`/backoff enquanto o socket fica preso em limbo — comportamento
do jogo, não nosso, e não dá (nem devia) pra "consertar" isso no código deles.

**Mas isso sugeriu um fix de qualidade de vida real, sem tocar no servidor da Polyphony:**
se a conexão falhar **rápido e definitivamente** (RST/`ECONNREFUSED`) em vez de ficar em
`SYN-SENT` esperando o timeout da rede, o GT7 para de tentar imediatamente — porque o "loop"
só existe enquanto o estado fica indefinido. Implementado um mecanismo de override de
resolver **dentro do shadPS4** (não é `/etc/hosts`, não mexe no sistema, é só um arquivo de
config do usuário — reversível e não toca em nada de terceiro):

- Novo `~/.local/share/shadPS4/resolver_overrides.json` (ou
  `$SHADPS4_RESOLVER_OVERRIDES_JSON`), formato `{"hostname": "ip_ou_hostname_alvo"}`.
  Implementado em `src/core/libraries/network/net_util.cpp`
  (`LoadResolverOverrideState`/`LookupResolverOverride`, chamado no início de
  `ResolveHostname`), espelhando o `ApplyHostOverride` que já existia em `http.cpp` — só que
  esse cobre **qualquer** resolução de hostname via `getaddrinfo`, incluindo sockets crus
  como o `SimpleTcpClient` do GT7, que o `host_overrides.json` (HTTP-only) nunca alcançava.
- Configurado localmente: `"api.develop-stable.vegas.granturismo-online.net": "127.0.0.1"`
  — redireciona pro próprio localhost, onde nada escuta na porta 443, então o SO devolve
  `ECONNREFUSED` quase na hora (RST local, sem round-trip de rede nenhum).
- **Testado e confirmado**: antes, ~80 mil linhas de `EALREADY` por socket em menos de 90s
  gravadas no log e o processo preso naquele estado o tempo todo. Depois do override,
  **uma única linha** `sys_connect ... error code: 61 (ECONNREFUSED)` por socket, e o jogo
  segue em frente pro mesmo diálogo `CE-210716` de sempre — só que quase instantâneo em vez
  de travado por minutos. Nenhuma mudança de conteúdo/gameplay (o erro visível pro jogador é
  idêntico, é esperado e honesto — só o caminho até ele ficou saudável).
- Isso **não** é o servidor falso (não fizemos o GT7 aceitar nada, só fizemos a rejeição
  chegar rápido) — mas é uma peça de infraestrutura reutilizável: se algum dia alguém montar
  um servidor de verdade que fale o protocolo do `SimpleTcpClient`, é só trocar o `127.0.0.1`
  nesse mesmo arquivo pelo endereço do servidor real, sem precisar de root nem mexer no SO.

Commitado no fork (`net_util.cpp` + este texto).

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

`origin` = `https://github.com/shadps4-emu/shadPS4.git` (upstream público, só leitura).
`github` = `LeVonikke/GT7-PC-Recomp`, repo privado — é pra onde os commits vão. Compartilhado
também pelo lado Windows dessa máquina dual-boot (mesmo caminho, via `F:\...` — se aparecer
diff gigante de CRLF em `.cpp`/`.h` sem eu ter mexido, é isso, não é incidente).

Commits próprios até agora (2026-09-03): fix de crash de boot (memória liberada virando
guard page), fix de vazamento de epoll, fix de tiling PRT, fix de assert de interpolação,
ferramenta `--dump-elf`, override de resolver (`resolver_overrides.json`). Todos enviados
pro `github` remote.

**Busca por trabalho prévio da comunidade (2026-09-03):** nada publicado sobre
`SimpleTcpClient` ou o protocolo do backend `vegas`/`granturismo-online.net`. O que existe
de engenharia reversa pública do GT7 (Nenkai e outros) é sobre o protocolo **UDP de
telemetria local** (`192.168.x.x`, usado por simuladores/motion rigs) — feature completamente
diferente, já documentada e sem relação com a conexão remota que investigamos. Confirma:
não há atalho/referência pública pra reverter o protocolo do backend real; servidor falso
continua sendo trabalho do zero, de escopo grande, pausado por decisão já registrada acima.
