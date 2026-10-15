// Optional CLI for automation. The desktop product does not install this file's
// executable; both frontends call the same typed engine rather than one another.
#include "engine.hpp"
#include "dialogue_batch.hpp"
#include <neoshared/PathUtf8.hpp>
#include <atomic>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#ifndef CSLU_APP_VERSION
#define CSLU_APP_VERSION "1.0.0"
#endif
namespace fs = std::filesystem;
namespace {
volatile std::sig_atomic_t interrupted = 0;
void interrupt(int) { interrupted = 1; }
void print(const neolip::Result& result) {
    std::cout << result.message << '\n';
    if (result.status == neolip::Status::Created)
        std::cout << "Duration: " << result.durationSeconds << " seconds\nMouth keys: " << result.mouthKeys << '\n';
    for (const auto& warning : result.warnings) std::cerr << "Review " << warning.startSeconds << "-" << warning.endSeconds << "s: " << warning.message << '\n';
    for (const auto& word : result.unknownWords) std::cerr << "Unknown word: " << word << '\n';
}
}
static int commandMain(int argc, char** argv) {
    try {
        const auto usage = [](std::ostream& out) {
            out << "NeoLIP " CSLU_APP_VERSION "\n"
                "Usage: neolip-cli <command> [options]\n"
                "create <wav> <transcript.txt> <output.lip> --models <folder> --dictionaries <folder> [--minimal] [--replace] [--channel N]\n"
                "batch <input folder> <output folder> --models <folder> --dictionaries <folder> [--minimal] [--replace] [--recursive]\n"
                "dialogue <file.dlg> <game directory or search folder> <output folder> [--tlk dialog.tlk] [--scan-only] --models <folder> --dictionaries <folder> [--minimal] [--replace]\n"
                "lookup <word> --models <folder> --dictionaries <folder> [--user-dictionary <file>]\n"
                "--help, -h   Show usage without loading assets.\n"
                "--version    Show the application version.\n";
        };
        if (argc < 2) { usage(std::cerr); return 2; }
        std::string command = argv[1];
        if (command == "--help" || command == "-h") { usage(std::cout); return 0; }
        if (command == "--version") { std::cout << "NeoLIP " CSLU_APP_VERSION "\n"; return 0; }
        neolip::Assets assets;
        neolip::MouthMovement movement = neolip::MouthMovement::Natural;
        bool replace = false, recursive = false, scanOnly = false;
        fs::path talkTable;
        int channel = -1;
        std::vector<std::string> positional;
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            const auto value = [&]() { if (++i >= argc) throw std::runtime_error(arg + " requires a value"); return neoshared::pathFromUtf8(argv[i]); };
            if (arg == "--models") assets.models = value();
            else if (arg == "--dictionaries") assets.dictionaries = value();
            else if (arg == "--user-dictionary") assets.userDictionary = value();
            else if (arg == "--tlk") talkTable = value();
            else if (arg == "--scan-only") scanOnly = true;
            else if (arg == "--channel") {
                if (++i >= argc) throw std::runtime_error("--channel requires a one-based channel number");
                std::size_t used = 0; channel = std::stoi(argv[i], &used) - 1;
                if (used != std::string(argv[i]).size() || channel < 0 || channel >= 32)
                    throw std::runtime_error("--channel must be from 1 to 32");
            }
            else if (arg == "--minimal") movement = neolip::MouthMovement::MinimalSmoothing;
            else if (arg == "--replace") replace = true;
            else if (arg == "--recursive") recursive = true;
            else if (arg.rfind("--", 0) == 0) throw std::runtime_error("Unknown option: " + arg);
            else positional.push_back(arg);
        }
        if (command != "dialogue" && (scanOnly || !talkTable.empty()))
            throw std::runtime_error("--tlk and --scan-only apply only to dialogue batches");
        std::signal(SIGINT, interrupt); std::signal(SIGTERM, interrupt);
        std::atomic<bool> cancel{false};
        neolip::Context context;
        context.work.cancelled = &cancel;
        context.work.yield = [&]() { if (interrupted) cancel.store(true); };
        if (command == "dialogue" && positional.size() == 3) {
            if (channel != -1 || recursive) throw std::runtime_error("Dialogue search is recursive; resolve per-line channel choices in the GUI.");
            const auto plan = neolip::planDialogue({neoshared::pathFromUtf8(positional[0]), neoshared::pathFromUtf8(positional[1]), talkTable, neoshared::pathFromUtf8(positional[2])}, context);
            std::cout << "Search folder: " << neoshared::pathToUtf8(plan.request.audioRoot) << '\n'
                      << "Recordings indexed: " << plan.recordingsIndexed << '\n';
            if (!plan.request.talkTable.empty()) std::cout << "Talk table: " << neoshared::pathToUtf8(plan.request.talkTable) << '\n';
            std::size_t unresolved = 0;
            for (const auto& row : plan.rows) {
                std::cout << row.line.label() << ": " << neolip::dialogueStateName(row.state) << " - " << row.detail << '\n';
                if (!row.audio.empty()) std::cout << "  Recording: " << neoshared::pathToUtf8(row.audio.lexically_relative(plan.request.audioRoot)) << '\n';
                if (row.state != neolip::DialogueState::Ready && row.state != neolip::DialogueState::Duplicate && row.state != neolip::DialogueState::NoVoice && row.state != neolip::DialogueState::Excluded) ++unresolved;
            }
            if (scanOnly) return unresolved ? 1 : 0;
            if (plan.batch.pairs.empty()) throw std::runtime_error("No dialogue lines have both recorded audio and resolved text.");
            neolip::Engine engine(assets);
            const auto result = engine.batch(plan.batch, movement, replace, context, print);
            std::cout << result.created << " created, " << result.skipped << " kept, " << result.failed << " failed; " << unresolved << " unresolved dialogue nodes.\n";
            return result.failed || result.cancelled || unresolved ? 1 : 0;
        }
        if (command == "create" && positional.size() == 3) {
            neolip::Engine engine(assets);
            auto result = engine.create({neoshared::pathFromUtf8(positional[0]), neolip::readTranscript(neoshared::pathFromUtf8(positional[1])),
                neoshared::pathFromUtf8(positional[2]), movement, replace, channel}, context);
            print(result); return result.status == neolip::Status::Created ? 0 : 1;
        }
        if (command == "batch" && positional.size() == 2) {
            if (channel != -1) throw std::runtime_error("Use --channel with single-file creation; batch flags unusual channel layouts for individual review.");
            const auto plan = neolip::planBatch(neoshared::pathFromUtf8(positional[0]), neoshared::pathFromUtf8(positional[1]), recursive, context);
            if (plan.pairs.empty()) throw std::runtime_error("No matching audio/transcript pairs.");
            for (const auto& problem : plan.problems) std::cerr << "Excluded: " << problem << '\n';
            neolip::Engine engine(assets);
            const auto result = engine.batch(plan, movement, replace, context, print);
            std::cout << result.created << " created, " << result.skipped << " skipped, " << result.failed << " failed; "
                << plan.missingTranscripts.size() << " audio files without transcripts.\n";
            return result.failed || result.cancelled || !plan.problems.empty() ? 1 : 0;
        }
        if (command == "lookup" && positional.size() == 1) {
            neolip::Engine engine(assets);
            const auto result = engine.pronounce(positional[0], context);
            for (const auto& word : result.wordInfo) std::cout << word.word << ": " << word.pronunciation << '\n';
            return result.unknownWords.empty() ? 0 : 1;
        }
        throw std::runtime_error("Unknown command or wrong number of arguments.");
    } catch (const std::exception& e) { std::cerr << "Error: " << e.what() << '\n'; return 1; }
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    try {
        std::vector<std::string> utf8;
        utf8.reserve(static_cast<std::size_t>(argc));
        for (int i = 0; i < argc; ++i) {
            const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, nullptr, 0, nullptr, nullptr);
            if (count <= 0) throw std::runtime_error("Invalid Unicode command-line argument");
            std::string value(static_cast<std::size_t>(count), '\0');
            if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, value.data(), count, nullptr, nullptr))
                throw std::runtime_error("Cannot decode command-line argument");
            value.pop_back(); utf8.push_back(std::move(value));
        }
        std::vector<char*> arguments;
        for (auto& value : utf8) arguments.push_back(value.data());
        return commandMain(argc, arguments.data());
    } catch (const std::exception& e) { std::cerr << "Error: " << e.what() << '\n'; return 1; }
}
#else
int main(int argc, char** argv) { return commandMain(argc, argv); }
#endif
