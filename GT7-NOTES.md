# GT7 PC compat — notas do projeto

Leia isto antes de mexer no repo em qualquer máquina nova. Espelha o padrão do
`CONTEXTO7920.md` da 7920 (que este projeto não faz parte — repo separado por design,
para poder ser clonado em qualquer máquina, Windows incluído).

## O que é isto

Fork privado do [shadPS4](https://github.com/shadps4-emu/shadPS4) (`origin` continua
apontando pro upstream público) para trabalhar em compatibilidade do **Gran Turismo 7**
(CUSA24769), jogo próprio, cópia legal. Estado em 2026-08-30: **nenhuma modificação de
código ainda** — só infra de build validada. O trabalho de fato (implementar
`libSceAudio3d`, etc.) não começou.

## Bloqueio conhecido do GT7

O jogo boota até o logo e crasha. Causa identificada publicamente (issues
[#528](https://github.com/shadps4-compatibility/shadps4-game-compatibility/issues/528),
[#2290](https://github.com/shadps4-compatibility/shadps4-game-compatibility/issues/2290),
[#3004](https://github.com/shadps4-emu/shadPS4/issues/3004)): **`libSceAudio3d` não está
implementada**. Esse é o próximo alvo concreto — achar onde o GT7 chama essa lib e
implementar stub/real.

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
