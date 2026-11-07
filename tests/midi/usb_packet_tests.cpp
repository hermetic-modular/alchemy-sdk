#include "alchemy/midi/usb_packets.h"
#include "alchemy/midi/wire.h"
#include <cassert>
#include <vector>
using namespace alchemy::midi;
int main() {
    for(uint8_t cable=0;cable<16;++cable)for(unsigned status=0x80;status<=0xff;++status){
        ShortMessage message;message.bytes={uint8_t(status),60,100};
        message.size=status<0xf0?((status&0xf0)==0xc0||(status&0xf0)==0xd0?2:3):status==0xf2?3:status==0xf1||status==0xf3?2:1;
        const auto valid=Validate(message);UsbPacket packet;
        assert(EncodeUsb(message,packet,cable)==valid);
        if(valid==TxResult::Accepted){uint8_t actual;const uint8_t* bytes;size_t count;assert(DecodeUsb(packet,actual,bytes,count));assert(actual==cable && count==message.size);for(size_t i=0;i<count;++i)assert(bytes[i]==message.bytes[i]);if(status>=0xf8)assert((packet.bytes[0]&15)==15);}
    }
    for(unsigned length=0;length<300;++length){
        UsbSysExEncoder encoder(3);std::vector<UsbPacket> packets;
        auto sink=[&](const UsbPacket& p){packets.push_back(p);return TxResult::Accepted;};
        assert(encoder.Feed(0xf0,sink)==TxResult::Accepted);
        for(unsigned i=0;i<length;++i){if(i%7==0)assert(encoder.Feed(0xf8,sink)==TxResult::Accepted);assert(encoder.Feed(uint8_t(i%128),sink)==TxResult::Accepted);}
        const size_t full=packets.size();
        assert(encoder.Feed(0xf7,[](const UsbPacket&){return TxResult::Busy;})==TxResult::Busy && encoder.Active() && packets.size()==full);
        assert(encoder.Feed(0xf7,sink)==TxResult::Accepted && !encoder.Active());
        StreamDecoder decoder;assert(decoder.Init({0,1}));std::vector<uint8_t> data;unsigned clocks=0,begins=0,ends=0;
        decoder.SetSysExSink({&data,[](void* p,const SysExChunk& c){if(c.phase==SysExPhase::Data)static_cast<std::vector<uint8_t>*>(p)->push_back(c.byte);}});
        for(auto& packet:packets){uint8_t cable;const uint8_t* bytes;size_t count;assert(DecodeUsb(packet,cable,bytes,count));assert(cable==3);begins+=bytes[0]==0xf0;ends+=bytes[count-1]==0xf7;for(size_t i=0;i<count;++i){Event event;if(decoder.Feed(bytes[i],0,event)){assert(event.type==EventType::TimingClock);++clocks;}}}
        assert(data.size()==length && begins==1 && ends==1 && clocks==(length+6)/7);
        for(unsigned i=0;i<length;++i)assert(data[i]==i%128);
    }
    UsbPacket invalid{{9,0x80,60,100}};uint8_t cable;const uint8_t* bytes;size_t count;assert(!DecodeUsb(invalid,cable,bytes,count));
    invalid.bytes={4,1,2,0xf7};assert(!DecodeUsb(invalid,cable,bytes,count));
    invalid.bytes={0,0,0,0};assert(!DecodeUsb(invalid,cable,bytes,count));
    UsbSysExEncoder encoder;assert(encoder.Feed(0xf7,[](const UsbPacket&){return TxResult::Accepted;})==TxResult::Invalid);
    UsbPacketDecoder stateful;
    auto parse=[&](std::array<uint8_t,4> value){UsbPacket p{value};return stateful.Decode(p,cable,bytes,count);};
    assert(!parse({4,74,127,0}) && !parse({5,0xf7,0,0}));
    assert(parse({4,0xf0,0x7d,1}));
    assert(parse({15,0xf8,0,0}));
    assert(parse({4,0xf0,1,2}));
    assert(parse({4,2,3,4}) && parse({7,5,6,0xf7}));
    assert(parse({6,0xf0,0xf7,0}) && parse({7,0xf0,1,0xf7}));
    assert(parse({15,0xf0,0,0}) && parse({4,1,2,3}) && parse({15,0xf7,0,0}));
    assert(parse({4,0xf0,1,2}) && parse({9,0x90,60,100}));
    assert(!parse({4,1,2,3}));
    assert(parse({4,0xf0,1,2}));stateful.Reset();assert(!parse({5,0xf7,0,0}));
    assert(parse({0x14,0xf0,1,2}) && !parse({4,1,2,3}) && parse({0x15,0xf7,0,0}));
}
