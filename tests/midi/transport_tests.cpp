#include "alchemy/midi/raw_input.h"
#include "alchemy/midi/output.h"
#include <cassert>
#include <vector>
using namespace alchemy::midi;

struct Sink {
    std::vector<ShortMessage> sent;
    TxResult next = TxResult::Busy;
    static TxResult Submit(void* p, const ShortMessage& m,uint32_t) {
        auto& s = *static_cast<Sink*>(p);
        if (s.next == TxResult::Accepted) s.sent.push_back(m);
        return s.next;
    }
};
int main() {
    RawInput<8> raw;
    const uint8_t bytes[] = {0x90,60,100,0x80,60,0};
    assert(raw.Receive(bytes,6,42));
    assert(raw.Size() == 6);
    for (auto expected : bytes) { StampedByte b; assert(raw.Pop(b)); assert(b.byte==expected && b.tick==42); }
    assert(raw.Receive(bytes,6,43));
    assert(!raw.Receive(bytes,6,44));
    assert(raw.Lost());
    assert(raw.DiscardLost());
    assert(raw.Size()==0 && raw.Lost());
    assert(!raw.Receive(bytes,1,45));
    raw.Restart();
    assert(raw.Receive(bytes,1,46));
    uint32_t time = 0;
    assert(ArrivalTime(0xfffffff0u,{0x10u,100,32000000},time) && time==99);
    assert(ArrivalTime(100,{100,10,240000000},time) && time==10);
    assert(!ArrivalTime(101,{100,10,240000000},time));
    assert(!ArrivalTime(0,{1,0,0},time));
    assert(!ArrivalTime(0,{3000000,3000000,1000000},time));
    ShortMessage m;
    auto note = Event::NoteOn({0,1},15,60,127);
    assert(Encode(note,m)==TxResult::Accepted && m.size==3 && m.bytes[0]==0x9f && m.bytes[2]==127);
    assert(Encode(Event::Bend({0,1},0,8192),m)==TxResult::Accepted && m.bytes[1]==0 && m.bytes[2]==64);
    assert(Encode(Event::System(EventType::SongPosition,{0,1},0,16383),m)==TxResult::Accepted && m.bytes[0]==0xf2 && m.bytes[1]==127 && m.bytes[2]==127);
    assert(Encode(Event::System(EventType::TimingClock,{0,1}),m)==TxResult::Accepted && m.size==1 && m.bytes[0]==0xf8);
    assert(Encode(Event::System(EventType::SysEx,{0,1}),m)==TxResult::Unsupported);
    note.channel=16; assert(Encode(note,m)==TxResult::Invalid);
    note.channel=0; note.value=128; assert(Encode(note,m)==TxResult::Invalid);
    auto program = Event::System(EventType::ProgramChange,{0,1},0,42);
    program.channel=2;
    assert(Encode(program,m)==TxResult::Accepted && m.size==2 && m.bytes[0]==0xc2 && m.bytes[1]==42);
    Sink sink;
    Output<4> out({&sink,Sink::Submit});
    for(unsigned i=0;i<4;++i) assert(out.Send(Event::NoteOn({0,1},0,uint8_t(60+i),100))==TxResult::Accepted);
    assert(out.Send(Event::NoteOff({0,1},0,60))==TxResult::Busy);
    assert(out.Poll()==TxResult::Busy && out.Pending()==4);
    sink.next=TxResult::Error;
    assert(out.Poll()==TxResult::Error && out.Pending()==4);
    sink.next=TxResult::Accepted;
    assert(out.Poll(0,1)==TxResult::Accepted && out.Pending()==3);
    assert(out.Poll()==TxResult::Accepted && out.Pending()==0);
    assert(sink.sent.size()==4 && out.Submitted()==4);
    for(unsigned i=0;i<4;++i) assert(sink.sent[i].bytes[1]==60+i);
    m.bytes = {0x90, 60, 128}; m.size = 3; assert(Validate(m)==TxResult::Invalid);
    m.bytes = {0x90, 60, 0}; m.size = 2; assert(Validate(m)==TxResult::Invalid);
    m.bytes = {0xc0, 60, 0}; m.size = 2; assert(Validate(m)==TxResult::Accepted);
    m.bytes = {0xf9, 0, 0}; m.size = 1; assert(Validate(m)==TxResult::Unsupported);
    m.bytes = {0xf0, 0, 0}; m.size = 3; assert(Validate(m)==TxResult::Unsupported);
    sink.next=TxResult::Disconnected;
    assert(out.Send(Event::NoteOff({0,1},0,60))==TxResult::Accepted);
    assert(out.Poll()==TxResult::Disconnected && out.Pending()==1 && out.Submitted()==4);
    out.Clear(); assert(out.Pending()==0);
    auto hi = Event::NoteOn({0,1},0,60,1); hi.resolution=14; hi.value=16383;
    assert(Encode(hi,m)==TxResult::Unsupported);
    sink.next=TxResult::Accepted;
    Output<2> hires({&sink,Sink::Submit});
    assert(hires.Send(hi)==TxResult::Accepted && hires.Pending()==2);
    assert(hires.Send(hi)==TxResult::Busy);
    const size_t before=sink.sent.size(); hires.Poll();
    assert(sink.sent[before].bytes[1]==88 && sink.sent[before].bytes[2]==127);
    assert(sink.sent[before+1].bytes[0]==0x90 && sink.sent[before+1].bytes[2]==127);
    hi.value=127; assert(hires.Send(hi)==TxResult::Invalid && hires.Pending()==0);
    hi.value=0; assert(hires.Send(hi)==TxResult::Accepted && hires.Pending()==1);
    hires.Poll(); assert(sink.sent.back().bytes[0]==0x90 && sink.sent.back().bytes[2]==0);
    hi.type=EventType::NoteOff; hi.value=63;
    assert(hires.Send(hi)==TxResult::Accepted && hires.Pending()==2); hires.Poll();
    assert(sink.sent.back().bytes[0]==0x80 && sink.sent.back().bytes[2]==0);
    Output<2> missing;
    assert(missing.Poll()==TxResult::Unsupported);
    Output<4> bounded({&sink,Sink::Submit,1});
    assert(bounded.Send(Event::NoteOn({0,1},0,60,100))==TxResult::Accepted);
    assert(bounded.Send(Event::NoteOn({0,1},0,61,100))==TxResult::Accepted);
    assert(bounded.Poll(8)==TxResult::Accepted && bounded.Pending()==1);
    assert(bounded.Poll(8)==TxResult::Accepted && bounded.Pending()==0);
    auto shorthand=Event::NoteOn({0,1},0,60,0);
    assert(Encode(shorthand,m)==TxResult::Accepted && m.bytes[0]==0x90 && m.bytes[2]==0);
    shorthand.value=1;assert(Encode(shorthand,m)==TxResult::Invalid);
}
