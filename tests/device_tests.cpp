#include "playback.hpp"
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>

// This target compiles the real AudioPlayback implementation with a test-only
// null output, not a mocked miniaudio header and not a product runtime option.
namespace {
void require(bool condition,const char* why){if(!condition)throw std::runtime_error(why);}
void advance(neolip::AudioPlayback& player) {
    const double start=player.position();const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(std::chrono::steady_clock::now()<until&&player.position()<=start+.03)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    require(player.position()>start+.03,"Device callback did not advance PCM");
}
}
int main() {
    try {
        auto audio=std::make_shared<neolip::AudioBuffer>();audio->waveform.sampleRate=16000;
        audio->waveform.mono.assign(160000,.1f);
        neolip::AudioPlayback player;player.load(audio);player.setVolume(.2f);player.play();advance(player);
        player.pause();const auto paused=player.position();std::this_thread::sleep_for(std::chrono::milliseconds(40));
        require(player.position()==paused&&!player.playing(),"Pause advanced source");
        player.seek(2);require(std::abs(player.position()-2)<1e-6,"Seek failed");
        player.setVolume(0);player.play();advance(player);require(player.pollError().empty(),"Unexpected device error");
        player.stop();require(player.position()==0&&!player.playing(),"Stop failed");
        player.load(audio);player.play();advance(player);player.stop();
        std::cout<<"PASS: real miniaudio null-device callback, pause, seek, mute, stop and reopen\n";return 0;
    } catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
