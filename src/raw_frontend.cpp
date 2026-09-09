#include "work_control.hpp"
#include "raw_frontend.hpp"
#include "feature_context.hpp"
#include "wav.hpp"
#include "audio.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <complex>
#include <cstdint>
#include <fstream>
#include <functional>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <sstream>

namespace cslu {
namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;

std::vector<double> hammingSquared(int n, double& sumHamming) {
    if (n <= 1) {
        throw std::runtime_error("Hamming window too short");
    }
    constexpr double twoPi = 6.283185307179586476925286766559;
    std::vector<double> w(static_cast<std::size_t>(n));
    sumHamming = 0.0;
    for (int i = 0; i < n; ++i) {
        if ((i & 63) == 0) checkWork();
        const double h = 0.54 - 0.46 * std::cos(twoPi * static_cast<double>(i) / static_cast<double>(n - 1));
        sumHamming += h;
        w[static_cast<std::size_t>(i)] = h * h;
    }
    return w;
}



[[maybe_unused]] constexpr int kIntensitySampleRate = 16000;
constexpr int kIntensityHopSamples = 80;      // 5 ms at 16 kHz
constexpr int kIntensitySmallWinSamples = 640; // 40 ms at 16 kHz
constexpr int kIntensityLargeWinSamples = 4000; // 250 ms at 16 kHz
constexpr int kIntensityDeltaFrames = 4;      // 20 ms / 5 ms

const std::array<std::array<double, 64>, 7> kIntensityFilters = {{
    {{ // pass210to430
        -2.2067814000e-03, -2.6527002000e-03, -3.2506624000e-03, -4.0385172000e-03,
        -5.0367081000e-03, -6.2447440000e-03, -7.6389157000e-03, -9.1714689000e-03,
        -1.0771360000e-02, -1.2346628000e-02, -1.3788305000e-02, -1.4975712000e-02,
        -1.5782868000e-02, -1.6085672000e-02, -1.5769477000e-02, -1.4736604000e-02,
        -1.2913356000e-02, -1.0256097000e-02, -6.7560166000e-03, -2.4422269000e-03,
        2.6170169000e-03, 8.3151345000e-03, 1.4510092000e-02, 2.1029238000e-02,
        2.7675907000e-02, 3.4237472000e-02, 4.0494465000e-02, 4.6230299000e-02,
        5.1241082000e-02, 5.5345036000e-02, 5.8390985000e-02, 6.0265497000e-02,
        6.0898272000e-02, 6.0265497000e-02, 5.8390985000e-02, 5.5345036000e-02,
        5.1241082000e-02, 4.6230299000e-02, 4.0494465000e-02, 3.4237472000e-02,
        2.7675907000e-02, 2.1029238000e-02, 1.4510092000e-02, 8.3151345000e-03,
        2.6170169000e-03, -2.4422269000e-03, -6.7560166000e-03, -1.0256097000e-02,
        -1.2913356000e-02, -1.4736604000e-02, -1.5769477000e-02, -1.6085672000e-02,
        -1.5782868000e-02, -1.4975712000e-02, -1.3788305000e-02, -1.2346628000e-02,
        -1.0771360000e-02, -9.1714689000e-03, -7.6389157000e-03, -6.2447440000e-03,
        -5.0367081000e-03, -4.0385172000e-03, -3.2506624000e-03, -2.6527002000e-03
    }},
    {{ // pass430to710
        1.7774073000e-03, 2.3654662000e-03, 3.0442113000e-03, 3.8225155000e-03,
        4.6613330000e-03, 5.4668917000e-03, 6.0925760000e-03, 6.3503261000e-03,
        6.0310765000e-03, 4.9324065000e-03, 2.8903880000e-03, -1.8826981000e-04,
        -4.2981003000e-03, -9.3149997000e-03, -1.4988139000e-02, -2.0946050000e-02,
        -2.6717726000e-02, -3.1767805000e-02, -3.5543139000e-02, -3.7526514000e-02,
        -3.7292110000e-02, -3.4556752000e-02, -2.9221055000e-02, -2.1395386000e-02,
        -1.1406898000e-02, 2.1417056000e-04, 1.2768637000e-02, 2.5440432000e-02,
        3.7362057000e-02, 4.7687393000e-02, 5.5664831000e-02, 6.0703559000e-02,
        6.2426352000e-02, 6.0703559000e-02, 5.5664831000e-02, 4.7687393000e-02,
        3.7362057000e-02, 2.5440432000e-02, 1.2768637000e-02, 2.1417056000e-04,
        -1.1406898000e-02, -2.1395386000e-02, -2.9221055000e-02, -3.4556752000e-02,
        -3.7292110000e-02, -3.7526514000e-02, -3.5543139000e-02, -3.1767805000e-02,
        -2.6717726000e-02, -2.0946050000e-02, -1.4988139000e-02, -9.3149997000e-03,
        -4.2981003000e-03, -1.8826981000e-04, 2.8903880000e-03, 4.9324065000e-03,
        6.0310765000e-03, 6.3503261000e-03, 6.0925760000e-03, 5.4668917000e-03,
        4.6613330000e-03, 3.8225155000e-03, 3.0442113000e-03, 2.3654662000e-03
    }},
    {{ // pass710to1060
        2.4202929000e-04, -4.7223353000e-04, -1.3443592000e-03, -2.3792945000e-03,
        -3.5082723000e-03, -4.5546483000e-03, -5.2286033000e-03, -5.1620722000e-03,
        -3.9849893000e-03, -1.4315320000e-03, 2.5463045000e-03, 7.6874903000e-03,
        1.3382995000e-02, 1.8711351000e-02, 2.2556513000e-02, 2.3797049000e-02,
        2.1536953000e-02, 1.5333951000e-02, 5.3751445000e-03, -7.4455103000e-03,
        -2.1578088000e-02, -3.5002474000e-02, -4.5528094000e-02, -5.1161049000e-02,
        -5.0474051000e-02, -4.2908458000e-02, -2.8945687000e-02, -1.0106451000e-02,
        1.1232908000e-02, 3.2183298000e-02, 4.9789940000e-02, 6.1506876000e-02,
        6.5614824000e-02, 6.1506876000e-02, 4.9789940000e-02, 3.2183298000e-02,
        1.1232908000e-02, -1.0106451000e-02, -2.8945687000e-02, -4.2908458000e-02,
        -5.0474051000e-02, -5.1161049000e-02, -4.5528094000e-02, -3.5002474000e-02,
        -2.1578088000e-02, -7.4455103000e-03, 5.3751445000e-03, 1.5333951000e-02,
        2.1536953000e-02, 2.3797049000e-02, 2.2556513000e-02, 1.8711351000e-02,
        1.3382995000e-02, 7.6874903000e-03, 2.5463045000e-03, -1.4315320000e-03,
        -3.9849893000e-03, -5.1620722000e-03, -5.2286033000e-03, -4.5546483000e-03,
        -3.5082723000e-03, -2.3792945000e-03, -1.3443592000e-03, -4.7223353000e-04
    }},
    {{ // pass1060to1530
        -3.0876156000e-04, -5.7276340000e-04, -7.5807203000e-04, -6.9128294000e-04,
        -1.7295169000e-04, 9.2902753000e-04, 2.5336055000e-03, 4.2205186000e-03,
        5.2247512000e-03, 4.6265409000e-03, 1.7299472000e-03, -3.4856303000e-03,
        -1.0040227000e-02, -1.5961153000e-02, -1.8737826000e-02, -1.6164129000e-02,
        -7.3177608000e-03, 6.6989516000e-03, 2.2621295000e-02, 3.5683221000e-02,
        4.0989794000e-02, 3.5215225000e-02, 1.8047430000e-02, -7.1948274000e-03,
        -3.4118148000e-02, -5.4941409000e-02, -6.2853825000e-02, -5.4295614000e-02,
        -3.0371936000e-02, 3.1472616000e-03, 3.7334062000e-02, 6.2695126000e-02,
        7.2039970000e-02, 6.2695126000e-02, 3.7334062000e-02, 3.1472616000e-03,
        -3.0371936000e-02, -5.4295614000e-02, -6.2853825000e-02, -5.4941409000e-02,
        -3.4118148000e-02, -7.1948274000e-03, 1.8047430000e-02, 3.5215225000e-02,
        4.0989794000e-02, 3.5683221000e-02, 2.2621295000e-02, 6.6989516000e-03,
        -7.3177608000e-03, -1.6164129000e-02, -1.8737826000e-02, -1.5961153000e-02,
        -1.0040227000e-02, -3.4856303000e-03, 1.7299472000e-03, 4.6265409000e-03,
        5.2247512000e-03, 4.2205186000e-03, 2.5336055000e-03, 9.2902753000e-04,
        -1.7295169000e-04, -6.9128294000e-04, -7.5807203000e-04, -5.7276340000e-04
    }},
    {{ // pass1530to2180
        3.3841488000e-04, 1.0781149000e-03, 1.2520406000e-03, 7.9647025000e-04,
        -2.7259874000e-05, -6.8515412000e-04, -7.1367883000e-04, -1.9560214000e-04,
        9.5776945000e-05, -7.4392851000e-04, -2.7070141000e-03, -4.2268921000e-03,
        -2.8478966000e-03, 2.7623593000e-03, 1.0752399000e-02, 1.5909577000e-02,
        1.2400607000e-02, -1.6967592000e-03, -2.1147551000e-02, -3.4687052000e-02,
        -3.1109338000e-02, -7.2295643000e-03, 2.7615502000e-02, 5.4625539000e-02,
        5.5719903000e-02, 2.5302513000e-02, -2.4020541000e-02, -6.6499830000e-02,
        -7.7261416000e-02, -4.7355175000e-02, 9.5669524000e-03, 6.3760396000e-02,
        8.5869103000e-02, 6.3760396000e-02, 9.5669524000e-03, -4.7355175000e-02,
        -7.7261416000e-02, -6.6499830000e-02, -2.4020541000e-02, 2.5302513000e-02,
        5.5719903000e-02, 5.4625539000e-02, 2.7615502000e-02, -7.2295643000e-03,
        -3.1109338000e-02, -3.4687052000e-02, -2.1147551000e-02, -1.6967592000e-03,
        1.2400607000e-02, 1.5909577000e-02, 1.0752399000e-02, 2.7623593000e-03,
        -2.8478966000e-03, -4.2268921000e-03, -2.7070141000e-03, -7.4392851000e-04,
        9.5776945000e-05, -1.9560214000e-04, -7.1367883000e-04, -6.8515412000e-04,
        -2.7259874000e-05, 7.9647025000e-04, 1.2520406000e-03, 1.0781149000e-03
    }},
    {{ // pass2180to3060
        -6.8381936000e-05, -1.1976730000e-03, -1.4316337000e-03, 1.6440620000e-05,
        2.2322194000e-03, 2.7829165000e-03, 1.7724356000e-04, -3.6059723000e-03,
        -4.3497467000e-03, -5.1764841000e-04, 4.0325691000e-03, 4.2470970000e-03,
        5.5982094000e-04, -1.4683457000e-03, 4.7304460000e-04, 7.9071087000e-04,
        -5.7634346000e-03, -1.2482439000e-02, -4.9950476000e-03, 1.7760678000e-02,
        3.2473231000e-02, 1.3109593000e-02, -3.2474503000e-02, -5.8010106000e-02,
        -2.4888486000e-02, 4.6205302000e-02, 8.3704757000e-02, 3.8316569000e-02,
        -5.5003902000e-02, -1.0285961000e-01, -4.9987661000e-02, 5.6306662000e-02,
        1.0995539000e-01, 5.6306662000e-02, -4.9987661000e-02, -1.0285961000e-01,
        -5.5003902000e-02, 3.8316569000e-02, 8.3704757000e-02, 4.6205302000e-02,
        -2.4888486000e-02, -5.8010106000e-02, -3.2474503000e-02, 1.3109593000e-02,
        3.2473231000e-02, 1.7760678000e-02, -4.9950476000e-03, -1.2482439000e-02,
        -5.7634346000e-03, 7.9071087000e-04, 4.7304460000e-04, -1.4683457000e-03,
        5.5982094000e-04, 4.2470970000e-03, 4.0325691000e-03, -5.1764841000e-04,
        -4.3497467000e-03, -3.6059723000e-03, 1.7724356000e-04, 2.7829165000e-03,
        2.2322194000e-03, 1.6440620000e-05, -1.4316337000e-03, -1.1976730000e-03
    }},
    {{ // pass3060to4000
        -5.4324072000e-04, -4.7744035000e-04, 9.3710748000e-04, 1.4055037000e-03,
        -1.0302895000e-03, -2.9319698000e-03, 3.3065241000e-04, 4.6381315000e-03,
        1.5217968000e-03, -5.4601883000e-03, -3.9585991000e-03, 4.3868904000e-03,
        5.1470718000e-03, -1.7323811000e-03, -2.8067055000e-03, 2.8502022000e-07,
        -3.9438241000e-03, -3.2895279000e-03, 1.2822507000e-02, 1.5027034000e-02,
        -1.8188868000e-02, -3.5090683000e-02, 1.3194966000e-02, 5.8199407000e-02,
        6.4333395000e-03, -7.5072263000e-02, -3.9027575000e-02, 7.6334512000e-02,
        7.6239081000e-02, -5.7356288000e-02, -1.0587329000e-01, 2.1335082000e-02,
        1.1717547000e-01, 2.1335082000e-02, -1.0587329000e-01, -5.7356288000e-02,
        7.6239081000e-02, 7.6334512000e-02, -3.9027575000e-02, -7.5072263000e-02,
        6.4333395000e-03, 5.8199407000e-02, 1.3194966000e-02, -3.5090683000e-02,
        -1.8188868000e-02, 1.5027034000e-02, 1.2822507000e-02, -3.2895279000e-03,
        -3.9438241000e-03, 2.8502022000e-07, -2.8067055000e-03, -1.7323811000e-03,
        5.1470718000e-03, 4.3868904000e-03, -3.9585991000e-03, -5.4601883000e-03,
        1.5217968000e-03, 4.6381315000e-03, 3.3065241000e-04, -2.9319698000e-03,
        -1.0302895000e-03, 1.4055037000e-03, 9.3710748000e-04, -4.7744035000e-04
    }},
}};

// burst.dll band-pass filters from include/burst.h; Burst_compute passes length 0x40,
// so the mirrored 65th coefficient in the header is intentionally omitted.
const std::array<std::array<double, 64>, 7> kBurstFilters = {{
    {{ // pass210to430
        -1.4026079000e-04, 6.0540433000e-05, 3.8888747000e-04, 8.9112682000e-04,
        1.6106025000e-03, 2.5655260000e-03, 3.7249428000e-03, 4.9885952000e-03,
        6.1766287000e-03, 7.0340146000e-03, 7.2523511000e-03, 6.5086813000e-03,
        4.5176008000e-03, 1.0898263000e-03, -3.8118628000e-03, -1.0029314000e-02,
        -1.7187625000e-02, -2.4702632000e-02, -3.1820417000e-02, -3.7687332000e-02,
        -4.1444100000e-02, -4.2333291000e-02, -3.9806368000e-02, -3.3615382000e-02,
        -2.3875328000e-02, -1.1086332000e-02, 3.8902771000e-03, 1.9901907000e-02,
        3.5614252000e-02, 4.9648345000e-02, 6.0728844000e-02, 6.7825225000e-02,
        7.0268027000e-02, 6.7825225000e-02, 6.0728844000e-02, 4.9648345000e-02,
        3.5614252000e-02, 1.9901907000e-02, 3.8902771000e-03, -1.1086332000e-02,
        -2.3875328000e-02, -3.3615382000e-02, -3.9806368000e-02, -4.2333291000e-02,
        -4.1444100000e-02, -3.7687332000e-02, -3.1820417000e-02, -2.4702632000e-02,
        -1.7187625000e-02, -1.0029314000e-02, -3.8118628000e-03, 1.0898263000e-03,
        4.5176008000e-03, 6.5086813000e-03, 7.2523511000e-03, 7.0340146000e-03,
        6.1766287000e-03, 4.9885952000e-03, 3.7249428000e-03, 2.5655260000e-03,
        1.6106025000e-03, 8.9112682000e-04, 3.8888747000e-04, 6.0540433000e-05
    }},
    {{ // pass430to710
        1.2282338000e-04, -1.2774241000e-04, -2.1427825000e-04, -1.0560874000e-04,
        1.8361058000e-04, 5.4058009000e-04, 7.2283463000e-04, 3.9224013000e-04,
        -7.6346016000e-04, -2.8434650000e-03, -5.5501066000e-03, -8.1041879000e-03,
        -9.3430539000e-03, -8.0295421000e-03, -3.3157108000e-03, 4.7776602000e-03,
        1.5050837000e-02, 2.5151464000e-02, 3.2017420000e-02, 3.2679978000e-02,
        2.5229237000e-02, 9.6481583000e-03, -1.1785833000e-02, -3.4736462000e-02,
        -5.3706501000e-02, -6.3411797000e-02, -6.0270956000e-02, -4.3575016000e-02,
        -1.5959035000e-02, 1.7020440000e-02, 4.8142055000e-02, 7.0298080000e-02,
        7.8313723000e-02, 7.0298080000e-02, 4.8142055000e-02, 1.7020440000e-02,
        -1.5959035000e-02, -4.3575016000e-02, -6.0270956000e-02, -6.3411797000e-02,
        -5.3706501000e-02, -3.4736462000e-02, -1.1785833000e-02, 9.6481583000e-03,
        2.5229237000e-02, 3.2679978000e-02, 3.2017420000e-02, 2.5151464000e-02,
        1.5050837000e-02, 4.7776602000e-03, -3.3157108000e-03, -8.0295421000e-03,
        -9.3430539000e-03, -8.1041879000e-03, -5.5501066000e-03, -2.8434650000e-03,
        -7.6346016000e-04, 3.9224013000e-04, 7.2283463000e-04, 5.4058009000e-04,
        1.8361058000e-04, -1.0560874000e-04, -2.1427825000e-04, -1.2774241000e-04
    }},
    {{ // pass710to1060
        1.5194073000e-03, 1.4218718000e-03, 6.8011105000e-04, -4.4145279000e-04,
        -1.4400122000e-03, -1.7635079000e-03, -1.1925999000e-03, -1.3847577000e-04,
        5.1904787000e-04, 1.3437740000e-04, -9.1699316000e-04, -1.1368143000e-03,
        1.0726209000e-03, 5.7242057000e-03, 1.0201880000e-02, 1.0130276000e-02,
        2.2578164000e-03, -1.2236320000e-02, -2.6547065000e-02, -3.0896800000e-02,
        -1.8364684000e-02, 9.3636599000e-03, 4.0428168000e-02, 5.7591295000e-02,
        4.7693439000e-02, 1.0564561000e-02, -3.8419447000e-02, -7.4794238000e-02,
        -7.7795461000e-02, -4.2506671000e-02, 1.5945313000e-02, 6.9270956000e-02,
        9.0681542000e-02, 6.9270956000e-02, 1.5945313000e-02, -4.2506671000e-02,
        -7.7795461000e-02, -7.4794238000e-02, -3.8419447000e-02, 1.0564561000e-02,
        4.7693439000e-02, 5.7591295000e-02, 4.0428168000e-02, 9.3636599000e-03,
        -1.8364684000e-02, -3.0896800000e-02, -2.6547065000e-02, -1.2236320000e-02,
        2.2578164000e-03, 1.0130276000e-02, 1.0201880000e-02, 5.7242057000e-03,
        1.0726209000e-03, -1.1368143000e-03, -9.1699316000e-04, 1.3437740000e-04,
        5.1904787000e-04, -1.3847577000e-04, -1.1925999000e-03, -1.7635079000e-03,
        -1.4400122000e-03, -4.4145279000e-04, 6.8011105000e-04, 1.4218718000e-03
    }},
    {{ // pass1060to1530
        -2.4872469000e-04, -8.9061918000e-04, -7.8991385000e-04, 5.9997137000e-04,
        2.2928138000e-03, 2.0913876000e-03, -9.8193041000e-04, -4.4590754000e-03,
        -4.0908399000e-03, 1.0276496000e-03, 6.0466288000e-03, 5.2801879000e-03,
        -4.7354182000e-04, -4.3124733000e-03, -2.4438795000e-03, 8.5488677000e-07,
        -3.3893060000e-03, -8.1191923000e-03, -1.5399793000e-03, 1.7617070000e-02,
        2.8332753000e-02, 7.4857444000e-03, -3.5689556000e-02, -5.6295146000e-02,
        -1.9068628000e-02, 5.2317335000e-02, 8.5877765000e-02, 3.4887585000e-02,
        -6.1878305000e-02, -1.0861050000e-01, -5.0731791000e-02, 6.1119766000e-02,
        1.1715405000e-01, 6.1119766000e-02, -5.0731791000e-02, -1.0861050000e-01,
        -6.1878305000e-02, 3.4887585000e-02, 8.5877765000e-02, 5.2317335000e-02,
        -1.9068628000e-02, -5.6295146000e-02, -3.5689556000e-02, 7.4857444000e-03,
        2.8332753000e-02, 1.7617070000e-02, -1.5399793000e-03, -8.1191923000e-03,
        -3.3893060000e-03, 8.5488677000e-07, -2.4438795000e-03, -4.3124733000e-03,
        -4.7354182000e-04, 5.2801879000e-03, 6.0466288000e-03, 1.0276496000e-03,
        -4.0908399000e-03, -4.4590754000e-03, -9.8193041000e-04, 2.0913876000e-03,
        2.2928138000e-03, 5.9997137000e-04, -7.8991385000e-04, -8.9061918000e-04
    }},
    {{ // pass1530to2180
        -1.3231477000e-03, 6.3729539000e-04, 1.7751770000e-03, -3.1525181000e-04,
        -1.9813766000e-03, -1.2050131000e-04, 1.3100188000e-03, 1.3328728000e-04,
        8.1573326000e-04, 1.3699331000e-03, -4.0887054000e-03, -5.3114595000e-03,
        6.9341944000e-03, 1.1160062000e-02, -7.2719557000e-03, -1.6080684000e-02,
        4.3121656000e-03, 1.5566078000e-02, -2.8175546000e-04, -5.7604433000e-03,
        6.0289621000e-04, -1.3597250000e-02, -1.1751147000e-02, 3.7498642000e-02,
        3.7509615000e-02, -5.6773628000e-02, -7.5657883000e-02, 6.1967915000e-02,
        1.1730641000e-01, -4.8181609000e-02, -1.4980586000e-01, 1.8180611000e-02,
        1.6209855000e-01, 1.8180611000e-02, -1.4980586000e-01, -4.8181609000e-02,
        1.1730641000e-01, 6.1967915000e-02, -7.5657883000e-02, -5.6773628000e-02,
        3.7509615000e-02, 3.7498642000e-02, -1.1751147000e-02, -1.3597250000e-02,
        6.0289621000e-04, -5.7604433000e-03, -2.8175546000e-04, 1.5566078000e-02,
        4.3121656000e-03, -1.6080684000e-02, -7.2719557000e-03, 1.1160062000e-02,
        6.9341944000e-03, -5.3114595000e-03, -4.0887054000e-03, 1.3699331000e-03,
        8.1573326000e-04, 1.3328728000e-04, 1.3100188000e-03, -1.2050131000e-04,
        -1.9813766000e-03, -3.1525181000e-04, 1.7751770000e-03, 6.3729539000e-04
    }},
    {{ // pass2180to3060
        1.5789828000e-03, -9.3411149000e-04, -6.9377560000e-04, 1.2338019000e-03,
        -3.1391676000e-04, 1.6395188000e-04, -1.6381719000e-03, 1.2732529000e-03,
        3.2916266000e-03, -6.7099687000e-03, 2.2285638000e-03, 5.8301123000e-03,
        -6.4891633000e-03, 6.5455622000e-04, -7.9164244000e-04, 6.7216413000e-03,
        -9.2534678000e-04, -1.8902091000e-02, 2.4502056000e-02, 1.5192900000e-03,
        -2.9077876000e-02, 2.1510947000e-02, 2.4531400000e-03, 1.7519810000e-03,
        -1.8513911000e-02, -1.4241094000e-02, 8.3869888000e-02, -7.7387997000e-02,
        -5.5645110000e-02, 1.7830594000e-01, -1.1326006000e-01, -1.0088226000e-01,
        2.2043263000e-01, -1.0088226000e-01, -1.1326006000e-01, 1.7830594000e-01,
        -5.5645110000e-02, -7.7387997000e-02, 8.3869888000e-02, -1.4241094000e-02,
        -1.8513911000e-02, 1.7519810000e-03, 2.4531400000e-03, 2.1510947000e-02,
        -2.9077876000e-02, 1.5192900000e-03, 2.4502056000e-02, -1.8902091000e-02,
        -9.2534678000e-04, 6.7216413000e-03, -7.9164244000e-04, 6.5455622000e-04,
        -6.4891633000e-03, 5.8301123000e-03, 2.2285638000e-03, -6.7099687000e-03,
        3.2916266000e-03, 1.2732529000e-03, -1.6381719000e-03, 1.6395188000e-04,
        -3.1391676000e-04, 1.2338019000e-03, -6.9377560000e-04, -9.3411149000e-04
    }},
    {{ // pass3060to4000
        -7.9487408000e-04, 6.5938618000e-04, -1.4758081000e-04, -6.0196535000e-04,
        1.2675091000e-03, -1.4006124000e-03, 6.5381262000e-04, 8.9931638000e-04,
        -2.5790811000e-03, 3.2856202000e-03, -2.0966877000e-03, -9.9414984000e-04,
        4.6903383000e-03, -6.7742210000e-03, 5.2700149000e-03, 1.4567913000e-04,
        -7.3602620000e-03, 1.2388113000e-02, -1.1323033000e-02, 2.8377867000e-03,
        1.0185755000e-02, -2.1142535000e-02, 2.2582459000e-02, -1.0407873000e-02,
        -1.2684465000e-02, 3.6602817000e-02, -4.7035316000e-02, 3.1483079000e-02,
        1.4401337000e-02, -8.3238657000e-02, 1.5718053000e-01, -2.1393225000e-01,
        2.3519814000e-01, -2.1393225000e-01, 1.5718053000e-01, -8.3238657000e-02,
        1.4401337000e-02, 3.1483079000e-02, -4.7035316000e-02, 3.6602817000e-02,
        -1.2684465000e-02, -1.0407873000e-02, 2.2582459000e-02, -2.1142535000e-02,
        1.0185755000e-02, 2.8377867000e-03, -1.1323033000e-02, 1.2388113000e-02,
        -7.3602620000e-03, 1.4567913000e-04, 5.2700149000e-03, -6.7742210000e-03,
        4.6903383000e-03, -9.9414984000e-04, -2.0966877000e-03, 3.2856202000e-03,
        -2.5790811000e-03, 8.9931638000e-04, 6.5381262000e-04, -1.4006124000e-03,
        1.2675091000e-03, -6.0196535000e-04, -1.4758081000e-04, 6.5938618000e-04
    }},
}};
const std::array<double, 7> kBurstBandScales = {{
    0.103407291404, 0.33159180554, 0.580529833714, 0.817128897654, 0.999437085376, 1, 0.78318162382
}};
[[maybe_unused]] constexpr int kBurstSampleRate = 8000;
constexpr int kBurstSamplesPerMs = 8;
constexpr int kBurstFrameSamples = 40; // 5 ms at 8 kHz
constexpr int kBurstGroupSamples = kBurstFrameSamples / kBurstSamplesPerMs;
constexpr int kBurstLargeWinSamples = 176; // 22 ms
constexpr int kBurstSmallWinSamples = 192; // 24 ms
constexpr int kBurstDeltaFrames = 14; // 14 ms at the internal 1 ms step
constexpr int kBurstEffectiveShiftFrames = 12; // recovered burst.dll post-compute delay: large_win/2 in 1 ms units + 1
constexpr double kBurstPulseThreshold = 0.075;

std::vector<double> computeDeltaCslu(const std::vector<double>& input, int deltaFrames) {
    std::vector<double> out(input.size(), 0.0);
    if (deltaFrames <= 0 || input.size() <= static_cast<std::size_t>(2 * deltaFrames)) {
        return out;
    }
    double denom = 0.0;
    for (int k = 1; k <= deltaFrames; ++k) {
        denom += static_cast<double>(k * k);
    }
    denom *= 2.0;
    for (std::size_t r = static_cast<std::size_t>(deltaFrames);
         r + static_cast<std::size_t>(deltaFrames) < input.size(); ++r) {
        double num = 0.0;
        for (int k = 1; k <= deltaFrames; ++k) {
            num += static_cast<double>(k) * (input[r + static_cast<std::size_t>(k)] - input[r - static_cast<std::size_t>(k)]);
        }
        out[r] = num / denom;
    }
    return out;
}


std::vector<double> computeDeltaCsluPartial(const std::vector<double>& input, int deltaFrames) {
    // burst.dll's local delta helper uses all available symmetric samples at
    // the signal edges. It does not zero the first/last deltaFrames rows.
    std::vector<double> out(input.size(), 0.0);
    if (deltaFrames <= 0 || input.empty()) {
        return out;
    }
    for (std::size_t r = 0; r < input.size(); ++r) {
        if ((r & 63) == 0) checkWork();
        double numerator = 0.0;
        double denom = 0.0;
        for (int k = 1; k <= deltaFrames; ++k) {
            const std::size_t ku = static_cast<std::size_t>(k);
            if (r >= ku && r + ku < input.size()) {
                numerator += static_cast<double>(k) * (input[r + ku] - input[r - ku]);
                denom += static_cast<double>(k * k);
            }
        }
        if (denom != 0.0) {
            out[r] = numerator / (2.0 * denom);
        } else {
            // burst.dll's x87 delta helper does not special-case the
            // all-out-of-range edge row; 0/0 propagates as NaN, and
            // Burst_compute's later threshold comparison maps that row to
            // the configured floor value.  Preserve that behavior for the
            // diagnostic burst band columns.
            out[r] = std::numeric_limits<double>::quiet_NaN();
        }
    }
    return out;
}

std::vector<double> csluFir64(const std::vector<double>& input, const std::array<double, 64>& coeff) {
    std::vector<double> out(input.size(), 0.0);
    if (input.empty()) {
        return out;
    }
    std::array<double, 64> ring{};
    ring.fill(input.front());
    for (std::size_t i = 0; i < input.size(); ++i) {
        if ((i & 63) == 0) checkWork();
        ring[i % ring.size()] = input[i];
        double acc = 0.0;
        std::size_t idx = (i + 1U) % ring.size();
        for (std::size_t k = 0; k < ring.size(); ++k) {
            acc += ring[idx] * coeff[k];
            idx = (idx + 1U) % ring.size();
        }
        out[i] = acc;
    }
    return out;
}

std::vector<double> burstFir64(const std::vector<double>& input, const std::array<double, 64>& coeff) {
    // Exact translation of the FIR helper used by burst.dll before Burst_compute.
    // It is non-causal: it preloads input[0..63], emits output[0] from the
    // forward window input[1..64], and flushes the tail by repeating the DLL's
    // circular-buffer predecessor. This is deliberately separate from csluFir64,
    // because the existing intensity path matches the CSLU trace with its own
    // causal wrapper behavior.
    constexpr std::size_t N = 64;
    std::vector<double> out(input.size(), 0.0);
    if (input.empty()) {
        return out;
    }
    std::array<double, N> ring{};
    for (std::size_t i = 0; i < N; ++i) {
        if ((i & 63) == 0) checkWork();
        ring[i] = (i < input.size()) ? input[i] : 0.0;
    }
    std::size_t phase = N;
    for (std::size_t outIndex = 0; outIndex < input.size(); ++outIndex, ++phase) {
        const double next = (phase < input.size()) ? input[phase] : ring[(phase - 1U) % N];
        ring[phase % N] = next;
        double acc = 0.0;
        std::size_t idx = (phase + 1U) % N;
        for (std::size_t k = 0; k < N; ++k) {
            acc += ring[idx] * coeff[k];
            idx = (idx + 1U) % N;
        }
        out[outIndex] = acc;
    }
    return out;
}

std::vector<double> intensityWindowLogEnergy(const std::vector<double>& squared,
                                             int hopSamples,
                                             int winSamples,
                                             double floorEnergy) {
    double sumHamming = 0.0;
    const std::vector<double> win2 = hammingSquared(winSamples, sumHamming);
    const int half = winSamples / 2;

    std::vector<double> out;
    out.reserve(squared.size() / static_cast<std::size_t>(hopSamples) + 1U);
    int center = 0;
    int right = half;
    while (center < static_cast<int>(squared.size())) {
        double weighted = 0.0;
        int k = 0;
        for (int sampleIndex = center - half; sampleIndex < right; ++sampleIndex) {
            if (sampleIndex >= 0 && static_cast<std::size_t>(sampleIndex) < squared.size()) {
                // Intensity_discrimination advances the Hamming pointer only for
                // valid samples at the signal edges. This differs from logenergy.dll.
                weighted += squared[static_cast<std::size_t>(sampleIndex)] * win2[static_cast<std::size_t>(k)];
                ++k;
            }
        }
        double e = weighted / sumHamming;
        if (e < floorEnergy) {
            e = floorEnergy;
        }
        out.push_back(10.0 * std::log10(e));
        center += hopSamples;
        right += hopSamples;
    }
    return out;
}

struct IntensityBandResult {
    std::vector<double> value;
    std::vector<double> delta;
};

IntensityBandResult computeIntensityBand(const std::vector<double>& signal) {
    std::vector<double> squared(signal.size(), 0.0);
    for (std::size_t i = 0; i < signal.size(); ++i) {
        if ((i & 63) == 0) checkWork();
        squared[i] = signal[i] * signal[i];
    }
    const std::vector<double> smallLog = intensityWindowLogEnergy(
        squared, kIntensityHopSamples, kIntensitySmallWinSamples, 1.0);
    const std::vector<double> smallDelta = computeDeltaCslu(smallLog, kIntensityDeltaFrames);
    const std::vector<double> largeLog = intensityWindowLogEnergy(
        squared, kIntensityHopSamples, kIntensityLargeWinSamples, 2.0);

    IntensityBandResult out;
    out.value.assign(smallLog.size(), 0.0);
    constexpr double negLimit = -10.0;
    constexpr double posLimit = 10.0;
    for (std::size_t r = 0; r < out.value.size(); ++r) {
        if ((r & 63) == 0) checkWork();
        const double denom = (r < largeLog.size()) ? largeLog[r] : largeLog.back();
        const double ratio = smallDelta[r] / denom + 1.0;
        double v = (ratio > 0.0) ? (10.0 * std::log10(ratio)) : negLimit;
        if (v < negLimit) v = negLimit;
        if (v > posLimit) v = posLimit;
        out.value[r] = v;
    }
    out.delta = computeDeltaCslu(out.value, kIntensityDeltaFrames);
    return out;
}

std::vector<double> burstWindowEnergy(const std::vector<double>& squared,
                                      int hopSamples,
                                      int winSamples,
                                      double floorEnergy) {
    double sumHamming = 0.0;
    const std::vector<double> win2 = hammingSquared(winSamples, sumHamming);
    const int half = winSamples / 2;

    std::vector<double> out;
    out.reserve((squared.size() + static_cast<std::size_t>(hopSamples) - 1U) /
                static_cast<std::size_t>(hopSamples));
    int center = 0;
    int right = half;
    while (center < static_cast<int>(squared.size())) {
        double weighted = 0.0;
        int k = 0;
        for (int sampleIndex = center - half; sampleIndex < right; ++sampleIndex) {
            if (sampleIndex >= 0 && static_cast<std::size_t>(sampleIndex) < squared.size()) {
                weighted += squared[static_cast<std::size_t>(sampleIndex)] * win2[static_cast<std::size_t>(k)];
                ++k;
            }
        }
        double e = weighted / sumHamming;
        if (e < floorEnergy) {
            e = floorEnergy;
        }
        out.push_back(e);
        center += hopSamples;
        right += hopSamples;
    }
    return out;
}

std::vector<double> computeBurstBandIntermediate(const std::vector<double>& samples,
                                                 const std::array<double, 64>& coeff,
                                                 double scale) {
    const std::vector<double> filtered = burstFir64(samples, coeff);
    std::vector<double> squared(filtered.size(), 0.0);
    if (filtered.size() > 1) {
        // burst.dll allocates a work buffer of the input length, fills
        // samples 0..N-2 with squared FIR output, and leaves the final
        // slot effectively zero for the traced FA path.  The extra logical
        // row affects the last partial-delta frames used by burst_mx's
        // diagnostic band columns.
        for (std::size_t i = 0; i + 1U < filtered.size(); ++i) {
            if ((i & 63) == 0) checkWork();
            squared[i] = filtered[i] * filtered[i];
        }
    }

    const std::vector<double> small = burstWindowEnergy(
        squared, kBurstSamplesPerMs, kBurstSmallWinSamples, 1.0);
    const std::vector<double> large = burstWindowEnergy(
        squared, kBurstSamplesPerMs, kBurstLargeWinSamples, 1.0);
    const std::vector<double> delta = computeDeltaCsluPartial(small, kBurstDeltaFrames);

    std::vector<double> out(small.size(), 0.0);
    const double cutoff = std::pow(10.0, -0.1) - 1.0; // disabled-threshold branch used by Burst_compute
    for (std::size_t r = 0; r < out.size(); ++r) {
        if ((r & 63) == 0) checkWork();
        const double denom = (r < large.size() && large[r] != 0.0) ? large[r] : 1.0;
        const double ratio = delta[r] / denom;
        double v = (!std::isfinite(ratio) || ratio < cutoff || (1.0 + ratio) <= 0.0)
            ? -1.0
            : 10.0 * std::log10(1.0 + ratio);
        v *= scale * 0.025;
        if (v < -1.0) v = -1.0;
        if (v > 1.0) v = 1.0;
        out[r] = v;
    }
    return out;
}

std::vector<double> computeBurstPulseIntermediate(const std::array<std::vector<double>, 7>& bands) {
    std::size_t rows = 0;
    for (const auto& b : bands) {
        rows = std::max(rows, b.size());
    }
    std::vector<double> out(rows, 0.0);
    for (std::size_t r = 0; r < rows; ++r) {
        if ((r & 63) == 0) checkWork();
        double prodP = 1.0;
        double prodQ = 1.0;
        for (const auto& b : bands) {
            const double x = (r < b.size()) ? b[r] : 0.0;
            double p = 0.5 * x + 0.5;
            if (p < 0.0) p = 0.0;
            if (p > 1.0) p = 1.0;
            prodP *= p;
            prodQ *= (1.0 - p);
        }
        const double denom = prodP + prodQ;
        double v = (denom != 0.0) ? (2.0 * (prodP / denom) - 1.0) : 0.0;
        if (v < kBurstPulseThreshold) {
            v = 0.0;
        }
        out[r] = v;
    }
    return out;
}

std::vector<double> shiftRightWithZeros(const std::vector<double>& in, int shift) {
    std::vector<double> out(in.size(), 0.0);
    if (shift <= 0) {
        return in;
    }
    const std::size_t s = static_cast<std::size_t>(shift);
    if (s < in.size()) {
        std::copy(in.begin(), in.end() - static_cast<std::ptrdiff_t>(s), out.begin() + static_cast<std::ptrdiff_t>(s));
    }
    return out;
}

} // namespace

Pcm16Mono readPcm16Mono(const std::filesystem::path& path) {
    const auto audio = neolip::decodeAudio(path);
    if (audio->channelChoiceRequired) throw std::runtime_error(audio->warnings.front().message);
    Pcm16Mono result; result.sampleRate = audio->waveform.sampleRate;
    result.samples.reserve(audio->waveform.mono.size());
    for (const float sample : audio->waveform.mono) result.samples.push_back(static_cast<double>(sample) * 32768.0);
    return result;
}
Matrix computeLogEnergy(const std::vector<double>& samples,
                        int sampleRate,
                        double winMs,
                        double frameMs,
                        std::size_t outputRows) {
    if (sampleRate <= 0 || winMs <= 0.0 || frameMs <= 0.0) {
        throw std::runtime_error("invalid logenergy arguments");
    }
    const int winSamples = static_cast<int>(static_cast<double>(sampleRate) * winMs * 0.001);
    const int stepSamples = static_cast<int>(static_cast<double>(sampleRate) * frameMs * 0.001);
    if (winSamples <= 1 || stepSamples <= 0) {
        throw std::runtime_error("invalid derived logenergy geometry");
    }
    if (outputRows == 0) {
        outputRows = samples.size() / static_cast<std::size_t>(stepSamples);
    }

    std::vector<double> squared(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        if ((i & 63) == 0) checkWork();
        squared[i] = samples[i] * samples[i];
    }

    double sumHamming = 0.0;
    const std::vector<double> win2 = hammingSquared(winSamples, sumHamming);
    const int half = winSamples / 2;

    Matrix out;
    out.rows = outputRows;
    out.cols = 1;
    out.data.assign(outputRows, 0.0);
    for (std::size_t frame = 0; frame < outputRows; ++frame) {
        if ((frame & 63) == 0) checkWork();
        const long center = static_cast<long>(frame) * static_cast<long>(stepSamples);
        const long start = center - half;
        double weighted = 0.0;
        for (int k = 0; k < winSamples; ++k) {
            const long sampleIndex = start + k;
            if (sampleIndex >= 0 && static_cast<std::size_t>(sampleIndex) < squared.size()) {
                weighted += win2[static_cast<std::size_t>(k)] * squared[static_cast<std::size_t>(sampleIndex)];
            }
        }
        double energy = weighted / sumHamming;
        if (energy < 1.0) {
            energy = 1.0;
        }
        out(frame, 0) = 10.0 * std::log10(energy);
    }
    return out;
}

Matrix computeIntensityEnergyCol0(const std::vector<double>& samples,
                                  int sampleRate,
                                  double frameMs,
                                  double smallWinMs,
                                  std::size_t outputRows,
                                  int robustTopRank) {
    Matrix raw = computeLogEnergy(samples, sampleRate, smallWinMs, frameMs, outputRows);
    if (raw.rows == 0) {
        return raw;
    }
    const double minLog = *std::min_element(raw.data.begin(), raw.data.end());
    std::vector<double> descending = raw.data;
    std::sort(descending.begin(), descending.end(), std::greater<double>());
    const int safeRank = std::max(1, robustTopRank);
    const std::size_t idx = std::min<std::size_t>(static_cast<std::size_t>(safeRank - 1), descending.size() - 1);
    double denom = descending[idx] - minLog;
    if (denom == 0.0) {
        denom = 1.0;
    }
    for (double& v : raw.data) {
        v = (v - minLog) / denom;
    }
    return raw;
}



Matrix computeIntensityDiscrimination16k(const std::vector<double>& samples,
                                         std::size_t outputRows) {
    if (outputRows == 0) {
        outputRows = samples.size() / static_cast<std::size_t>(kIntensityHopSamples);
    }
    Matrix out;
    out.rows = outputRows;
    out.cols = 16;
    out.data.assign(out.rows * out.cols, 0.0);

    const IntensityBandResult full = computeIntensityBand(samples);
    for (std::size_t r = 0; r < out.rows; ++r) {
        if ((r & 63) == 0) checkWork();
        // The original Intensity_discrimination wrapper copies the full-band pair
        // from one double before the returned work buffer. In practice this yields
        // a one-frame right shift with a zero first row; preserve it for CSLU
        // trace compatibility.
        if (r > 0 && r - 1 < full.value.size()) {
            out(r, 0) = full.value[r - 1];
            out(r, 1) = full.delta[r - 1];
        }
    }

    for (std::size_t band = 0; band < kIntensityFilters.size(); ++band) {
        const std::vector<double> filtered = csluFir64(samples, kIntensityFilters[band]);
        const IntensityBandResult b = computeIntensityBand(filtered);
        const std::size_t col = 2 + band * 2;
        for (std::size_t r = 0; r < out.rows; ++r) {
            if ((r & 63) == 0) checkWork();
            if (r < b.value.size()) {
                out(r, col) = b.value[r];
                out(r, col + 1) = b.delta[r];
            }
        }
    }
    return out;
}


Matrix computeBurstMx8k(const std::vector<double>& samples, std::size_t outputRows) {
    if (outputRows == 0) {
        outputRows = samples.size() / static_cast<std::size_t>(kBurstFrameSamples);
    }

    std::array<std::vector<double>, 7> bands{};
    for (std::size_t band = 0; band < bands.size(); ++band) {
        bands[band] = computeBurstBandIntermediate(samples, kBurstFilters[band], kBurstBandScales[band]);
    }
    std::vector<double> pulses = computeBurstPulseIntermediate(bands);
    // Burst_compute then delays all seven bands and the derived pulse stream by
    // floor((large_win_samples / 2) / samples_per_ms) + 1 = 12 one-ms ticks.
    // This final shift is what restores CSLU's startup and flush frames.
    for (std::size_t band = 0; band < bands.size(); ++band) {
        bands[band] = shiftRightWithZeros(bands[band], kBurstEffectiveShiftFrames);
    }
    pulses = shiftRightWithZeros(pulses, kBurstEffectiveShiftFrames);

    Matrix out;
    out.rows = outputRows;
    out.cols = 8;
    out.data.assign(out.rows * out.cols, 0.0);
    for (std::size_t r = 0; r < outputRows; ++r) {
        if ((r & 63) == 0) checkWork();
        const std::size_t start = r * static_cast<std::size_t>(kBurstGroupSamples);
        const std::size_t end = start + static_cast<std::size_t>(kBurstGroupSamples);
        for (std::size_t c = 0; c < 8; ++c) {
            const std::vector<double>& src = (c < 7) ? bands[c] : pulses;
            double best = 0.0;
            bool have = false;
            for (std::size_t i = start; i < end && i < src.size(); ++i) {
                if (!have || std::abs(src[i]) > std::abs(best)) {
                    best = src[i];
                    have = true;
                }
            }
            out(r, c) = have ? best : 0.0;
        }
    }
    return out;
}

Matrix computeBurstPulsesCol7(const Matrix& burstMx) {
    if (burstMx.cols < 8) {
        throw std::runtime_error("burstMx must have at least 8 columns");
    }
    Matrix out;
    out.rows = burstMx.rows;
    out.cols = 1;
    out.data.assign(out.rows, 0.0);
    for (std::size_t r = 0; r < out.rows; ++r) {
        if ((r & 63) == 0) checkWork();
        out(r, 0) = burstMx(r, 7);
    }
    return out;
}

ValidatedEnergyFeatures computeValidatedEnergyFeatures16k(const Pcm16Mono& wav16k) {
    if (wav16k.sampleRate != 16000) {
        throw std::runtime_error("validated FA energy path expects 16 kHz input");
    }
    ValidatedEnergyFeatures out;
    out.specEnergy24 = computeLogEnergy(wav16k.samples, 16000, 24.0, 5.0);
    out.specEnergyBigwin100 = computeLogEnergy(wav16k.samples, 16000, 100.0, 5.0, out.specEnergy24.rows);

    out.specEnergyNorm24 = out.specEnergy24;
    double minBig = std::numeric_limits<double>::infinity();
    double maxBig = -std::numeric_limits<double>::infinity();
    for (double v : out.specEnergyBigwin100.data) {
        minBig = std::min(minBig, v);
        maxBig = std::max(maxBig, v);
    }
    double denom = maxBig - minBig;
    if (denom == 0.0) {
        denom = 1.0;
    }
    for (double& v : out.specEnergyNorm24.data) {
        v = (v - minBig) * (2.0 / denom) - 1.0;
    }

    out.intensityEnergyCol0 = computeIntensityEnergyCol0(wav16k.samples, 16000, 5.0, 40.0, out.specEnergy24.rows, 10);
    return out;
}

namespace {

std::uint16_t u16leForWrite(std::int16_t v) {
    return static_cast<std::uint16_t>(v);
}

void writeLe16(std::ofstream& out, std::uint16_t v) {
    const char b[2] = {static_cast<char>(v & 0xffU), static_cast<char>((v >> 8) & 0xffU)};
    out.write(b, 2);
}

void writeLe32(std::ofstream& out, std::uint32_t v) {
    const char b[4] = {
        static_cast<char>(v & 0xffU),
        static_cast<char>((v >> 8) & 0xffU),
        static_cast<char>((v >> 16) & 0xffU),
        static_cast<char>((v >> 24) & 0xffU)};
    out.write(b, 4);
}

std::int16_t truncClipPcm16(double v) {
    if (v < -32768.0) return static_cast<std::int16_t>(-32768);
    if (v > 32767.0) return static_cast<std::int16_t>(32767);
    return static_cast<std::int16_t>(static_cast<int>(v));
}

std::size_t nextPow2(std::size_t n) {
    std::size_t p = 1;
    while (p < n) p <<= 1U;
    return p;
}

void fftRadix2(std::vector<std::complex<double>>& a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        if ((i & 63) == 0) checkWork();
        std::size_t bit = n >> 1U;
        for (; j & bit; bit >>= 1U) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    constexpr double pi = 3.141592653589793238462643383279502884;
    for (std::size_t len = 2; len <= n; len <<= 1U) {
        const double ang = -2.0 * pi / static_cast<double>(len);
        const std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            if ((i & 63) == 0) checkWork();
            std::complex<double> w(1.0, 0.0);
            for (std::size_t j = 0; j < len / 2; ++j) {
                const std::complex<double> u = a[i + j];
                const std::complex<double> v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

double hzToCsluBark(double hz) {
    return 6.0 * std::asinh(hz / 600.0);
}

double barkToHz(double bark) {
    return 600.0 * std::sinh(bark / 6.0);
}

Matrix padFeatureLikeTclForPlp(const Matrix& initFeat, std::size_t padFrames, std::size_t outputRows) {
    if (initFeat.rows == 0 || initFeat.cols == 0) return initFeat;
    const std::size_t rows = outputRows ? outputRows : initFeat.rows + 2U * padFrames;
    Matrix out;
    out.rows = rows;
    out.cols = initFeat.cols;
    out.data.assign(out.rows * out.cols, 0.0);
    for (std::size_t r = 0; r < rows; ++r) {
        if ((r & 63) == 0) checkWork();
        std::size_t src = 0;
        if (r < padFrames) {
            src = 0;
        } else if (r - padFrames < initFeat.rows) {
            src = r - padFrames;
        } else {
            src = initFeat.rows - 1U;
        }
        for (std::size_t c = 0; c < initFeat.cols; ++c) {
            out(r, c) = initFeat(src, c);
        }
    }
    return out;
}

Matrix resizeEnergyLikeTcl(const Matrix& energyNorm, std::size_t rows) {
    if (energyNorm.cols != 1) throw std::runtime_error("expected one-column energy matrix");
    Matrix out;
    out.rows = rows;
    out.cols = 1;
    out.data.assign(rows, 0.0);
    for (std::size_t r = 0; r < rows; ++r) {
        if ((r & 63) == 0) checkWork();
        if (r < energyNorm.rows) {
            out(r, 0) = energyNorm(r, 0);
        } else if (energyNorm.rows > 0) {
            out(r, 0) = energyNorm(energyNorm.rows - 1U, 0);
        }
    }
    return out;
}

} // namespace

Pcm16Mono applyPrepDcrmIir(const Pcm16Mono& wav, double tauMs) {
    if (wav.sampleRate == 0 || tauMs <= 0.0) {
        throw std::runtime_error("applyPrepDcrmIir requires positive sample rate and tau");
    }
    // prep.dll dcrmInit/dcrm disassembly: a = pow(0.5, 0.001 * rate / tau).
    // Tcl calls `prep !dc initialize -tau 300 -rate <samplerate>` in the FA path.
    const float a = static_cast<float>(std::pow(0.5, 0.001 * static_cast<double>(wav.sampleRate) / tauMs));
    const float oneMinusA = static_cast<float>(1.0f - a);
    float mean = 0.0f;
    Pcm16Mono out;
    out.sampleRate = wav.sampleRate;
    out.samples.resize(wav.samples.size());
    for (std::size_t i = 0; i < wav.samples.size(); ++i) {
        if ((i & 63) == 0) checkWork();
        const double x = wav.samples[i];
        double y = x - static_cast<double>(mean);
        if (y < -32768.0) y = -32768.0;
        if (y > 32767.0) y = 32767.0;
        // The 32-bit Windows path uses an integer conversion equivalent to truncation toward zero.
        out.samples[i] = static_cast<double>(static_cast<int>(y));
        mean = static_cast<float>(static_cast<double>(mean) * static_cast<double>(a) + x * static_cast<double>(oneMinusA));
    }
    return out;
}

Matrix pcmSamplesColumn(const Pcm16Mono& wav) {
    Matrix m;
    m.rows = wav.samples.size();
    m.cols = 1;
    m.data = wav.samples;
    return m;
}

void writePcm16Mono(const Pcm16Mono& wav, const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write WAV: " + path.string());
    const std::uint16_t channels = 1;
    const std::uint16_t bits = 16;
    const std::uint16_t blockAlign = channels * bits / 8;
    const std::uint32_t byteRate = wav.sampleRate * blockAlign;
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(wav.samples.size() * sizeof(std::int16_t));
    out.write("RIFF", 4);
    writeLe32(out, 36U + dataBytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    writeLe32(out, 16U);
    writeLe16(out, 1U);
    writeLe16(out, channels);
    writeLe32(out, wav.sampleRate);
    writeLe32(out, byteRate);
    writeLe16(out, blockAlign);
    writeLe16(out, bits);
    out.write("data", 4);
    writeLe32(out, dataBytes);
    for (double v : wav.samples) {
        writeLe16(out, u16leForWrite(truncClipPcm16(v)));
    }
    out.flush();
    if (!out) throw std::runtime_error("failed writing WAV: " + path.string());
    out.close();
    if (!out) throw std::runtime_error("failed closing WAV: " + path.string());
}

Matrix computeCsluDeltaOrder2(const Matrix& m) {
    Matrix d;
    d.rows = m.rows;
    d.cols = m.cols;
    d.data.assign(d.rows * d.cols, 0.0);
    if (m.rows == 0 || m.cols == 0) return d;
    auto sample = [&](long r, std::size_t c) -> double {
        if (r < 0) r = 0;
        if (static_cast<std::size_t>(r) >= m.rows) r = static_cast<long>(m.rows - 1U);
        return m(static_cast<std::size_t>(r), c);
    };
    for (std::size_t r = 0; r < m.rows; ++r) {
        if ((r & 63) == 0) checkWork();
        for (std::size_t c = 0; c < m.cols; ++c) {
            const long rr = static_cast<long>(r);
            d(r, c) = (sample(rr + 1, c) - sample(rr - 1, c) +
                       2.0 * (sample(rr + 2, c) - sample(rr - 2, c))) / 10.0;
        }
    }
    return d;
}

Matrix computeApproxFeatureInit16k(const std::vector<double>& samples,
                                   double frameMs,
                                   double winMs,
                                   int filters,
                                   int outputs) {
    if (frameMs <= 0.0 || winMs <= 0.0 || filters < 3 || outputs < 1) {
        throw std::runtime_error("invalid PLP arguments");
    }
    // Recovered from feature.dll's forced-alignment path:
    //   DOPREEMPHASIS -> DOHAMMING -> DOPOWERSPEC -> DOFBANKBARK ->
    //   DOLOGSPEC -> DOINVDCT -> DOGEXP.
    // The Bark warp is 6*asinh(f/600). The default Bark filterbank is
    // truncated to [center-2.5, center+1.3] Bark. Inside that support the
    // weights use a flat 1-Bark top, a left 10^(d+0.5) slope, and a right
    // 10^(-2.5*(d-0.5)) slope. DOGEXP is cepstral liftering: c[k] *= k^0.6
    // for k >= 1; it is not signed power compression.
    constexpr int sampleRate = 16000;
    constexpr double preemphasis = 0.97;
    constexpr double gexp = 0.6;
    constexpr double pi = 3.141592653589793238462643383279502884;
    const int hop = static_cast<int>(sampleRate * frameMs * 0.001);
    const int win = static_cast<int>(sampleRate * winMs * 0.001);
    if (hop <= 0 || win <= 1) throw std::runtime_error("invalid PLP geometry");
    const std::size_t nfft = nextPow2(static_cast<std::size_t>(win));
    if (samples.size() < static_cast<std::size_t>(win)) {
        return Matrix{0, static_cast<std::size_t>(outputs), {}};
    }
    const std::size_t rows = 1U + (samples.size() - static_cast<std::size_t>(win)) / static_cast<std::size_t>(hop);

    std::vector<double> hamming(static_cast<std::size_t>(win));
    for (int i = 0; i < win; ++i) {
        if ((i & 63) == 0) checkWork();
        hamming[static_cast<std::size_t>(i)] = 0.54 - 0.46 * std::cos(2.0 * pi * static_cast<double>(i) / static_cast<double>(win - 1));
    }

    const int bins = static_cast<int>(nfft / 2U) + 1;
    const double maxBark = hzToCsluBark(static_cast<double>(sampleRate) / 2.0);
    const double barkStep = maxBark / static_cast<double>(filters - 1);
    std::vector<double> centersBark(static_cast<std::size_t>(filters));
    std::vector<double> centersHz(static_cast<std::size_t>(filters));
    for (int i = 0; i < filters; ++i) {
        if ((i & 63) == 0) checkWork();
        centersBark[static_cast<std::size_t>(i)] = static_cast<double>(i) * barkStep;
        centersHz[static_cast<std::size_t>(i)] = barkToHz(centersBark[static_cast<std::size_t>(i)]);
    }

    int firstGood = 0;
    while (firstGood < filters && std::ceil(centersHz[static_cast<std::size_t>(firstGood)]) <= 0.0) {
        ++firstGood;
    }
    int lastFilt = filters - 1;
    while (lastFilt > firstGood && std::ceil(centersHz[static_cast<std::size_t>(lastFilt)]) > static_cast<double>(sampleRate) / 2.0) {
        --lastFilt;
    }
    if (lastFilt <= firstGood) {
        throw std::runtime_error("invalid recovered Bark filter range");
    }
    const int activeFilters = lastFilt - firstGood; // feature.dll uses [firstGood,lastFilt)

    std::vector<double> binBark(static_cast<std::size_t>(bins));
    for (int b = 0; b < bins; ++b) {
        const double hz = static_cast<double>(b) * static_cast<double>(sampleRate) / static_cast<double>(nfft);
        binBark[static_cast<std::size_t>(b)] = hzToCsluBark(hz);
    }

    struct BarkFilter {
        int lowBin = 0;
        int highBin = 0;
        std::vector<double> weights;
    };
    std::vector<BarkFilter> fb(static_cast<std::size_t>(activeFilters));
    for (int f = 0; f < activeFilters; ++f) {
        const int centerIndex = firstGood + f;
        const double center = centersBark[static_cast<std::size_t>(centerIndex)];

        // feature.dll's default Bark filterbank does not sum every FFT bin.
        // MFCCInitFBank truncates each filter to the finite Bark support
        // [center-2.5, center+1.3], then applies the asymmetric auditory
        // weighting shape inside that support. The constants -2.5 and +1.3
        // are present in feature.dll's fbank initialization path.
        int lowBin = static_cast<int>(std::floor(barkToHz(center - 2.5) *
                                                 static_cast<double>(nfft) /
                                                 static_cast<double>(sampleRate)));
        int highBin = static_cast<int>(std::floor(barkToHz(center + 1.3) *
                                                  static_cast<double>(nfft) /
                                                  static_cast<double>(sampleRate)));
        if (lowBin < 0) lowBin = 0;
        if (highBin < 0) highBin = 0;
        if (lowBin >= bins) lowBin = bins - 1;
        if (highBin >= bins) highBin = bins - 1;
        if (highBin < lowBin) highBin = lowBin;

        BarkFilter bf;
        bf.lowBin = lowBin;
        bf.highBin = highBin;
        bf.weights.assign(static_cast<std::size_t>(highBin - lowBin + 1), 0.0);
        for (int b = lowBin; b <= highBin; ++b) {
            const double d = binBark[static_cast<std::size_t>(b)] - center;
            double exponent = 0.0;
            if (d < -0.5) {
                exponent = d + 0.5;
            } else if (d > 0.5) {
                exponent = -2.5 * (d - 0.5);
            }
            bf.weights[static_cast<std::size_t>(b - lowBin)] = std::pow(10.0, exponent);
        }
        fb[static_cast<std::size_t>(f)] = std::move(bf);
    }

    std::vector<std::vector<double>> cost(static_cast<std::size_t>(outputs),
                                          std::vector<double>(static_cast<std::size_t>(activeFilters), 0.0));
    for (int k = 0; k < outputs; ++k) {
        for (int j = 0; j < activeFilters; ++j) {
            cost[static_cast<std::size_t>(k)][static_cast<std::size_t>(j)] =
                std::cos((static_cast<double>(j + 1) - 0.5) * static_cast<double>(k) * pi /
                         static_cast<double>(activeFilters));
        }
    }

    Matrix out;
    out.rows = rows;
    out.cols = static_cast<std::size_t>(outputs);
    out.data.assign(out.rows * out.cols, 0.0);
    std::vector<std::complex<double>> buf(nfft);
    std::vector<double> powSpec(static_cast<std::size_t>(bins));
    std::vector<double> logFb(static_cast<std::size_t>(activeFilters));

    double previousFrameLastSample = 0.0; // feature.dll's feature_t::x_1 state
    for (std::size_t r = 0; r < rows; ++r) {
        if ((r & 63) == 0) checkWork();
        std::fill(buf.begin(), buf.end(), std::complex<double>(0.0, 0.0));
        const std::size_t base = r * static_cast<std::size_t>(hop);
        std::vector<double> frame(static_cast<std::size_t>(win));
        for (int i = 0; i < win; ++i) {
            if ((i & 63) == 0) checkWork();
            frame[static_cast<std::size_t>(i)] = samples[base + static_cast<std::size_t>(i)];
        }
        const double lastOriginalSample = frame.back();
        for (int i = win - 1; i >= 1; --i) {
            if ((i & 63) == 0) checkWork();
            frame[static_cast<std::size_t>(i)] -= preemphasis * frame[static_cast<std::size_t>(i - 1)];
        }
        frame[0] -= preemphasis * previousFrameLastSample;
        previousFrameLastSample = lastOriginalSample;

        for (int i = 0; i < win; ++i) {
            if ((i & 63) == 0) checkWork();
            buf[static_cast<std::size_t>(i)] = frame[static_cast<std::size_t>(i)] * hamming[static_cast<std::size_t>(i)];
        }
        fftRadix2(buf);
        for (int b = 0; b < bins; ++b) {
            powSpec[static_cast<std::size_t>(b)] = std::norm(buf[static_cast<std::size_t>(b)]);
        }
        for (int f = 0; f < activeFilters; ++f) {
            const BarkFilter& bf = fb[static_cast<std::size_t>(f)];
            double e = 0.0;
            for (int b = bf.lowBin; b <= bf.highBin; ++b) {
                e += bf.weights[static_cast<std::size_t>(b - bf.lowBin)] *
                     powSpec[static_cast<std::size_t>(b)];
            }
            logFb[static_cast<std::size_t>(f)] = (e < 1.0) ? 0.0 : std::log(e);
        }
        for (int k = 0; k < outputs; ++k) {
            double s = 0.0;
            for (int j = 0; j < activeFilters; ++j) {
                s += cost[static_cast<std::size_t>(k)][static_cast<std::size_t>(j)] * logFb[static_cast<std::size_t>(j)];
            }
            s /= static_cast<double>(activeFilters);
            if (k >= 1) {
                s *= std::pow(static_cast<double>(k), gexp);
            }
            out(r, static_cast<std::size_t>(k)) = s;
        }
    }
    return out;
}

PlpApproxFeatures postprocessPlpFromInitAndEnergy(const Matrix& initFeat,
                                                  const Matrix& energyNormIn,
                                                  const Matrix& energyBigwin,
                                                  std::size_t outputRows) {
    if (initFeat.cols != 13) {
        throw std::runtime_error("PLP postprocess expects 13-column init_feat");
    }
    if (energyNormIn.cols != 1 || energyBigwin.cols != 1) {
        throw std::runtime_error("PLP postprocess expects one-column energy matrices");
    }
    PlpApproxFeatures out;
    out.initFeat = initFeat;
    const std::size_t padFrames = 2; // round(((24-5)/2)/5) from features_new_16k.tcl
    out.featMfccPadded = padFeatureLikeTclForPlp(out.initFeat, padFrames, outputRows);
    Matrix energyNorm = resizeEnergyLikeTcl(energyNormIn, out.featMfccPadded.rows);

    std::size_t minIdx = 0;
    double minEnergy = std::numeric_limits<double>::infinity();
    for (std::size_t r = 0; r < energyBigwin.rows; ++r) {
        if ((r & 63) == 0) checkWork();
        const double v = energyBigwin(r, 0);
        if (v < minEnergy) { minEnergy = v; minIdx = r; }
    }
    out.minEnergyFrame = minIdx;
    int bframe = static_cast<int>(minIdx) - 10;
    int eframe = static_cast<int>(minIdx) + 10;
    if (bframe < 0) { eframe -= bframe; bframe = 0; }
    if (eframe >= static_cast<int>(out.featMfccPadded.rows)) {
        const int diff = eframe - static_cast<int>(out.featMfccPadded.rows) + 1;
        eframe -= diff;
        bframe -= diff;
    }
    if (bframe < 0) bframe = 0;
    if (eframe < bframe) eframe = bframe;
    out.cmsBegin = static_cast<std::size_t>(bframe);
    out.cmsEnd = static_cast<std::size_t>(eframe);

    std::vector<double> avg(out.featMfccPadded.cols, 0.0);
    const double count = static_cast<double>(eframe - bframe + 1);
    for (int r = bframe; r <= eframe; ++r) {
        if ((r & 63) == 0) checkWork();
        for (std::size_t c = 0; c < out.featMfccPadded.cols; ++c) {
            avg[c] += out.featMfccPadded(static_cast<std::size_t>(r), c);
        }
    }
    for (double& v : avg) v /= count;

    out.featCms.rows = out.featMfccPadded.rows;
    out.featCms.cols = out.featMfccPadded.cols;
    out.featCms.data.assign(out.featCms.rows * out.featCms.cols, 0.0);
    out.featNoDelta.rows = out.featMfccPadded.rows;
    out.featNoDelta.cols = out.featMfccPadded.cols;
    out.featNoDelta.data.assign(out.featNoDelta.rows * out.featNoDelta.cols, 0.0);
    for (std::size_t r = 0; r < out.featMfccPadded.rows; ++r) {
        if ((r & 63) == 0) checkWork();
        for (std::size_t c = 0; c < out.featMfccPadded.cols; ++c) {
            out.featCms(r, c) = out.featMfccPadded(r, c) - avg[c];
        }
        out.featNoDelta(r, 0) = energyNorm(r, 0);
        for (std::size_t c = 1; c < out.featMfccPadded.cols; ++c) {
            out.featNoDelta(r, c) = out.featCms(r, c);
        }
    }
    out.featDelta = computeCsluDeltaOrder2(out.featNoDelta);
    out.featWithDelta.rows = out.featNoDelta.rows;
    out.featWithDelta.cols = out.featNoDelta.cols + out.featDelta.cols;
    out.featWithDelta.data.assign(out.featWithDelta.rows * out.featWithDelta.cols, 0.0);
    for (std::size_t r = 0; r < out.featWithDelta.rows; ++r) {
        if ((r & 63) == 0) checkWork();
        for (std::size_t c = 0; c < out.featNoDelta.cols; ++c) {
            out.featWithDelta(r, c) = out.featNoDelta(r, c);
        }
        for (std::size_t c = 0; c < out.featDelta.cols; ++c) {
            out.featWithDelta(r, out.featNoDelta.cols + c) = out.featDelta(r, c);
        }
    }
    return out;
}

PlpApproxFeatures computePlpApprox16k(const Pcm16Mono& wav16k, std::size_t outputRows) {
    if (wav16k.sampleRate != 16000) {
        throw std::runtime_error("computePlpApprox16k expects a 16 kHz post-DC WAV");
    }
    PlpApproxFeatures out;
    out.initFeat = computeApproxFeatureInit16k(wav16k.samples, 5.0, 24.0, 40, 13);
    const std::size_t padFrames = 2; // round(((24-5)/2)/5) from features_new_16k.tcl
    out.featMfccPadded = padFeatureLikeTclForPlp(out.initFeat, padFrames, outputRows);

    const ValidatedEnergyFeatures energy = computeValidatedEnergyFeatures16k(wav16k);
    Matrix energyNorm = resizeEnergyLikeTcl(energy.specEnergyNorm24, out.featMfccPadded.rows);

    std::size_t minIdx = 0;
    double minEnergy = std::numeric_limits<double>::infinity();
    for (std::size_t r = 0; r < energy.specEnergyBigwin100.rows; ++r) {
        if ((r & 63) == 0) checkWork();
        const double v = energy.specEnergyBigwin100(r, 0);
        if (v < minEnergy) { minEnergy = v; minIdx = r; }
    }
    out.minEnergyFrame = minIdx;
    int bframe = static_cast<int>(minIdx) - 10;
    int eframe = static_cast<int>(minIdx) + 10;
    if (bframe < 0) { eframe -= bframe; bframe = 0; }
    if (eframe >= static_cast<int>(out.featMfccPadded.rows)) {
        const int diff = eframe - static_cast<int>(out.featMfccPadded.rows) + 1;
        eframe -= diff;
        bframe -= diff;
    }
    if (bframe < 0) bframe = 0;
    if (eframe < bframe) eframe = bframe;
    out.cmsBegin = static_cast<std::size_t>(bframe);
    out.cmsEnd = static_cast<std::size_t>(eframe);

    std::vector<double> avg(out.featMfccPadded.cols, 0.0);
    const double count = static_cast<double>(eframe - bframe + 1);
    for (int r = bframe; r <= eframe; ++r) {
        if ((r & 63) == 0) checkWork();
        for (std::size_t c = 0; c < out.featMfccPadded.cols; ++c) {
            avg[c] += out.featMfccPadded(static_cast<std::size_t>(r), c);
        }
    }
    for (double& v : avg) v /= count;

    out.featCms.rows = out.featMfccPadded.rows;
    out.featCms.cols = out.featMfccPadded.cols;
    out.featCms.data.assign(out.featCms.rows * out.featCms.cols, 0.0);
    out.featNoDelta.rows = out.featMfccPadded.rows;
    out.featNoDelta.cols = out.featMfccPadded.cols;
    out.featNoDelta.data.assign(out.featNoDelta.rows * out.featNoDelta.cols, 0.0);
    for (std::size_t r = 0; r < out.featMfccPadded.rows; ++r) {
        if ((r & 63) == 0) checkWork();
        for (std::size_t c = 0; c < out.featMfccPadded.cols; ++c) {
            out.featCms(r, c) = out.featMfccPadded(r, c) - avg[c];
        }
        out.featNoDelta(r, 0) = energyNorm(r, 0);
        for (std::size_t c = 1; c < out.featMfccPadded.cols; ++c) {
            out.featNoDelta(r, c) = out.featCms(r, c);
        }
    }
    out.featDelta = computeCsluDeltaOrder2(out.featNoDelta);
    out.featWithDelta.rows = out.featNoDelta.rows;
    out.featWithDelta.cols = out.featNoDelta.cols + out.featDelta.cols;
    out.featWithDelta.data.assign(out.featWithDelta.rows * out.featWithDelta.cols, 0.0);
    for (std::size_t r = 0; r < out.featWithDelta.rows; ++r) {
        if ((r & 63) == 0) checkWork();
        for (std::size_t c = 0; c < out.featNoDelta.cols; ++c) {
            out.featWithDelta(r, c) = out.featNoDelta(r, c);
        }
        for (std::size_t c = 0; c < out.featDelta.cols; ++c) {
            out.featWithDelta(r, out.featNoDelta.cols + c) = out.featDelta(r, c);
        }
    }
    return out;
}


namespace {

int truncToInt(double x) {
    return static_cast<int>(x);
}

std::int16_t floorToPcm16(double x) {
    const double f = std::floor(x);
    if (f < -32768.0) return static_cast<std::int16_t>(-32768);
    if (f > 32767.0) return static_cast<std::int16_t>(32767);
    return static_cast<std::int16_t>(static_cast<int>(f));
}

double waveDllSampleKernel(double x, double cutoff, double gaussianWidth) {
    const double arg = x * cutoff * (2.0 * kPi);
    const double sinc = (std::abs(arg) < 1e-50) ? 1.0 : (std::sin(arg) / arg);
    const double windowArg = 2.0 * x * gaussianWidth;
    const double window = std::exp(-kPi * windowArg * windowArg);
    return 0.8 * (2.0 * cutoff) * sinc * window;
}

struct WaveDllResampleState {
    int inputOffset = 0;
    int base = 0;
    int phase = 0;
    int outputIndex = 0;
};

int waveDllResampleBlock(const std::vector<float>& input,
                         int inputCount,
                         std::vector<float>& output,
                         int maxOutput,
                         const std::vector<float>& coeff,
                         int coeffCount,
                         int outputFactor,
                         int inputFactor,
                         WaveDllResampleState& state) {
    while (true) {
        state.base = (state.phase * inputFactor) / outputFactor;
        const int sourceStart = state.inputOffset + state.base;
        if (sourceStart + coeffCount > inputCount) {
            state.inputOffset += coeffCount - inputCount - 1;
            return state.outputIndex;
        }

        const float* x = input.data() + sourceStart;
        const float* h = coeff.data() + static_cast<std::size_t>(state.phase) * static_cast<std::size_t>(coeffCount);
        long double sum = 0.0L;
        for (int i = 0; i < coeffCount; ++i) {
            if ((i & 63) == 0) checkWork();
            sum += static_cast<long double>(x[i]) * static_cast<long double>(h[i]);
        }
        output[static_cast<std::size_t>(state.outputIndex)] = static_cast<float>(sum);

        ++state.phase;
        state.phase %= outputFactor;
        if (state.phase == 0) {
            state.inputOffset += inputFactor;
        }

        ++state.outputIndex;
        state.outputIndex %= maxOutput;
        if (state.outputIndex == 0) {
            return maxOutput;
        }
    }
}

} // namespace

Pcm16Mono sampleConvertWaveDllCompatible(const Pcm16Mono& wav, std::uint32_t targetRate) {
    if (wav.sampleRate == 0 || targetRate == 0) {
        throw std::runtime_error("sampleConvertWaveDllCompatible requires positive sample rates");
    }
    if (wav.sampleRate == targetRate) {
        return wav;
    }
    if (wav.sampleRate > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        targetRate > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("sampleConvertWaveDllCompatible rate exceeds 32-bit CSLU range");
    }

    const int inputRate = static_cast<int>(wav.sampleRate);
    const int outputRate = static_cast<int>(targetRate);
    const int g = std::gcd(inputRate, outputRate);
    if (g <= 0) {
        throw std::runtime_error("sampleConvertWaveDllCompatible failed to compute rate GCD");
    }
    const int inputFactor = inputRate / g;
    const int outputFactor = outputRate / g;
    if (inputFactor < 1 || outputFactor < 1 || inputFactor > 1024 || outputFactor > 1024) {
        throw std::runtime_error("wave.dll sampleconvert supports only reduced rate factors in [1,1024]");
    }

    int coeffCount = 0;
    double cutoff = 0.461;
    double gaussianWidth = 0.0116;
    if (outputFactor > inputFactor) {
        coeffCount = truncToInt(static_cast<double>(outputFactor) * 162.0 / static_cast<double>(inputFactor));
    } else {
        const double ratio = static_cast<double>(outputFactor) / static_cast<double>(inputFactor);
        cutoff = ratio * 0.461;
        gaussianWidth = ratio * 0.0116;
        coeffCount = truncToInt(static_cast<double>(inputFactor) * 162.0 / static_cast<double>(outputFactor));
    }
    if (coeffCount < 1 || coeffCount > 1024) {
        throw std::runtime_error("wave.dll sampleconvert derived an unsupported FIR coefficient count");
    }

    std::vector<float> coeff(static_cast<std::size_t>(outputFactor) * static_cast<std::size_t>(coeffCount), 0.0f);
    for (int coeffIndex = 0; coeffIndex < coeffCount; ++coeffIndex) {
        for (int phase = 0; phase < outputFactor; ++phase) {
            const double frac = std::fmod(static_cast<double>(phase * inputFactor) / static_cast<double>(outputFactor), 1.0);
            const double x = frac + 0.5 * static_cast<double>(coeffCount - 1) - static_cast<double>(coeffIndex);
            // The DLL stores the coefficient table as float, phase-major after the
            // pointer arithmetic in Wave_sampleConvert's construction loop.
            coeff[static_cast<std::size_t>(phase) * static_cast<std::size_t>(coeffCount) + static_cast<std::size_t>(coeffIndex)] =
                static_cast<float>(waveDllSampleKernel(x, cutoff, gaussianWidth));
        }
    }

    constexpr int kInputBufferFloats = 2048;
    constexpr int kOutputChunk = 1024;
    const int overlap = coeffCount - 1;
    if (overlap >= kInputBufferFloats) {
        throw std::runtime_error("wave.dll sampleconvert overlap exceeds input buffer");
    }

    std::vector<float> inputBuffer(kInputBufferFloats, 0.0f);
    std::vector<float> outputBuffer(kOutputChunk, 0.0f);
    std::vector<std::int16_t> rawOutput;
    if (!wav.samples.empty()) {
        const double capacityEstimate = std::ceil(static_cast<double>(wav.samples.size()) *
                                                  static_cast<double>(targetRate) * 1.1 /
                                                  static_cast<double>(wav.sampleRate)) + 2000.0;
        if (capacityEstimate > 0.0 && capacityEstimate < static_cast<double>(std::numeric_limits<std::size_t>::max())) {
            rawOutput.reserve(static_cast<std::size_t>(capacityEstimate));
        }
    }

    WaveDllResampleState state;
    std::size_t inputIndex = 0;
    while (inputIndex < wav.samples.size()) {
        const int maxCopy = kInputBufferFloats - overlap;
        const int remaining = static_cast<int>(std::min<std::size_t>(wav.samples.size() - inputIndex,
                                                                     static_cast<std::size_t>(maxCopy)));
        for (int i = 0; i < remaining; ++i) {
            if ((i & 63) == 0) checkWork();
            inputBuffer[static_cast<std::size_t>(overlap + i)] = static_cast<float>(wav.samples[inputIndex + static_cast<std::size_t>(i)]);
        }
        inputIndex += static_cast<std::size_t>(remaining);
        const int inputCount = overlap + remaining;

        while (true) {
            const int produced = waveDllResampleBlock(inputBuffer, inputCount, outputBuffer, kOutputChunk,
                                                      coeff, coeffCount, outputFactor, inputFactor, state);
            if (produced == kOutputChunk) {
                for (int i = 0; i < kOutputChunk; ++i) {
                    rawOutput.push_back(floorToPcm16(static_cast<double>(outputBuffer[static_cast<std::size_t>(i)])));
                }
            } else {
                break;
            }
        }

        std::memmove(inputBuffer.data(),
                     inputBuffer.data() + remaining,
                     static_cast<std::size_t>(overlap) * sizeof(float));
    }

    // Flush with a zero block exactly like wave.dll's trailing path.
    std::fill(inputBuffer.begin() + overlap,
              inputBuffer.begin() + std::min<int>(kInputBufferFloats, overlap + overlap),
              0.0f);
    const int flushInputCount = overlap + overlap;
    while (true) {
        const int produced = waveDllResampleBlock(inputBuffer, flushInputCount, outputBuffer, kOutputChunk,
                                                  coeff, coeffCount, outputFactor, inputFactor, state);
        if (produced > 0) {
            for (int i = 0; i < produced; ++i) {
                rawOutput.push_back(floorToPcm16(static_cast<double>(outputBuffer[static_cast<std::size_t>(i)])));
            }
        }
        if (produced != kOutputChunk) {
            break;
        }
    }

    const int shift = static_cast<int>(std::floor((static_cast<double>(coeffCount - 1) *
                                                  static_cast<double>(outputFactor) * 0.5 /
                                                  static_cast<double>(inputFactor)) + 0.5));
    const int trim = coeffCount / 4;
    const std::size_t finalLen = (rawOutput.size() > static_cast<std::size_t>(trim))
        ? (rawOutput.size() - static_cast<std::size_t>(trim))
        : 0U;

    Pcm16Mono out;
    out.sampleRate = targetRate;
    out.samples.assign(finalLen, 0.0);
    for (std::size_t i = 0; i < finalLen; ++i) {
        if ((i & 63) == 0) checkWork();
        const std::size_t src = static_cast<std::size_t>(shift) + i;
        const std::int16_t v = (src < rawOutput.size()) ? rawOutput[src] : 0;
        out.samples[i] = static_cast<double>(v);
    }
    return out;
}

Pcm16Mono resamplePcm16MonoLinear(const Pcm16Mono& wav, std::uint32_t targetRate) {
    return sampleConvertWaveDllCompatible(wav, targetRate);
}

FullFrontendFeatures computeFullFrontendFromPreDcWavs(const Pcm16Mono& wave16PreDc,
                                                      const Pcm16Mono& wave8PreDc) {
    if (wave16PreDc.sampleRate != 16000) {
        throw std::runtime_error("computeFullFrontendFromPreDcWavs expects the first WAV to be 16 kHz pre-DC audio");
    }
    if (wave8PreDc.sampleRate != 8000) {
        throw std::runtime_error("computeFullFrontendFromPreDcWavs expects the second WAV to be 8 kHz pre-DC audio");
    }

    FullFrontendFeatures f;
    f.wave16BeforeDc = wave16PreDc;
    f.wave8BeforeDc = wave8PreDc;
    f.wave16AfterDc = applyPrepDcrmIir(f.wave16BeforeDc);
    f.wave8AfterDc = applyPrepDcrmIir(f.wave8BeforeDc);

    // Use the PLP row count as the common alignment length. This mirrors the Tcl
    // min-row join in compute_all_feat, while keeping all branches explicitly
    // derived from audio rather than from previously dumped matrices.
    f.plp = computePlpApprox16k(f.wave16AfterDc);
    f.plpFeat16k = f.plp.featWithDelta;
    const std::size_t rows = f.plpFeat16k.rows;

    f.burstMx = computeBurstMx8k(f.wave8AfterDc.samples);
    f.burstPulsesCol7 = computeBurstPulsesCol7(f.burstMx);
    f.intensity0To15 = computeIntensityDiscrimination16k(f.wave16AfterDc.samples, rows);
    f.intensityEnergyCol0 = computeIntensityEnergyCol0(f.wave16AfterDc.samples, 16000, 5.0, 40.0, rows, 10);
    f.allFeat = composeAllFeat(f.plpFeat16k, f.burstPulsesCol7, f.intensity0To15, f.intensityEnergyCol0);
    f.netin = composeNetInFromAllFeat(f.allFeat);
    return f;
}

FullFrontendFeatures computeFullFrontendFromOriginalWav(const Pcm16Mono& original) {
    Pcm16Mono wave16 = sampleConvertWaveDllCompatible(original, 16000);
    Pcm16Mono wave8 = sampleConvertWaveDllCompatible(original, 8000);
    return computeFullFrontendFromPreDcWavs(wave16, wave8);
}

void writeFullFrontendFeatures(const FullFrontendFeatures& f,
                               const std::filesystem::path& outDir) {
    std::filesystem::create_directories(outDir);
    writePcm16Mono(f.wave16BeforeDc, outDir / "audio" / "wave_16k_before_dc.wav");
    writePcm16Mono(f.wave8BeforeDc, outDir / "audio" / "wave_8k_before_dc.wav");
    writePcm16Mono(f.wave16AfterDc, outDir / "audio" / "wave_16k_after_dc.wav");
    writePcm16Mono(f.wave8AfterDc, outDir / "audio" / "wave_8k_after_dc.wav");

    const std::filesystem::path spec = outDir / "features" / "spec_16000_24.0";
    writeNumericTsv(f.plp.initFeat, spec / "init_feat.tsv");
    writeNumericTsv(f.plp.featMfccPadded, spec / "feat_mfcc_padded.tsv");
    writeNumericTsv(f.plp.featCms, spec / "feat_cms.tsv");
    writeNumericTsv(f.plp.featNoDelta, spec / "feat_nodelta.tsv");
    writeNumericTsv(f.plp.featDelta, spec / "feat_delta.tsv");
    writeNumericTsv(f.plp.featWithDelta, spec / "feat_with_delta.tsv");

    writeNumericTsv(f.plpFeat16k, outDir / "features" / "plp_feat_16k.tsv");
    writeNumericTsv(f.burstMx, outDir / "features" / "burst_mx.tsv");
    writeNumericTsv(f.burstPulsesCol7, outDir / "features" / "burst_pulses_col7.tsv");
    writeNumericTsv(f.intensity0To15, outDir / "features" / "intensity_0_15.tsv");
    writeNumericTsv(f.intensityEnergyCol0, outDir / "features" / "intensity_energy_col0.tsv");
    writeNumericTsv(f.allFeat, outDir / "features" / "all_feat.tsv");
    for (const char* name : {"manner", "place", "height", "trans"}) {
        writeNumericTsv(f.netin, outDir / "netin" / (std::string(name) + ".tsv"));
    }

    std::ofstream meta(outDir / "frontend_summary.tsv");
    if (!meta) throw std::runtime_error("cannot write frontend_summary.tsv");
    meta << "wave16_before_samples\t" << f.wave16BeforeDc.samples.size() << "\n";
    meta << "wave8_before_samples\t" << f.wave8BeforeDc.samples.size() << "\n";
    meta << "wave16_after_samples\t" << f.wave16AfterDc.samples.size() << "\n";
    meta << "wave8_after_samples\t" << f.wave8AfterDc.samples.size() << "\n";
    meta << "plp_rows\t" << f.plpFeat16k.rows << "\n";
    meta << "burst_rows\t" << f.burstPulsesCol7.rows << "\n";
    meta << "intensity_rows\t" << f.intensity0To15.rows << "\n";
    meta << "energy_rows\t" << f.intensityEnergyCol0.rows << "\n";
    meta << "all_feat_rows\t" << f.allFeat.rows << "\n";
    meta << "all_feat_cols\t" << f.allFeat.cols << "\n";
    meta << "netin_rows\t" << f.netin.rows << "\n";
    meta << "netin_cols\t" << f.netin.cols << "\n";
    meta << "plp_cms_begin\t" << f.plp.cmsBegin << "\n";
    meta << "plp_cms_end\t" << f.plp.cmsEnd << "\n";
    meta << "plp_min_energy_frame\t" << f.plp.minEnergyFrame << "\n";
}

} // namespace cslu
