#include "engine.hpp"
#include "lip.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
namespace fs = std::filesystem;
namespace {
void require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
void near(double a, double b, const char* message) { require(std::abs(a-b)<1e-5, message); }
template<class F> void rejects(F function, const char* message) {
    bool failed=false; try { function(); } catch (const std::exception&) { failed=true; }
    require(failed,message);
}
std::string bytes(const fs::path& path) { std::ifstream in(path,std::ios::binary); return {std::istreambuf_iterator<char>(in),{}}; }
std::vector<unsigned> sequence(const cslu::LipFile& lip) {
    std::vector<unsigned> result;
    for(const auto& key:lip.frames) if(result.empty() || result.back()!=key.shape) result.push_back(key.shape);
    return result;
}
void strict(const cslu::LipFile& lip) {
    const auto check=cslu::validateLipForKotorRuntime(lip);
    require(check.ok, "Generator emitted an invalid curve");
    require(lip.frames.front().timeSeconds==0 && lip.frames.front().shape==0,"New file convention changed");
    require(lip.frames.back().timeSeconds==lip.durationSeconds,"New endpoint does not equal duration");
    for(std::size_t i=1;i<lip.frames.size();++i) require(lip.frames[i].timeSeconds>lip.frames[i-1].timeSeconds,"Generated equal/descending time");
}
void run(const fs::path& root) {
    // Reference structure, not copied assets: the nonzero final pose is legal.
    const cslu::LipFile source{0.343f, {{0,0},{0.201f,6},{0.343f,4}}};
    const auto original=root/"original.lip", saved=root/"saved.lip";
    cslu::writeLip(original,source);
    double duration=0; const auto keys=cslu::readKotorKeyframes(original,&duration);
    require(keys.size()==source.frames.size() && keys.front().timeSeconds==0,"Reader hid record zero");
    neolip::saveEditedLip(saved,keys,duration,false);
    require(bytes(saved)==bytes(original),"No-edit save changed header, pose or time bits");
    rejects([&]{neolip::saveEditedLip(saved,keys,duration,false);},"Save silently overwrote destination");
    auto nonzero=source; nonzero.frames.front().shape=7;
    cslu::writeLip(root/"nonzero-first.lip",nonzero);
    near(cslu::readLip(root/"nonzero-first.lip").frames.front().shape,7,"Nonzero first pose lost");
    // The runtime accepts a small relative end-time tolerance. Imported values
    // inside it are preserved; newly generated files still use exact equality.
    auto tolerated=source; tolerated.durationSeconds=std::nextafter(tolerated.durationSeconds,1.0f);
    cslu::writeLip(root/"tolerance.lip",tolerated);
    const auto toleratedKeys=cslu::readKotorKeyframes(root/"tolerance.lip",&duration);
    neolip::saveEditedLip(root/"tolerance-copy.lip",toleratedKeys,duration,false);
    require(bytes(root/"tolerance.lip")==bytes(root/"tolerance-copy.lip"),"Stored duration was inferred from last key");
    auto duplicate=source; duplicate.frames.insert(duplicate.frames.end()-1,{0.201f,10});
    cslu::writeLip(root/"duplicate.lip",duplicate);
    auto same=cslu::readKotorKeyframes(root/"duplicate.lip",&duration);
    neolip::saveEditedLip(root/"duplicate-copy.lip",same,duration,false);
    require(bytes(root/"duplicate.lip")==bytes(root/"duplicate-copy.lip"),"Equal-time source keys were normalized");
    require(!cslu::validateLipForKotorRuntime(duplicate).warnings.empty(),"Equal-time source was not flagged");
    const auto hit=cslu::sampleLipPose(duplicate,duplicate.frames[1].timeSeconds);
    require(hit.fromShape==10 && hit.weight==0,"Defensive duplicate-time preview is not deterministic");
    auto invalid=source; std::swap(invalid.frames[1],invalid.frames[2]);
    const auto before=bytes(saved);
    rejects([&]{cslu::writeLip(saved,invalid);},"Descending timestamps silently sorted");
    require(bytes(saved)==before,"Rejected save damaged existing file");
    rejects([&]{(void)cslu::sampleLipPose(invalid,0.1);},"Preview accepted descending times");
    for (const float bad : {-1.0f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}) {
        auto malformed=source; malformed.frames[1].timeSeconds=bad;
        rejects([&]{cslu::writeLip(saved,malformed);},"Invalid key time accepted");
        malformed=source; malformed.durationSeconds=bad;
        rejects([&]{cslu::writeLip(saved,malformed);},"Invalid duration accepted");
    }
    invalid=source; invalid.frames[1].shape=16;
    rejects([&]{cslu::writeLip(saved,invalid);},"Invalid shape accepted");
    invalid=source; invalid.frames.back().timeSeconds=0.25f;
    rejects([&]{cslu::writeLip(saved,invalid);},"Mismatched endpoint accepted");
    invalid=source; invalid.frames.erase(invalid.frames.begin());
    rejects([&]{cslu::writeLip(saved,invalid);},"Missing time-zero key accepted");

    std::vector<cslu::KotorKeyframe> broken{{0,0},{.28,8},{.2,3},{.2,4},{.343,12}};
    cslu::repairKotorKeyframes(broken,.343f);
    const auto repaired=cslu::lipFromKeyframes(broken,.343f);
    require(repaired.frames.back().shape==12 && repaired.durationSeconds==.343f,"Explicit repair changed ending pose/duration");
    require(repaired.frames.size()==4 && repaired.frames[1].shape==4,"Explicit collision policy changed");
    cslu::writeKotorKeyframes(root/"repaired.lip",broken,.343f);

    const cslu::LipFile hold{.339036f,{{0,0},{.125f,10},{.24263829f,10},{.26414588f,2},{.339036f,12}}};
    auto at=cslu::sampleLipPose(hold,.230);
    require(at.fromShape==10 && at.toShape==10,"Hold was mistaken for a transition");
    const auto between=(static_cast<double>(hold.frames[2].timeSeconds)+hold.frames[3].timeSeconds)/2;
    at=cslu::sampleLipPose(hold,between);
    require(at.fromShape==10 && at.toShape==2,"Blended numeric IDs instead of two sampled poses");
    near(at.weight,.5,"Interpolation weight does not match recovered game arithmetic");
    near(at.fromTalkPosition,11.0/16,"Pose sample offset is wrong");
    near(at.toTalkPosition,3.0/16,"Second pose sample offset is wrong");
    at=cslu::sampleLipPose(hold,20);
    require(at.completed && at.fromShape==12 && at.toShape==12,"End sample forced pose zero");
    near(cslu::sampleLipPose(hold,-1).weight,0,"Before-start cursor was not clamped");
    rejects([&]{(void)cslu::sampleLipPose(hold,std::numeric_limits<double>::quiet_NaN());},"NaN preview time accepted");
    auto reduced=hold; cslu::simplifyLipHolds(reduced);
    require(reduced.frames.size()==hold.frames.size(),"Removed a two-key hold");
    auto redundant=hold; redundant.frames.insert(redundant.frames.begin()+2,{.17f,10});
    cslu::simplifyLipHolds(redundant);
    require(redundant.frames.size()==hold.frames.size(),"Did not remove exact interior hold redundancy");
    for(int i=0;i<1000;++i) {
        const double t=hold.durationSeconds*i/999;
        const auto a=cslu::sampleLipPose(hold,t), b=cslu::sampleLipPose(redundant,t);
        // Compare per-pose weights, independent of key index or same-pose alpha.
        for(unsigned pose=0;pose<16;++pose) {
            const auto aw=(a.fromShape==pose?1-a.weight:0)+(a.toShape==pose?a.weight:0);
            const auto bw=(b.fromShape==pose?1-b.weight:0)+(b.toShape==pose?b.weight:0);
            near(aw,bw,"Curve simplification changed sampled animation");
        }
    }

    cslu::PhnFile phn; phn.intervals={{0,100,".pau"},{100,120,"A"},{120,180,"b"},{180,250,"E"}};
    auto natural=cslu::phnToLip(phn,{},neolip::qualityFor(neolip::MouthMovement::Natural));
    auto minimal=cslu::phnToLip(phn,{},neolip::qualityFor(neolip::MouthMovement::MinimalSmoothing));
    strict(natural); strict(minimal);
    require(sequence(natural)==std::vector<unsigned>({0,3,11,1}),"Natural dropped/reordered short vowel or forced final rest");
    require(sequence(minimal)==sequence(natural),"Minimal and Natural have different articulation order");
    near(natural.durationSeconds,.25,"No-silence clip acquired artificial tail");
    require(natural.frames.back().shape==1,"Final spoken pose replaced");
    auto findTime=[](const cslu::LipFile& lip,unsigned pose){ for(auto k:lip.frames)if(k.shape==pose)return double(k.timeSeconds);return -1.0; };
    near(findTime(natural,11),.120,"Unexpected fixed anticipation on normal creation");
    auto anticipate=neolip::qualityFor(neolip::MouthMovement::Natural); anticipate.anticipateClosuresMs=40;
    auto bounded=cslu::phnToLip(phn,{},anticipate); strict(bounded);
    require(sequence(bounded)==sequence(natural),"Opt-in anticipation reordered phonemes");
    require(findTime(bounded,11)>findTime(bounded,3),"Closure overtook its predecessor");
    auto onset=phn; onset.intervals.erase(onset.intervals.begin()); onset.intervals.front().startMs=0;
    strict(cslu::phnToLip(onset));
    phn.intervals.push_back({250,270,".pau"});
    auto tail=cslu::phnToLip(phn); strict(tail);
    require(tail.frames.back().shape==0 && tail.durationSeconds==.270f,"Brief trailing silence has no neutral hold");
    auto extended=cslu::phnToLip(phn,{false,100}); strict(extended);
    require(extended.durationSeconds==.370f,"Explicit tail authoring request ignored");
    auto empty=cslu::phnToLip({}); strict(empty); require(empty.frames.size()==2,"Empty synthetic source generated junk keys");
    cslu::PhnFile silent; silent.intervals={{0,350,".pau"}};
    require(cslu::phnToLip(silent).frames.size()==2,"Silence should be one hold");
    cslu::PhnFile bad; bad.intervals={{100,120,"A"},{110,130,"b"}};
    rejects([&]{(void)cslu::phnToLip(bad);},"Overlapping source intervals accepted");

    std::mt19937 random(413);
    const std::vector<std::string> phones={"A","b","i:","E","T","g","m","f","u",".pau"};
    for(int trial=0;trial<500;++trial) {
        cslu::PhnFile sample; int t=0;
        for(int j=0;j<30;++j) { const int end=t+1+static_cast<int>(random()%400); sample.intervals.push_back({t,end,phones[random()%phones.size()]}); t=end; }
        auto a=cslu::phnToLip(sample,{},neolip::qualityFor(neolip::MouthMovement::Natural));
        auto b=cslu::phnToLip(sample,{},neolip::qualityFor(neolip::MouthMovement::MinimalSmoothing));
        strict(a); strict(b);
        require(sequence(a)==sequence(b) && a.durationSeconds==b.durationSeconds,"Preset altered source ordering or duration");
        for(int j=0;j<20;++j) {
            const auto pose=cslu::sampleLipPose(a,a.durationSeconds*j/19);
            require(pose.weight>=0 && pose.weight<=1 && std::isfinite(pose.weight),"Invalid generated blend");
        }
    }
    // Confirm no work/trace directories survive successful or rejected writes.
    for (const auto& item:fs::directory_iterator(root)) require(item.path().extension()==".lip","Sidecar or temporary directory left behind");
}
}
int main() {
    const auto root=fs::temp_directory_path()/("NeoLIP curve tests "+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try { fs::create_directories(root); run(root); fs::remove_all(root);
        std::cout<<"PASS: lossless saves, validation, explicit repair, pose interpolation, hold preservation, both generators and 500 randomized alignments\n"; return 0;
    } catch(const std::exception& e) { std::error_code ec; fs::remove_all(root,ec); std::cerr<<"FAIL: "<<e.what()<<'\n'; return 1; }
}
