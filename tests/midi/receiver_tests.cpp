#include "alchemy/midi/receiver.h"
#include "alchemy/midi/safety.h"
#include <cassert>
using namespace alchemy::midi;
static void ResetTimestamps() {
    for (const alchemy::TickTimebase::Sample sample : {
             alchemy::TickTimebase::Sample{}, {0, 0, 2000000}, {0, 0, 1000000, 1000000}}) {
        Receiver<8> input;
        assert(input.Init({0, 1}, 1000000));
        input.Poll({0, 0x90000000u, 1000000}, [](const Event&) { assert(false); return true; });
        unsigned resets = 0;
        input.Poll(sample, [&](const Event& e) {
            assert(e.type == EventType::SourceReset && e.Valid() && !e.timestamp_valid);
            ++resets;
            return true;
        });
        assert(resets == 1 && input.Faulted() && input.ResetReported());
    }
    Receiver<8> input;
    assert(input.Init({0, 1}, 1000000));
    input.MarkLost();
    input.Poll({0, 0, 1000000}, [](const Event& e) {
        assert(e.type == EventType::SourceReset && e.timestamp_valid && e.time_us == 0);
        return true;
    });
    assert(input.ResetReported());
}

int main() {
    ResetTimestamps();
    Receiver<8> input;
    assert(input.Init({0,1},1000000));
    assert(!input.Init({0,1},1000000) && !input.Init({0,2},1000000));
    ActiveSensing<> safety;
    safety.Observe(Event::System(EventType::ActiveSensing,{0,1},0));
    unsigned activity=0, aborted=0;
    struct Context { ActiveSensing<>* safety; unsigned* activity; } ctx{&safety,&activity};
    input.SetActivity({&ctx,[](void* p, SourceId s, uint32_t time) {
        auto& c=*static_cast<Context*>(p); c.safety->Touch(s,time); ++*c.activity;
    }});
    input.SetSysExSink({&aborted,[](void* p,const SysExChunk& chunk) {
        if(chunk.phase==SysExPhase::Abort) ++*static_cast<unsigned*>(p);
    }});
    unsigned events=0;
    const uint8_t chunk[]={0xf0,1,2,3};
    input.Receive(chunk,4,200000);
    input.Poll({200000,200000,1000000},[&](const Event&){++events;return true;});
    assert(activity==4 && events==0);
    assert(safety.Poll(350000,[](const Event&){assert(false);})==0);
    input.Receive(chunk,4,400000); input.Receive(chunk,4,400001); input.Receive(chunk,4,400002);
    assert(input.Faulted());
    input.Poll({400002,400002,1000000},[&](const Event& e){assert(e.type==EventType::SourceReset && e.source.session==1);++events;return true;});
    assert(events==1 && input.Faulted() && aborted==1);
    input.Poll({400003,400003,1000000},[&](const Event&){assert(false);return true;});
    assert(!input.Restart({0,1}) && !input.Restart({0,0}));
    assert(input.Restart({0,2}) && !input.Faulted());
    assert(!input.Restart({0,1}));
    const uint8_t notes[]={0x90,60,100,61,0};
    input.Receive(notes,5,500000);
    input.Poll({500003,500003,1000000},[&](const Event& e) {
        assert(e.source.session==2 && e.time_us==500000);
        if(e.data1==60) assert(e.type==EventType::NoteOn && e.value==100 && e.sequence==1);
        else assert(e.type==EventType::NoteOff && e.note_off_origin==NoteOffOrigin::ZeroVelocityNoteOn && e.sequence==2);
        ++events;return true;
    });
    assert(events==3);
    input.Receive(notes,5,500004);
    assert(input.Poll({500003,500003,1000000},[&](const Event&){assert(false);return true;})==0 && !input.Faulted());
    assert(input.Poll({500004,500004,1000000},[&](const Event& e){assert(e.time_us==500004);return true;})==5 && !input.Faulted());
    input.Receive(notes,5,500004);
    input.Poll({500004,500004,1000000},[&](const Event&){return false;});
    assert(input.Faulted());
    input.Poll({500005,500005,1000000},[&](const Event& e){assert(e.type==EventType::SourceReset);++events;return true;});
    assert(events==4 && input.Faulted());
    Receiver<8> stalled;
    assert(stalled.Init({1,1},1000000));
    stalled.Receive(notes,5,100);
    assert(stalled.Poll({50,0,1000000},[](const Event&){assert(false);return true;})==0 && !stalled.Faulted());
    assert(stalled.Poll({50,1000000,1000000},[](const Event&){assert(false);return true;})==0 && !stalled.Faulted());
    assert(stalled.Poll({50,2000001,1000000},[](const Event&){assert(false);return true;})==0 && stalled.Faulted());
    stalled.Poll({50,2000002,1000000},[](const Event& e){assert(e.type==EventType::SourceReset);return true;});
    Receiver<8> fractional;
    assert(fractional.Init({2,1},200000000));
    const uint8_t pulses[]={0xf8,0xf8};fractional.Receive(pulses,2,199);
    uint32_t first_stamp=0;
    fractional.Poll({398,1,200000000,198000000},[&](const Event& e){first_stamp=e.time_us;return true;},1);
    assert(first_stamp==0);
    fractional.Poll({399,1,200000000,199000000},[&](const Event& e){assert(e.time_us==first_stamp);return true;},1);
    fractional.Poll({300,21474837,200000000,196000000},[](const Event&){assert(false);return true;});
    fractional.Receive(pulses,2,199);
    fractional.Poll({300,21474837,200000000,196000000},[](const Event& e){assert(e.time_us==21474837);return true;});
    assert(!fractional.Faulted());
    Receiver<8> alias;
    assert(alias.Init({3,1},200000000));
    alias.Poll({100,0,200000000},[](const Event&){return true;});alias.Receive(notes,5,199);
    alias.Poll({300,21474837,200000000},[](const Event& e){assert(e.type==EventType::SourceReset);return true;});
    assert(alias.Faulted());
    Receiver<8> recovery;assert(recovery.Init({4,1},1000000));
    recovery.MarkLost();
    recovery.Poll({0,0,1000000},[&](const Event& e){assert(e.source.session==1);assert(recovery.Restart({4,2}));return true;});
    assert(!recovery.Faulted());recovery.MarkLost();
    unsigned reset=0;
    recovery.Poll({1,1,1000000},[&](const Event& e){assert(e.type==EventType::SourceReset && e.source.session==2);++reset;return true;});
    assert(reset==1 && recovery.Faulted());
}
