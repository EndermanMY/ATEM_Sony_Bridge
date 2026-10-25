// GUID 稳定性测试：CreateCameraObjectInfoEthernetConnection 生成的设备 GUID
// 在多次运行之间是否一致（决定 WiFi 配对能否复用）
#include <cstdio>
#include "common/Validation.h"
#include "CameraRemote_SDK.h"

#pragma comment(lib, "Cr_Core.lib")

int main(int argc,char** argv)
{
    if(argc!=2 || !validIpv4(argv[1])) {printf("Usage: guidtest IPv4\n");return 2;}
    if (!SCRSDK::Init(0)) { printf("Init failed\n"); return 1; }

    SCRSDK::ICrCameraObjectInfo* obj = nullptr;
    CrInt8u mac[6] = { 0 };
    unsigned a,b,c,d;
    if(argc!=2 || sscanf(argv[1],"%u.%u.%u.%u",&a,&b,&c,&d)!=4 || a>255||b>255||c>255||d>255){printf("Usage: guidtest IPv4\n");SCRSDK::Release();return 2;}
    CrInt32u ip=a|(b<<8)|(c<<16)|(d<<24);
    SCRSDK::CrError err = SCRSDK::CreateCameraObjectInfoEthernetConnection(
        &obj, SCRSDK::CrCameraDeviceModel_ILME_FX3, ip, mac, 0);
    if (err != SCRSDK::CrError_None || !obj) { printf("create failed 0x%x\n", err); SCRSDK::Release();return 1; }

    CrChar* g = obj->GetGuid();
    int len = 0;
    if (g) { while (g[len]) ++len; }
    printf("GUID len=%d codes:", len);
    for (int i = 0; i < len && i < 40; ++i) printf(" %04x", (unsigned)(CrInt32u)(CrInt16u)g[i]);
    printf("\n");
    obj->Release();
    SCRSDK::Release();
    return 0;
}
