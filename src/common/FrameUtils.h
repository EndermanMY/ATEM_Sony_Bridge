#pragma once
#include <vector>
#include <cstdint>
#include <algorithm>
inline std::vector<uint8_t> extractJpeg(const std::vector<uint8_t>& buffer) {
    const uint8_t soi[]{0xff,0xd8}, eoi[]{0xff,0xd9};
    auto first=std::search(buffer.begin(),buffer.end(),soi,soi+2);
    if(first==buffer.end()) return {};
    auto last=std::search(first+2,buffer.end(),eoi,eoi+2);
    if(last==buffer.end()) return {};
    return {first,last+2};
}
