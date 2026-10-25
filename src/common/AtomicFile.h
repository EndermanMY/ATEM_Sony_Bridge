#pragma once
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <windows.h>

// Replacement is on the same volume. The previous file remains in .bak.
inline void writeAtomicText(const std::string& filename,const std::string& contents) {
    if(filename.empty()) throw std::invalid_argument("Empty configuration path");
    auto destination=std::filesystem::u8path(filename);
    auto temporary=destination;temporary+=L".tmp";
    std::ofstream file(temporary,std::ios::binary|std::ios::trunc);
    file<<contents;file.flush();
    if(!file) throw std::runtime_error("Configuration write failed");
    file.close();
    if(!file) throw std::runtime_error("Configuration close failed");
    if(std::filesystem::exists(destination)) {
        auto backup=destination;backup+=L".bak";
        std::filesystem::copy_file(destination,backup,std::filesystem::copy_options::overwrite_existing);
    }
    if(!MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Configuration replace failed: "+std::to_string(GetLastError()));
}
