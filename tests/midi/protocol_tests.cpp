#include "alchemy/midi/wire.h"
#include "alchemy/midi/output.h"
#include <cassert>
#include <vector>
using namespace alchemy::midi;

struct Decoder {
    StreamDecoder wire;
    std::vector<Event> events;
    std::vector<SysExChunk> chunks;
    uint32_t time=0;
    explicit Decoder(SourceId source={0,1}) {
        assert(wire.Init(source));
        wire.SetSysExSink({&chunks,[](void* p,const SysExChunk& c){static_cast<std::vector<SysExChunk>*>(p)->push_back(c);}});
    }
    void Byte(uint8_t byte) {Event e; if(wire.Feed(byte,++time,e)) {assert(e.Valid());events.push_back(e);}}
    void Bytes(std::initializer_list<uint8_t> bytes) {for(auto b:bytes) Byte(b);}
};
int main() {
    const EventType types[]={EventType::NoteOff,EventType::NoteOn,EventType::PolyPressure,
        EventType::ControlChange,EventType::ProgramChange,EventType::ChannelPressure,EventType::PitchBend};
    for(unsigned ch=0;ch<16;++ch) for(unsigned kind=0;kind<7;++kind) {
        Decoder d({2,5});const auto status=uint8_t(0x80+16*kind+ch);
        const bool one=kind==4||kind==5;
        d.Byte(status);d.Byte(63);if(!one)d.Byte(127);
        d.Byte(64);if(!one)d.Byte(126);
        assert(d.events.size()==2);
        for(unsigned i=0;i<2;++i) {
            auto& e=d.events[i];assert(e.type==types[kind] && e.channel==ch && e.source==SourceId({2,5}));
            assert(e.time_us==(one?2+ i:3+2*i));
            if(one) assert(e.value==63+i);
            else if(kind==6) assert(e.value==(63+i)+(uint16_t(127-i)<<7) && e.resolution==14);
            else assert(e.data1==63+i && e.value==127-i);
        }
    }
    Decoder zero;zero.Bytes({0x90,60,0,0x80,60,0});
    assert(zero.events.size()==2 && zero.events[0].note_off_origin==NoteOffOrigin::ZeroVelocityNoteOn);
    assert(zero.events[1].note_off_origin==NoteOffOrigin::Explicit);
    Decoder original;original.Bytes({0xb0,88,127,0x90,60,0});Decoder forwarded({1,1});
    for(const auto& event:original.events) {
        ShortMessage message;assert(Encode(event,message)==TxResult::Accepted);
        for(unsigned i=0;i<message.size;++i)forwarded.Byte(message.bytes[i]);
    }
    assert(forwarded.events.size()==2 && forwarded.events[0].data1==88 && forwarded.events[0].value==127);
    assert(forwarded.events[1].type==EventType::NoteOff && forwarded.events[1].note_off_origin==NoteOffOrigin::ZeroVelocityNoteOn);
    Decoder mode;mode.Bytes({0xb3,123,0,7,127,120,0,8,64});
    assert(mode.events.size()==4 && mode.events[1].data1==7 && mode.events[3].data1==8);
    Decoder common;common.Bytes({0xf1,127,0x93,60,99,0xf2,1,2,0x93,61,100,0xf3,5,0x93,62,101,0xf6});
    assert(common.events.size()==7);
    assert(common.events[0].type==EventType::TimeCode && common.events[0].value==127);
    assert(common.events[2].type==EventType::SongPosition && common.events[2].value==257);
    assert(common.events[4].type==EventType::SongSelect && common.events[4].value==5);
    assert(common.events[6].type==EventType::TuneRequest);
    for(uint8_t status=0xf0;status<=0xf7;++status) {
        Decoder d;d.Bytes({0x90,60,100});d.Byte(status);
        if(status==0xf0)d.Byte(0xf7);
        else if(status==0xf1||status==0xf3)d.Byte(7);
        else if(status==0xf2)d.Bytes({7,0});
        const auto before=d.events.size();d.Bytes({61,99});assert(d.events.size()==before);
    }
    for(uint8_t rt=0xf8;;++rt) {
        Decoder d;d.Bytes({0x90,60});d.Byte(rt);d.Byte(100);d.Bytes({61,99});
        const bool defined=rt!=0xf9 && rt!=0xfd;
        assert(d.events.size()==(defined?3u:2u));
        assert(d.events[defined?1:0].type==EventType::NoteOn && d.events.back().data1==61);
        if(rt==0xff)break;
    }
    Decoder interrupted;interrupted.Bytes({0x90,60,0xc7,12,13,0xf2,1,0xf8,2,64,99});
    assert(interrupted.events.size()==4 && interrupted.events[0].type==EventType::ProgramChange);
    assert(interrupted.events[2].type==EventType::TimingClock && interrupted.events[3].type==EventType::SongPosition);
    Decoder sx;sx.Byte(0xf0);
    for(unsigned i=0;i<500;++i){if(i==250)sx.Byte(0xf8);sx.Byte(uint8_t(i&127));}
    sx.Byte(0xf7);
    assert(sx.chunks.size()==502 && sx.chunks.front().phase==SysExPhase::Begin && sx.chunks.back().phase==SysExPhase::End);
    assert(sx.events.size()==1 && sx.events[0].type==EventType::TimingClock && !sx.wire.InSysEx());
    for(unsigned i=0;i<500;++i) assert(sx.chunks[i+1].phase==SysExPhase::Data && sx.chunks[i+1].byte==(i&127));
    sx.Bytes({0xf0,1,0x90,60,100,0xf0,2,0xf0,3});
    assert(sx.chunks[504].phase==SysExPhase::Abort && sx.events.back().type==EventType::NoteOn);
    assert(sx.chunks[507].phase==SysExPhase::Abort && sx.wire.InSysEx());
    assert(!sx.wire.Init({1,2}) && !sx.wire.Init({0,1}) && !sx.wire.Init({0,0}));
    assert(sx.wire.Init({0,2}) && sx.chunks.back().phase==SysExPhase::Abort && sx.chunks.back().source.session==1);
    sx.Bytes({0xf0,4,0xf7});assert(sx.chunks.back().source.session==2);
    StreamDecoder detach;assert(detach.Init({4,1}));
    struct Detach {StreamDecoder* decoder;unsigned end=0,abort=0;} detached{&detach};
    const auto detach_sink=SysExSink{&detached,[](void* p,const SysExChunk& c) {
        auto& d=*static_cast<Detach*>(p);
        if(c.phase==SysExPhase::End || c.phase==SysExPhase::Abort) {
            assert(!d.decoder->InSysEx());
            if(c.phase==SysExPhase::End)++d.end;else++d.abort;
            d.decoder->SetSysExSink({});
        }
    }};
    Event ignored;detach.SetSysExSink(detach_sink);detach.Feed(0xf0,0,ignored);detach.Feed(0xf7,1,ignored);
    assert(detached.end==1 && detached.abort==0);
    detach.SetSysExSink(detach_sink);detach.Feed(0xf0,2,ignored);detach.Abort(3);
    assert(detached.end==1 && detached.abort==1);
    Decoder a({0,8}),b({1,3});a.Bytes({0x90,60});b.Bytes({0xd1,64});a.Byte(100);
    assert(a.events.size()==1 && a.events[0].data1==60 && a.events[0].source==SourceId({0,8}));
    assert(b.events.size()==1 && b.events[0].type==EventType::ChannelPressure && b.events[0].source==SourceId({1,3}));
    StreamDecoder fuzz;assert(fuzz.Init({3,1}));
    uint32_t random=0x13579bdf;
    for(uint32_t i=0;i<250000;++i) {
        random=random*1664525u+1013904223u;Event event;
        if(fuzz.Feed(uint8_t(random>>24),i,event)) {
            assert(event.Valid() && event.source==SourceId({3,1}) && event.time_us==i);
            ShortMessage encoded;assert(Encode(event,encoded)==TxResult::Accepted && Validate(encoded)==TxResult::Accepted);
            StreamDecoder roundtrip;assert(roundtrip.Init(event.source));Event parsed;bool produced=false;
            for(unsigned j=0;j<encoded.size;++j) produced=roundtrip.Feed(encoded.bytes[j],i,parsed);
            assert(produced && parsed.Valid() && parsed.type==event.type && parsed.channel==event.channel && parsed.value==event.value && parsed.data1==event.data1);
        }
    }
}
