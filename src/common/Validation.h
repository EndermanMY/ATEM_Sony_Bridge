#pragma once
#include <string>
#include <cstdint>
#include <vector>
#include <stdexcept>

inline bool validIpv4(const std::string& ip) {
    int parts=0, digits=0, value=0;
    for (char c : ip) {
        if(c=='.') {
            if(!digits || value>255 || ++parts>3) return false;
            value=digits=0;
        } else if(c>='0' && c<='9') {
            if(++digits>3) return false;
            value=value*10+c-'0';
        } else return false;
    }
    return parts==3 && digits && value<=255;
}

template<class Cameras>
inline int allocateCameraId(const Cameras& cameras, const std::string& ip, int requested) {
    if(requested==0) {
        for(const auto& camera:cameras) if(camera.ip==ip && camera.cameraId>0) return camera.cameraId;
        for(int id=1;id<=15;++id) {
            bool used=false;
            for(const auto& camera:cameras) if(camera.cameraId==id) used=true;
            if(!used) return id;
        }
        throw std::invalid_argument("No free Camera ID (1-15)");
    }
    if(requested<1 || requested>15) throw std::invalid_argument("Camera ID range is 1-15");
    for(const auto& camera:cameras)
        if(camera.ip!=ip && camera.cameraId==requested) throw std::invalid_argument("Camera ID already assigned");
    return requested;
}

inline int64_t decodeInteger(const uint8_t* bytes, int width, bool signedValue) {
    if(width!=1 && width!=2 && width!=4 && width!=8) throw std::invalid_argument("Invalid integer width");
    uint64_t value=0;
    for(int i=0;i<width;++i) value|=uint64_t(bytes[i])<<(8*i);
    if(signedValue && width<8 && (value&(uint64_t(1)<<(width*8-1))))
        value|=(~uint64_t(0))<<(width*8);
    return static_cast<int64_t>(value);
}

inline bool fitsInteger(int64_t value,int width,bool signedValue) {
    if(width!=1 && width!=2 && width!=4 && width!=8) return false;
    if(width==8) return signedValue || value>=0;
    int bits=width*8;
    if(signedValue) return value>=-(int64_t(1)<<(bits-1)) && value<(int64_t(1)<<(bits-1));
    return value>=0 && uint64_t(value)<(uint64_t(1)<<bits);
}
