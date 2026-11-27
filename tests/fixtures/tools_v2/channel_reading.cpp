#include <dafeiyu/tool_abi_v2.h>
#include <nlohmann/json.hpp>
#include <cstring>

// 在渠道 DLL 内将其原始磅单或台账记录映射为通用采样，完整保留业务原文。
extern "C" int dfy_channel_decode(dfy_v2_string_view config,char* output,std::size_t capacity) {
    using Json=nlohmann::json;
    try {
        const auto raw=Json::parse(config.data,config.data+config.length);
        Json p{{"format","dfy.observation/1"}};
        if(raw.contains("forward")) p["forward"]=raw.at("forward");
        else {
#ifdef DFY_FREIGHT_FIXTURE
            const char* keys[]={"assetTag","parcel","scale","waybill","handoff","gross","unit"};
#else
            const char* keys[]={"accession","title","instrument","entry","inventoryRound","reading","units"};
#endif
            const char* fields[]={"object","label","observer","sample","checkpoint","value","unit"};
            for(std::size_t i=0;i<7;++i) p[fields[i]]=raw.at(keys[i]);
            p["measure"]=raw.at("quantity"); p["native"]=raw;
        }
        const auto encoded=p.dump(); if(encoded.size()>=capacity) return 0;
        std::memcpy(output,encoded.c_str(),encoded.size()+1); return 1;
    } catch(...) {return 0;}
}
