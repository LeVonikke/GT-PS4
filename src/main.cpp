// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <vector>
#include <CLI/CLI.hpp>
#include <SDL3/SDL_messagebox.h>

#include "common/arch.h"
#include "common/key_manager.h"
#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "common/path_util.h"
#include "core/debugger.h"
#include "core/emulator_settings.h"
#include "core/emulator_state.h"
#include "core/file_sys/fs.h"
#include "core/ipc/ipc.h"
#include "core/loader/elf.h"
#include "core/user_settings.h"
#include "emulator.h"
#include "imgui/big_picture/big_picture.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#endif

// Extracts the plaintext ELF wrapped inside a (S)ELF eboot, for offline disassembly. Only
// handles segments that are neither encrypted nor compressed (Core::Loader::Elf::LoadSegment
// itself doesn't decrypt/decompress either - on this dump the segments are already plaintext,
// "fself"-style, which is why the emulator can load and run it directly with no extra keys).
static int DumpElf(const std::filesystem::path& ebootPath, const std::filesystem::path& outPath) {
    Core::Loader::Elf elf;
    elf.Open(ebootPath);
    if (!elf.IsSelfFile() && !elf.IsElfFile()) {
        std::cerr << "Not a recognizable (S)ELF/ELF file: " << ebootPath << "\n";
        return 1;
    }

    const auto elf_header = elf.GetElfHeader();
    const auto phdrs = elf.GetProgramHeader();
    const auto self_segments = elf.GetSegmentHeader();

    std::ifstream src(ebootPath, std::ios::binary);
    if (!src) {
        std::cerr << "Failed to open " << ebootPath << " for reading\n";
        return 1;
    }

    // Compute a fresh flat layout: ELF header, then program headers, then each PT_LOAD
    // segment's bytes back to back, page-aligned. Non-PT_LOAD segments keep their original
    // p_offset/p_filesz zeroed out (nothing to copy for them here).
    std::vector<elf_program_header> out_phdrs(phdrs.begin(), phdrs.end());
    u64 cursor = sizeof(elf_header) + phdrs.size() * sizeof(elf_program_header);
    std::vector<std::pair<u64, std::vector<u8>>> segment_data; // {new offset, bytes}
    // {segment index, vaddr, memsz, real offset in the ORIGINAL ebootPath file} - lets a
    // later patch step map an address found in the extracted ELF back to where those same
    // bytes actually live in eboot.bin, since the extraction rewrites p_offset.
    std::vector<std::tuple<size_t, u64, u64, u64>> segment_map;

    for (size_t i = 0; i < phdrs.size(); i++) {
        auto& phdr = out_phdrs[i];
        if (phdr.p_type != PT_LOAD || phdr.p_filesz == 0) {
            phdr.p_offset = 0;
            continue;
        }

        // Find the self_segment_header covering this program header (mirrors
        // Core::Loader::Elf::LoadSegment's own lookup, minus the memory write).
        const self_segment_header* seg = nullptr;
        for (const auto& s : self_segments) {
            if (s.IsBlocked() && s.GetId() == i) {
                seg = &s;
                break;
            }
        }
        u64 read_offset = phdr.p_offset;
        if (seg) {
            if (seg->IsEncrypted() || seg->IsCompressed()) {
                std::cerr << "Segment " << i
                          << " is encrypted/compressed - can't extract plaintext, aborting.\n";
                return 1;
            }
            read_offset = seg->file_offset;
        }

        std::vector<u8> buf(phdr.p_filesz);
        src.seekg(static_cast<std::streamoff>(read_offset));
        src.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        if (!src) {
            std::cerr << "Short read on segment " << i << " (offset " << read_offset << ", size "
                      << buf.size() << ")\n";
            return 1;
        }

        segment_map.emplace_back(i, phdr.p_vaddr, phdr.p_filesz, read_offset);
        phdr.p_offset = cursor;
        segment_data.emplace_back(cursor, std::move(buf));
        cursor += phdr.p_filesz;
        cursor = (cursor + 0xF) & ~u64(0xF);
    }

    std::ofstream out(outPath, std::ios::binary);
    if (!out) {
        std::cerr << "Failed to open " << outPath << " for writing\n";
        return 1;
    }
    out.write(reinterpret_cast<const char*>(&elf_header), sizeof(elf_header));
    out.write(reinterpret_cast<const char*>(out_phdrs.data()),
              static_cast<std::streamsize>(out_phdrs.size() * sizeof(elf_program_header)));
    for (const auto& [offset, bytes] : segment_data) {
        out.seekp(static_cast<std::streamoff>(offset));
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    std::cout << "Wrote " << outPath << " (" << out_phdrs.size() << " program headers, "
              << segment_data.size() << " PT_LOAD segments extracted)\n";

    // Sidecar: "<index> <vaddr_hex> <memsz_hex> <real_offset_in_ebootPath_hex>" per PT_LOAD
    // segment, one per line, so a patch found at a vaddr in the extracted ELF can be
    // translated back to a byte offset in the real eboot.bin (this dump's own p_offset
    // values are made-up and do NOT correspond to anything in the original file).
    const auto mapPath = outPath.string() + ".segmap";
    std::ofstream mapOut(mapPath);
    if (mapOut) {
        mapOut << "# index vaddr memsz real_offset_in(" << ebootPath.string() << ")\n";
        for (const auto& [idx, vaddr, memsz, real_off] : segment_map) {
            mapOut << idx << " 0x" << std::hex << vaddr << " 0x" << memsz << " 0x" << real_off
                   << std::dec << "\n";
        }
        std::cout << "Wrote " << mapPath << " (vaddr -> real eboot.bin offset map)\n";
    }
    return 0;
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

#if defined(__APPLE__) && defined(ARCH_X86_64)
    // KosmicKrisp only supports Apple Silicon. Check that we are not running on an Intel Mac.
    int sysctl_ret = 0;
    size_t sysctl_size = sizeof(sysctl_ret);
    sysctlbyname("sysctl.proc_translated", &sysctl_ret, &sysctl_size, nullptr, 0);
    if (sysctl_ret != 1) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "shadPS4",
                                 "shadPS4 only supports Apple Silicon Macs.", nullptr);
        std::cout << "shadPS4 only supports Apple Silicon Macs." << std::endl;
        return -1;
    }
#endif

    CLI::App app{"shadPS4 Emulator CLI"};

    // ---- CLI state ----
    std::optional<std::string> gamePath;
    std::vector<std::string> gameArgs;
    std::optional<std::filesystem::path> overrideRoot;
    std::optional<int> waitPid;
    bool waitForDebugger = false;

    std::optional<std::string> fullscreenStr;
    bool ignoreGamePatch = false;
    bool showFps = false;
    bool configClean = false;
    bool configGlobal = false;
    bool bigPicture = false;
    bool sameProcess = false;

    std::optional<std::filesystem::path> addGameFolder;
    std::optional<std::filesystem::path> setAddonFolder;
    std::optional<std::string> patchFile;
    std::optional<std::filesystem::path> dumpElfPath;

    std::vector<std::pair<std::filesystem::path, std::string>> mounts;
    static std::vector<std::string> env_vars;

    // ---- Options ----
    app.add_option("guest_arg", gamePath, "Game path or ID"); // positional
    app.add_option("-g,--game", gamePath, "Game path or ID");
    app.add_option("-p,--patch", patchFile, "Patch file to apply");
    app.add_flag("-i,--ignore-game-patch", ignoreGamePatch,
                 "Disable automatic loading of game patches");

    app.add_flag("-b,--big-picture", bigPicture, "Start in Big Picture Mode");
    app.add_flag("--same-process", sameProcess,
                 "Launch the game in the same process when using Big Picture Mode");

    app.add_option("-f,--fullscreen", fullscreenStr, "Fullscreen mode (true|false)");

    app.add_option("--override-root", overrideRoot)->check(CLI::ExistingDirectory);

    app.add_flag("--wait-for-debugger", waitForDebugger);
    app.add_option("--wait-for-pid", waitPid);

    app.add_flag("--show-fps", showFps);
    app.add_flag("--config-clean", configClean);
    app.add_flag("--config-global", configGlobal);
    app.add_flag("--log-append", Common::Log::g_should_append);

    app.add_option("--add-game-folder", addGameFolder)->check(CLI::ExistingDirectory);
    app.add_option("--set-addon-folder", setAddonFolder)->check(CLI::ExistingDirectory);
    app.add_option("--mount", mounts, "Mount source to destination");
    app.add_option("-e,--env", env_vars, "Environment variables to pass to the guest");
    app.add_option("--dump-elf", dumpElfPath,
                   "Extract the plaintext ELF from the game's (S)ELF eboot instead of running "
                   "it - for offline reverse-engineering (disassembly/symbol lookup), no "
                   "in-emulator effect. Fails loudly if any PT_LOAD segment is still encrypted "
                   "or compressed - this only works on already-decrypted 'fself' style dumps.");

    // ---- Capture args after `--` verbatim ----
    app.allow_extras();

    // ---- No-args behavior ----
    if (argc == 1) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "shadPS4",
                                 "This is a CLI application. Please use the '-b' flag for Big "
                                 "Picture mode, or QTLauncher for a standalone GUI:\n"
                                 "https://github.com/shadps4-emu/shadps4-qtlauncher/releases",
                                 nullptr);
        std::cout << app.help();
        return -1;
    }

    try {
        bool double_dash_found = false;
        int double_dash_index;
        for (int i = 0; i < argc; i++) {
            if (double_dash_found) {
                gameArgs.emplace_back(argv[i]);
            }
            if (!double_dash_found && std::string(argv[i]) == "--") {
                double_dash_found = true;
                double_dash_index = i;
            }
        }

        // If the -- arg is present, only parse args before it
        if (double_dash_found) {
            app.parse(double_dash_index, argv);
        } else {
            app.parse(argc, argv);
        }
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }

    if (waitPid)
        Core::Debugger::WaitForPid(*waitPid);

    // Initialize main log with default config
    Common::Log::Setup("shadps4.log");

    LOG_INFO(Debug, "Run: {}", std::span(argv, argc));

    IPC::Instance().Init();

    auto emu_state = std::make_shared<EmulatorState>();
    EmulatorState::SetInstance(emu_state);
    UserSettings.Load();

    // Initialize key manager
    auto key_manager = KeyManager::GetInstance();
    key_manager->LoadFromFile();

    // Load configurations
    std::shared_ptr<EmulatorSettingsImpl> emu_settings = std::make_shared<EmulatorSettingsImpl>();
    EmulatorSettingsImpl::SetInstance(emu_settings);
    emu_settings->Load();

    // Configure logger appropriately
    Common::Log::g_should_append |= EmulatorSettings.IsLogAppend();

    if (bigPicture) {
        BigPictureMode::Launch(argv[0], sameProcess);
        return 0;
    }

    // ---- Utility commands ----
    if (addGameFolder) {
        EmulatorSettings.AddGameInstallDir(*addGameFolder);
        EmulatorSettings.Save();
        LOG_INFO(Config, "Game folder successfully saved.");
        return 0;
    }

    if (setAddonFolder) {
        EmulatorSettings.SetAddonInstallDir(*setAddonFolder);
        EmulatorSettings.Save();
        LOG_INFO(Config, "Addon folder successfully saved.");
        return 0;
    }

    if (!gamePath.has_value()) {
        if (!gameArgs.empty()) {
            gamePath = gameArgs.front();
            gameArgs.erase(gameArgs.begin());
        } else {
            LOG_ERROR(Debug, "Please provide a game path or ID.");
            return 1;
        }
    }

    // ---- Apply flags ----
    if (patchFile)
        MemoryPatcher::patch_file = *patchFile;

    if (ignoreGamePatch)
        Core::FileSys::MntPoints::ignore_game_patches = true;

    if (fullscreenStr) {
        if (*fullscreenStr == "true") {
            EmulatorSettings.SetFullScreen(true);
        } else if (*fullscreenStr == "false") {
            EmulatorSettings.SetFullScreen(false);
        } else {
            LOG_ERROR(Debug, "Invalid argument for --fullscreen (use true|false)");
            return 1;
        }
    }

    if (showFps)
        EmulatorSettings.SetShowFpsCounter(true);

    if (configClean)
        EmulatorSettings.SetConfigMode(ConfigMode::Clean);

    if (configGlobal)
        EmulatorSettings.SetConfigMode(ConfigMode::Global);

    // ---- Resolve game path or ID ----
    std::filesystem::path ebootPath(*gamePath);
    const auto archive_component_exists = [](const std::filesystem::path& p) -> bool {
        std::filesystem::path accum;
        for (const auto& comp : p) {
            accum /= comp;
            if (comp.extension() == ".zar") {
                return std::filesystem::is_regular_file(accum);
            }
        }
        return false;
    };
    if (!std::filesystem::exists(ebootPath) && !archive_component_exists(ebootPath)) {
        bool found = false;
        constexpr int maxDepth = 5;
        for (const auto& installDir : EmulatorSettings.GetGameInstallDirs()) {
            if (auto foundPath = Common::FS::FindGameByID(installDir, *gamePath, maxDepth)) {
                ebootPath = *foundPath;
                found = true;
                break;
            }
        }
        if (!found) {
            LOG_ERROR(Debug, "Game ID or file path not found: {}", *gamePath);
            return 1;
        }
    }

    if (dumpElfPath.has_value()) {
        return DumpElf(ebootPath, *dumpElfPath);
    }

    auto* emulator = Common::Singleton<Core::Emulator>::Instance();
    emulator->executableName = argv[0];
    emulator->waitForDebuggerBeforeRun = waitForDebugger;
    emulator->Run(ebootPath, gameArgs, overrideRoot, mounts, env_vars);

    return 0;
}
