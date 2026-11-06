#include "alchemy/midi/output.h"
#include <algorithm>
#include <cassert>
#include <thread>
#include <vector>
using namespace alchemy::midi;
struct Capture {
    std::vector<ShortMessage> messages;
    TxResult result = TxResult::Accepted;
    unsigned bundles = 0;
    static TxResult Submit(void* context,const ShortMessage& message,uint32_t) {
        auto& self=*static_cast<Capture*>(context);
        if(self.result==TxResult::Accepted)self.messages.push_back(message);
        return self.result;
    }
    static TxResult Bundle(void* context,const ShortMessage* messages,size_t count,uint32_t) {
        auto& self=*static_cast<Capture*>(context);
        if(self.result==TxResult::Accepted){self.messages.insert(self.messages.end(),messages,messages+count);++self.bundles;}
        return self.result;
    }
};
static void BundleLimitsAndFailures() {
    Capture capture;
    Output<64, 32> bounded({&capture, Capture::Submit, 64, Capture::Bundle, nullptr, 16});
    std::array<Event, 17> chord;
    for (size_t i = 0; i < chord.size(); ++i) chord[i] = Event::NoteOn({0, 1}, 0, uint8_t(60 + i), 100);
    assert(bounded.SendBundle(chord.data(), chord.size()) == TxResult::Invalid && bounded.Pending() == 0);
    for (auto& note : chord) { note.resolution = 14; note.value = 1000; }
    assert(bounded.SendBundle(chord.data(), 9) == TxResult::Invalid && bounded.Pending() == 0);
    assert(bounded.SendBundle(chord.data(), 8) == TxResult::Accepted);
    assert(bounded.Poll() == TxResult::Accepted && bounded.Pending() == 0 && capture.messages.size() == 16);
    for (auto failure : {TxResult::Invalid, TxResult::Unsupported}) {
        assert(bounded.Send(Event::NoteOn({0, 1}, 0, 60, 100)) == TxResult::Accepted);
        assert(bounded.Send(Event::NoteOff({0, 1}, 0, 60)) == TxResult::Accepted);
        capture.result = failure;
        assert(bounded.Poll() == failure && bounded.Pending() == 1);
        capture.result = TxResult::Accepted;
        assert(bounded.Poll() == TxResult::Accepted && bounded.Pending() == 0);
        assert(capture.messages.back().bytes[0] == 0x80);
    }
    assert(bounded.Cancelled() == 2);
    assert(bounded.Send(Event::NoteOff({0, 1}, 0, 60)) == TxResult::Accepted);
    capture.result = TxResult::Error;
    assert(bounded.Poll() == TxResult::Error && bounded.Pending() == 1);
    capture.result = TxResult::Accepted;
    assert(bounded.Poll() == TxResult::Accepted && bounded.Pending() == 0);
}

static void SchedulingHorizon() {
    for (uint32_t origin : {0u, 0xfffffff0u}) {
        Capture capture;
        Output<8> output({&capture, Capture::Submit, 8});
        const uint32_t deadlines[] = {origin - 0x7ffffff0u, origin + 0x7ffffff0u,
                                      origin, origin + 5u, origin + 5u};
        const uint8_t values[] = {10, 14, 11, 12, 13};
        for (size_t i = 0; i < 5; ++i)
            assert(output.Schedule(Event::Cc({0, 1}, 0, 1, values[i]), deadlines[i]) == TxResult::Accepted);
        assert(output.Poll(origin) == TxResult::Accepted);
        assert(capture.messages.size() == 2 && capture.messages[0].bytes[2] == 10 && capture.messages[1].bytes[2] == 11);
        output.Poll(origin + 5u);
        assert(capture.messages.size() == 4 && capture.messages[2].bytes[2] == 12 && capture.messages[3].bytes[2] == 13);
        output.Poll(origin + 20u);
        assert(capture.messages.size() == 4);
        output.Poll(origin + 0x7ffffff0u);
        assert(capture.messages.size() == 5 && capture.messages[4].bytes[2] == 14 && output.Pending() == 0);
    }
}

int main() {
    BundleLimitsAndFailures();
    SchedulingHorizon();
    Capture capture;Output<16> output({&capture,Capture::Submit,16,Capture::Bundle});
    assert(output.Send(Event::NoteOn({0,1},0,60,100,200))==TxResult::Accepted);
    assert(output.Send(Event::NoteOn({0,1},0,61,100,100))==TxResult::Accepted);
    assert(output.Poll(99)==TxResult::Accepted && capture.messages.empty());
    output.Poll(100);assert(capture.messages.size()==1 && capture.messages[0].bytes[1]==61);
    output.Poll(199);assert(capture.messages.size()==1);output.Poll(200);assert(capture.messages.size()==2);
    output.Send(Event::Cc({0,1},0,1,10));output.Send(Event::Cc({0,1},0,1,20));
    output.Send(Event::System(EventType::TimingClock,{0,1}));output.Send(Event::System(EventType::Stop,{0,1}));
    output.Poll(201);assert(capture.messages[2].bytes[0]==0xf8 && capture.messages[3].bytes[0]==0xfc);
    assert(capture.messages[4].bytes[2]==10 && capture.messages[5].bytes[2]==20);
    output.Send(Event::Cc({0,1},0,1,30));capture.result=TxResult::Busy;output.Poll(202);
    capture.result=TxResult::Accepted;output.Send(Event::System(EventType::TimingClock,{0,1}));output.Poll(203);
    assert(capture.messages[6].bytes[0]==0xf8 && capture.messages[7].bytes[2]==30);
    Event bundle[]={Event::Cc({0,1},0,101,0),Event::Cc({0,1},0,100,1),Event::Cc({0,1},0,6,12),Event::Cc({0,1},0,38,34)};
    assert(output.SendBundle(bundle,4,300)==TxResult::Accepted);
    output.Send(Event::Cc({0,1},0,7,99));output.Poll(299);assert(capture.messages.back().bytes[1]==7);
    capture.result=TxResult::Busy;output.Poll(300);assert(output.Pending()==4);
    capture.result=TxResult::Accepted;output.Poll(300,1);assert(capture.bundles==1 && output.Pending()==0);
    for(size_t i=0;i<4;++i)assert(capture.messages[capture.messages.size()-4+i].bytes[1]==bundle[i].data1);
    output.Schedule(Event::NoteOn({0,1},0,62,100),0xfffffff0u);output.Schedule(Event::NoteOff({0,1},0,62),0x10);
    output.Poll(0xffffffe0u);const auto before=capture.messages.size();
    output.Poll(0xfffffff0u);assert(capture.messages.size()==before+1);
    output.Poll(0x0f);assert(capture.messages.size()==before+1);output.Poll(0x10);assert(capture.messages.size()==before+2);
    Output<128> shuffled({&capture,Capture::Submit,128});
    std::vector<std::pair<uint32_t,unsigned>> expected;
    uint32_t random=17;
    for(unsigned i=0;i<128;++i){random=random*1664525+1013904223;uint32_t time=100+(random%1000);expected.push_back({time,i});assert(shuffled.Schedule(Event::Cc({0,1},0,1,uint8_t(i)),time)==TxResult::Accepted);}
    std::stable_sort(expected.begin(),expected.end(),[](auto a,auto b){return a.first<b.first;});
    const auto offset=capture.messages.size();shuffled.Poll(1100,128);
    for(unsigned i=0;i<128;++i)assert(capture.messages[offset+i].bytes[2]==expected[i].second);
    assert(shuffled.Pending()==0);
    Output<64> concurrent({nullptr,[](void*,const ShortMessage&,uint32_t){return TxResult::Accepted;},64});
    constexpr unsigned count=50000;
    std::atomic<bool> done{false};
    std::thread producer([&]{for(unsigned i=0;i<count;++i)while(concurrent.Send(Event::Cc({0,1},0,1,uint8_t(i%128)))==TxResult::Busy)std::this_thread::yield();done.store(true,std::memory_order_release);});
    while(!done.load(std::memory_order_acquire)||concurrent.Pending())concurrent.Poll(500);
    producer.join();assert(concurrent.Submitted()==count && concurrent.Pending()==0);
    output.SendBundle(bundle,4);output.Schedule(Event::NoteOn({0,1},0,60,100),1000);output.Poll(900,1);
    output.Clear();assert(output.Pending()==0);assert(output.Send(Event::NoteOn({0,1},0,63,100))==TxResult::Accepted);output.Poll(901);assert(capture.messages.back().bytes[1]==63);
}
