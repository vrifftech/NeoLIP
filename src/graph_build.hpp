#pragma once

#include "backend.hpp"
#include "text_ntp.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace cslu {

struct SpecEntry {
    std::string state;
    int index = -1;
    int dur1 = 0;
    int dur2 = 0;
    std::string tie;
};

struct FeatureMapEntry {
    std::vector<std::string> feature;
    std::vector<std::string> leftContext;
    std::vector<std::string> rightContext;
};

struct GeneratedMappings {
    IndexLists featStatesM;
    IndexLists featStatesP;
    IndexLists featStatesH;
    std::vector<std::string> featPhnM;
    std::vector<std::string> featPhnP;
    std::vector<std::string> featPhnH;
    std::vector<int> depList;
    std::vector<int> vcList;
    IndexLists transStatesM;
    IndexLists transStatesP;
    IndexLists transStatesH;
};

struct GeneratedGraphBundle {
    std::string ntpOriginal;
    std::string ntpAfterSubstitutions;
    std::vector<std::string> phones;
    TraceGraph graph;
    GeneratedMappings mappings;
};

std::vector<std::string> tokenizeTclListLike(const std::string& line);
std::string applyFaNtpSubstitutions(const std::string& ntpText);
std::vector<std::string> parseSimpleNtpPhoneSequence(const std::string& ntpAfterSubstitutions);
std::vector<std::string> parseNtpFirstPathPhoneSequence(const std::string& ntpAfterSubstitutions);

std::map<std::string, int> readPartsFile(const std::filesystem::path& path);
std::vector<SpecEntry> readSpecEntries(const std::filesystem::path& path);
std::map<std::string, FeatureMapEntry> readFeatureMap(const std::filesystem::path& path);

GeneratedGraphBundle buildGraphBundleFromNtpText(const std::string& ntpText,
                                                  const std::filesystem::path& faRoot);
// Same automatic pronunciation graph, with non-scoring word identity metadata.
GeneratedGraphBundle buildGraphBundleFromWords(const TextToNtpResult& text,
                                               const std::filesystem::path& faRoot);

GeneratedGraphBundle buildGraphBundleFromNtpFile(const std::filesystem::path& ntpFile,
                                                  const std::filesystem::path& faRoot);

BackendTraceData applyGeneratedMappingsToTraceNnets(BackendTraceData data,
                                                    const GeneratedMappings& mappings);

void writeGraphBundle(const GeneratedGraphBundle& bundle,
                      const std::filesystem::path& outDir,
                      const std::filesystem::path& faRoot = {});

std::vector<std::string> phnProbLabels(const GeneratedGraphBundle& bundle);
std::vector<std::string> transProbLabels(const GeneratedGraphBundle& bundle);

} // namespace cslu
