#pragma once
// Synthetic engineering fixtures only; no game dialogue assets are bundled.
#include <GFFFile.hpp>
#include <neotlk/TlkFile.hpp>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
namespace dialogue_fixture {
struct Node {
    std::string vo, sound, text;
    std::uint32_t strref = 0xffffffffu;
    bool reply = false;
    int substring = 0;
};
inline void write(const std::filesystem::path& path, const std::vector<Node>& nodes) {
    neogff::GffFile file; file.NewFile("DLG ");
    auto entries = std::make_unique<neogff::GffList>("EntryList");
    auto replies = std::make_unique<neogff::GffList>("ReplyList");
    for (const auto& node : nodes) {
        auto item = std::make_unique<neogff::GffStruct>();
        item->AddField(std::make_unique<neogff::GffResRefField>("VO_ResRef", node.vo));
        item->AddField(std::make_unique<neogff::GffResRefField>("Sound", node.sound));
        item->AddField(std::make_unique<neogff::GffByteField>("SoundExists", 1));
        item->AddField(std::make_unique<neogff::GffExoStringField>("Speaker", node.reply ? "Player" : "Speaker"));
        auto text = std::make_unique<neogff::GffLocalizedStringField>("Text", node.strref);
        if (!node.text.empty()) text->AddString(node.substring, node.text);
        item->AddField(std::move(text));
        // Deliberately cyclic logical links; bulk import must not walk them.
        auto links = std::make_unique<neogff::GffList>(node.reply ? "EntriesList" : "RepliesList");
        auto link = std::make_unique<neogff::GffStruct>();
        link->AddField(std::make_unique<neogff::GffUInt32Field>("Index", 0)); links->AddStruct(std::move(link));
        item->AddField(std::move(links));
        if (node.reply) replies->AddStruct(std::move(item)); else entries->AddStruct(std::move(item));
    }
    file.root()->AddField(std::move(entries)); file.root()->AddField(std::move(replies));
    auto starters = std::make_unique<neogff::GffList>("StartingList");
    file.root()->AddField(std::move(starters));
    std::filesystem::create_directories(path.parent_path()); file.SaveFile(path);
}
inline void tlk(const std::filesystem::path& path, const std::vector<std::string>& texts, unsigned language = 0) {
    neotlk::TalkTable table; table.newFile(); table.setLanguage(language);
    for (const auto& text : texts) {
        neotlk::TalkString entry; entry.flags = neotlk::TEXT_PRESENT; entry.text = text;
        table.addEntry(std::move(entry));
    }
    std::filesystem::create_directories(path.parent_path()); table.save(path.string());
}
} // namespace dialogue_fixture
