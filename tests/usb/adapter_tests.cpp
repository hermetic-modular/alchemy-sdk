#include "alchemy/host_link/cdc_transport.h"
#include "alchemy/midi/usb_transport.h"
#include "alchemy/midi/wire.h"
#include "alchemy/midi/ingress.h"
#include "alchemy/midi/dispatcher.h"
#include "fixture.h"
extern "C" {
#include "usbd_ctlreq.h"
#include "usbd_desc.h"
extern USBD_HandleTypeDef hUsbDeviceHS;
}
#include <cassert>
#include <cstring>
#include <vector>
using namespace alchemy;
using namespace alchemy::midi;
static void Configure() {
    auto& device=hUsbDeviceHS;
    assert(USBD_LL_Reset(&device)==USBD_OK);
    device.dev_state=USBD_STATE_ADDRESSED;
    USBD_SetupReqTypedef request{0,USB_REQ_SET_CONFIGURATION,1,0,0};
    assert(USBD_StdDevReq(&device,&request)==USBD_OK);
}
static void Input(uint8_t endpoint,const uint8_t* data,size_t size) {
    assert(size<=64);std::memcpy(out[endpoint].buffer,data,size);out[endpoint].received=uint32_t(size);
    assert(USBD_LL_DataOutStage(&hUsbDeviceHS,endpoint,out[endpoint].buffer)==USBD_OK);
}
static std::vector<uint8_t> Drain(uint8_t endpoint) {
    usb::Device::Shared().Poll();std::vector<uint8_t> data;
    unsigned transfers=0;
    while(in[endpoint].active){assert(++transfers<1000);if(in[endpoint].size)data.insert(data.end(),in[endpoint].buffer,in[endpoint].buffer+in[endpoint].size);CompleteIn(hUsbDeviceHS,endpoint);}
    return data;
}
static void CallbackOwnership() {
    auto& device=usb::Device::Shared();
    const usb::ReceiveCallback context_free{nullptr,[](void*,const uint8_t*,size_t){}};
    int owner=0;
    for(auto set:{&usb::Device::SetCdcReceiver,&usb::Device::SetMidiReceiver}) {
        assert((device.*set)(context_free));
        assert(!(device.*set)({&owner,context_free.receive}));
        assert(!(device.*set)({&owner,nullptr}));
        assert((device.*set)({nullptr,nullptr}));
    }
}
static void SharedDeviceOwnership() {
    auto& device=usb::Device::Shared();
    USBD_DescriptorsTypeDef descriptors{};USBD_ClassTypeDef foreign{};int owner=0;
    foreign.DeInit=[](USBD_HandleTypeDef*,uint8_t){return uint8_t(USBD_OK);};
    assert(USBD_Init(&hUsbDeviceHS,&descriptors,DEVICE_HS)==USBD_OK);
    assert(USBD_RegisterClass(&hUsbDeviceHS,&foreign)==USBD_OK);
    hUsbDeviceHS.pUserData[0]=&owner;
    auto* peripheral=hUsbDeviceHS.pData;
    assert(!device.Init(daisy::UsbHandle::FS_EXTERNAL,"Other module"));
    assert(!device.Initialized() && hUsbDeviceHS.pClass[0]==&foreign);
    assert(hUsbDeviceHS.pUserData[0]==&owner && hUsbDeviceHS.pDesc==&descriptors);
    assert(hUsbDeviceHS.pData==peripheral && hUsbDeviceHS.NumClasses==1);
    assert(USBD_DeInit(&hUsbDeviceHS)==USBD_OK);hUsbDeviceHS={};
    fail_start=true;assert(!device.Init(daisy::UsbHandle::FS_EXTERNAL));
    assert(!device.Initialized() && !hUsbDeviceHS.pUserData[0]);fail_start=false;
}
static void ReconnectWithDispatcher(DaisyUsbPort<>& midi, unsigned reconnects) {
    struct Consumer {
        Ingress<16, 1> audio;
        bool allow_reset = false;
    } consumer;
    Dispatcher<1, 16, 1> dispatcher;
    assert(dispatcher.Subscribe(CriticalSink{&consumer, [](void* p, const Event& e) {
        auto& c = *static_cast<Consumer*>(p);
        return (e.type != EventType::SourceReset || c.allow_reset) && c.audio.Push(e);
    }}));
    const auto poll = [&] {
        if (dispatcher.Failed(midi.Source())) midi.MarkLost();
        midi.Poll({213,213,1000000,0}, [&](const Event& e) { return dispatcher.Accept(e); });
        dispatcher.Poll(213);
    };
    const auto old = midi.Source();
    const uint8_t held[] = {9, 0x90, 60, 100};
    Input(3, held, sizeof(held)); poll();
    assert(consumer.audio.DrainDue(213, [&](const Event& e) {
        assert(e.type == EventType::NoteOn && e.source == old && e.data1 == 60);
    }) == 1);

    for (unsigned i = 0; i < reconnects; ++i) {
        assert(USBD_LL_DevDisconnected(&hUsbDeviceHS) == USBD_OK);
        Configure();
        const uint8_t fresh[] = {9, 0x90, uint8_t(62 + i), 100};
        Input(3, fresh, sizeof(fresh));
        poll();
        assert(consumer.audio.Size() == 0);
    }

    consumer.allow_reset = true;
    poll();
    poll();
    std::vector<Event> delivered;
    consumer.audio.DrainDue(213, [&](const Event& e) { delivered.push_back(e); });
    assert(delivered.size() == reconnects + 1);
    for (unsigned i = 0; i < reconnects; ++i) {
        assert(delivered[i].type == EventType::SourceReset);
        assert(delivered[i].source == (SourceId{old.port, old.session + i}));
    }
    const auto& last = delivered.back();
    assert(last.type == EventType::NoteOn && last.data1 == 61 + reconnects);
    assert(last.source == midi.Source() && last.source.session == old.session + reconnects);
    assert(!midi.Faulted() && dispatcher.Lost() == 0);
}

static void RestartDuringSysEx(DaisyUsbPort<>& midi) {
    struct Context { DaisyUsbPort<>* port; unsigned aborts=0; } context{&midi};
    midi.SetSysExSink({&context,[](void* p,const SysExChunk& chunk) {
        assert(!__get_PRIMASK());
        if(chunk.phase!=SysExPhase::Abort)return;
        auto& state=*static_cast<Context*>(p);++state.aborts;
        assert(state.port->Submit({{0x90,65,100},3})==TxResult::Accepted);
    }});
    const uint8_t begin[]={4,0xf0,0x7d,1};Input(3,begin,sizeof(begin));
    midi.Poll({213,213,1000000,0},[](const Event&){return true;});
    assert(!midi.Restart(midi.Source()) && context.aborts==0);
    const uint8_t stale[]={9,0x90,61,100};Input(3,stale,sizeof(stale));
    auto source=midi.Source();++source.session;
    assert(midi.Restart(source) && context.aborts==1 && !__get_PRIMASK());
    const auto sent=Drain(3);assert(sent.size()==4 && sent[1]==0x90 && sent[2]==65);
    const uint8_t fresh[]={9,0x90,62,100};Input(3,fresh,sizeof(fresh));
    unsigned notes=0;
    midi.Poll({213,213,1000000,0},[&](const Event& e){
        assert(e.type==EventType::NoteOn && e.source==source && e.data1==62);++notes;return true;
    });
    assert(notes==1 && midi.Source()==source && !midi.Faulted());
    midi.SetSysExSink({});
}
int main() {
    CallbackOwnership();SharedDeviceOwnership();
    DaisyUsbPort<> midi;hostlink::CdcUsbTransport cdc;
    char product[]="Test module";
    assert(usb::Device::Shared().Configure(daisy::UsbHandle::FS_EXTERNAL,product,{0x1209,0x1001,0x0203}));
    assert(midi.Init({}, {0,1}));
    assert(cdc.Init(daisy::UsbHandle::FS_EXTERNAL,product));
    product[0]='X';
    uint16_t descriptor_size=0;
    const auto* identity=hUsbDeviceHS.pDesc->GetDeviceDescriptor(USBD_SPEED_FULL,&descriptor_size);
    assert(descriptor_size==18 && identity[8]==9 && identity[9]==0x12 && identity[10]==1 && identity[11]==0x10 && identity[12]==3 && identity[13]==2);
    const auto* product_descriptor=hUsbDeviceHS.pDesc->GetProductStrDescriptor(USBD_SPEED_FULL,&descriptor_size);
    assert(descriptor_size==24 && product_descriptor[2]=='T' && product_descriptor[3]==0);
    assert(!usb::Device::Shared().Init(daisy::UsbHandle::FS_INTERNAL));
    DaisyUsbPort<> other;assert(!other.Init({}, {1,1}));
    Configure();
    assert(!usb::Device::Shared().Configure(daisy::UsbHandle::FS_EXTERNAL,"Other"));
    assert(!usb::Device::Shared().Init(daisy::UsbHandle::FS_EXTERNAL,"Other"));
    assert(usb::Device::Shared().Init(daisy::UsbHandle::FS_EXTERNAL,"Test module"));
    std::vector<Event> events;auto receive=[&](const Event& event){events.push_back(event);return true;};
    const uint8_t note[]={9,0x90,60,100};const uint8_t hello[]={1,2,3,4,0};
    Input(3,note,4);Input(1,hello,5);
    uint8_t bytes[64];assert(cdc.Read(bytes,64)==5 && !std::memcmp(bytes,hello,5));
    test_tick=200;midi.Poll({200,200,1000000,0},receive);
    assert(events.size()==2 && events[0].type==EventType::SourceReset && events[1].type==EventType::NoteOn && events[1].time_us==100);
    assert(events[1].source.session==midi.Source().session && midi.Source().session==2);
    Output<16> output(midi.Sink());
    for(unsigned i=0;i<8;++i)assert(output.Send(Event::Cc({0,1},0,1,uint8_t(i)))==TxResult::Accepted);
    output.Poll(200);assert(output.Pending()==0);
    assert(cdc.WriteSpace()>=2048 && cdc.Write(hello,5)==5);
    assert(output.Send(Event::System(EventType::TimingClock,{0,1}))==TxResult::Accepted);output.Poll(200);
    auto packets=Drain(3);assert(packets.size()==36 && packets[0]==15 && packets[1]==0xf8);
    for(unsigned i=0;i<8;++i)assert(packets[4+i*4]==11 && packets[7+i*4]==i);
    auto serial=Drain(1);assert(serial==std::vector<uint8_t>(hello,hello+5));
    Event bundle[]={Event::System(EventType::SongPosition,{0,1},0,12),Event::System(EventType::Start,{0,1})};
    assert(output.SendBundle(bundle,2)==TxResult::Accepted);output.Poll(200);
    packets=Drain(3);assert(packets.size()==8 && packets[1]==0xf2 && packets[5]==0xfa);
    test_isr=1;ShortMessage short_note{{0x90,61,100},3};assert(midi.Submit(short_note)==TxResult::Invalid);test_isr=0;
    std::vector<uint8_t> sysex{0xf0};for(unsigned i=0;i<1400;++i)sysex.push_back(uint8_t(i%128));sysex.push_back(0xf7);
    std::vector<uint8_t> wire;
    size_t offset=0;
    while(offset<sysex.size()){
        auto result=midi.WriteSysEx(sysex.data()+offset,sysex.size()-offset,256);offset+=result.accepted;
        assert(result.result==TxResult::Accepted || result.result==TxResult::Busy);
        auto chunk=Drain(3);wire.insert(wire.end(),chunk.begin(),chunk.end());
    }
    auto final=Drain(3);wire.insert(wire.end(),final.begin(),final.end());
    std::vector<uint8_t> decoded;
    for(size_t i=0;i<wire.size();i+=4){UsbPacket packet;std::memcpy(packet.bytes.data(),wire.data()+i,4);uint8_t cable;const uint8_t* data;size_t size;assert(DecodeUsb(packet,cable,data,size));decoded.insert(decoded.end(),data,data+size);}
    assert(decoded==sysex);
    assert(output.Schedule(Event::NoteOn({0,1},0,65,100),6000)==TxResult::Accepted);output.Poll(200);
    const auto stale_generation=midi.DestinationGeneration();
    assert(midi.Submit(short_note)==TxResult::Accepted);
    USBD_LL_DevDisconnected(&hUsbDeviceHS);assert(midi.Submit(short_note)==TxResult::Disconnected && midi.CancelledTransfers()>0);
    Configure();Input(3,note,4);Input(1,hello,5);
    bool accept_reset=false;auto reject=[&](const Event& event){if(event.type==EventType::SourceReset&&!accept_reset)return false;events.push_back(event);return true;};
    const auto before=events.size();midi.Poll({200,200,1000000,0},reject);assert(events.size()==before);
    accept_reset=true;midi.Poll({200,200,1000000,0},reject);assert(events.size()==before+2 && events.back().type==EventType::NoteOn && midi.Source().session==3);
    assert(cdc.Read(bytes,64)==5 && !std::memcmp(bytes,hello,5));
    assert(midi.Submit(short_note,stale_generation)==TxResult::Disconnected);
    assert(output.Send(Event::NoteOn({0,1},0,66,100))==TxResult::Accepted);output.Poll(200);
    packets=Drain(3);assert(packets.size()==4 && packets[2]==66 && output.Cancelled()==1);
    output.Poll(6000);assert(Drain(3).empty());
    const uint8_t malformed[]={9,0x80,60,100};Input(3,malformed,4);midi.Poll({200,200,1000000,0},receive);assert(midi.Faulted() && events.back().type==EventType::SourceReset);
    midi.Poll({200,200,1000000,0},receive);assert(!midi.Faulted() && midi.Source().session==4);
    Input(3,note,4);midi.Poll({200,200,1000000,0},receive);assert(!midi.Faulted() && events.back().type==EventType::NoteOn && events.back().source.session==4);
    // A halt recovery cancels MIDI output only. Existing input state and CDC survive.
    const auto input_generation=usb::Device::Shared().Generation();
    const auto input_session=midi.Source();
    const auto tx_generation=midi.DestinationGeneration();
    const uint8_t begin[]={0xf0,0x7d,1}, end[]={2,3,0xf7};
    assert(midi.WriteSysEx(begin,3).accepted==3);
    usb::Device::Shared().Poll();assert(in[3].active);
    assert(cdc.Write(hello,5)==5);
    USBD_SetupReqTypedef clear{2,USB_REQ_CLEAR_FEATURE,USB_FEATURE_EP_HALT,usb::kMidiIn,0};
    auto* active_buffer=in[3].buffer;const auto flushes=in[3].flushed;
    assert(midi.WriteSysEx(end,3).accepted==3);
    assert(USBD_StdEPReq(&hUsbDeviceHS,&clear)==USBD_OK);
    assert(in[3].flushed==flushes && in[3].active && in[3].buffer==active_buffer);
    assert(midi.DestinationGeneration()!=tx_generation && usb::Device::Shared().Generation()==input_generation);
    const auto event_count=events.size();midi.Poll({200,200,1000000,0},receive);
    assert(events.size()==event_count && midi.Source()==input_session);
    assert(midi.WriteSysEx(begin,3).accepted==3);
    assert(USBD_StdEPReq(&hUsbDeviceHS,&clear)==USBD_OK);
    auto cancelled=midi.WriteSysEx(end,3);assert(cancelled.result==TxResult::Disconnected && cancelled.accepted==0);
    assert(midi.Submit(short_note,tx_generation)==TxResult::Disconnected);
    assert(midi.Submit(short_note)==TxResult::Accepted);
    packets=Drain(3);assert(packets.size()==8 && packets[0]==4 && packets[1]==0xf0 && packets[5]==0x90);
    assert(Drain(1)==std::vector<uint8_t>(hello,hello+5));
    const uint8_t complete[]={0xf0,0x7d,0xf7};
    assert(midi.WriteSysEx(complete,3).accepted==3);assert(Drain(3).size()==4);

    // Suspend can occur without a disconnect.
    assert(output.Schedule(Event::NoteOn({0,1},0,70,100),9000)==TxResult::Accepted);output.Poll(200);
    assert(midi.Submit(short_note)==TxResult::Accepted);
    assert(midi.WriteSysEx(begin,3).accepted==3);
    usb::Device::Shared().Poll();assert(in[3].active);
    const uint8_t unsent[]={2,3,4};assert(midi.WriteSysEx(unsent,3).accepted==3);
    const auto pending_cancelled=midi.CancelledTransfers();
    const auto resume_session=midi.Source();const auto suspend_tx=midi.DestinationGeneration();
    const auto before_suspend=events.size();
    assert(USBD_LL_Suspend(&hUsbDeviceHS)==USBD_OK);
    assert(usb::Device::Shared().Configured() && usb::Device::Shared().Suspended() && !midi.Available());
    midi.Poll({201,201,1000000,0},receive);
    assert(events.size()==before_suspend+1 && events.back().type==EventType::SourceReset && midi.Source()==resume_session);
    assert(midi.DestinationGeneration()!=suspend_tx && cdc.WriteSpace()==0 && midi.CancelledTransfers()==pending_cancelled+1);
    assert(midi.Submit(short_note)==TxResult::Disconnected && cdc.Write(hello,5)==0);
    midi.Poll({202,202,1000000,0},receive);assert(events.size()==before_suspend+1);
    output.Poll(202);assert(output.Pending()==0 && output.Cancelled()==2);
    assert(USBD_LL_Resume(&hUsbDeviceHS)==USBD_OK);
    midi.Poll({203,203,1000000,0},receive);
    assert(midi.Available() && midi.Source().session==resume_session.session+1 && events.size()==before_suspend+1);
    assert(in[3].active);packets=Drain(3);
    assert(packets.size()==8 && packets[1]==0x90 && packets[5]==0xf0); // Already-started transfer only.
    cancelled=midi.WriteSysEx(end,3);assert(cancelled.result==TxResult::Disconnected && cancelled.accepted==0);
    assert(midi.WriteSysEx(complete,3).accepted==3);assert(Drain(3).size()==4);
    Input(3,note,4);midi.Poll({203,203,1000000,0},receive);assert(events.back().source==midi.Source());
    output.Poll(9000);assert(Drain(3).empty());

    // A parsed SysEx continuation cannot use channel running status.
    const uint8_t orphan[]={11,0xb0,1,0,4,74,127,0};
    const auto before_orphan=events.size();Input(3,orphan,8);midi.Poll({203,203,1000000,0},receive);
    assert(midi.Faulted() && events.size()==before_orphan+1 && events.back().type==EventType::SourceReset);
    midi.Poll({204,204,1000000,0},receive);assert(!midi.Faulted());
    const uint8_t unparsed[]={15,0x90,0,0,15,61,0,0,15,100,0,0};
    Input(3,unparsed,12);midi.Poll({204,204,1000000,0},receive);
    assert(events.back().type==EventType::NoteOn && events.back().data1==61);
    midi.MarkLost();midi.Poll({205,205,1000000,0},receive);assert(!midi.Faulted());
    // Queue overflow must recover through the real adapter, without reconnecting USB.
    const uint8_t burst[]={9,0x90,60,100,9,0x90,61,100,9,0x90,62,100};
    Ingress<2> tiny;
    auto to_ingress=[&](const Event& e){return tiny.Push(e);};
    auto old_source=midi.Source();Input(3,burst,12);midi.Poll({206,206,1000000,0},to_ingress);
    assert(midi.Faulted() && tiny.Failed(old_source));
    midi.Poll({207,207,1000000,0},to_ingress);assert(midi.Faulted() && midi.Source()==old_source);
    unsigned resets=0, recovered_notes=0;
    tiny.Drain(207,[&](const Event& e){resets+=e.type==EventType::SourceReset;recovered_notes+=e.type==EventType::NoteOn;});
    assert(resets==1 && recovered_notes==0);
    midi.Poll({208,208,1000000,0},to_ingress);
    assert(!midi.Faulted() && midi.Source().session==old_source.session+1);
    Input(3,note,4);midi.Poll({209,209,1000000,0},to_ingress);
    tiny.Drain(209,[&](const Event& e){assert(e.source==midi.Source());recovered_notes+=e.type==EventType::NoteOn;});
    assert(recovered_notes==1 && !tiny.Push(Event::NoteOn(old_source,0,60,100)));

    // Checked fan-out reports a downstream fault and coordinates session attachment.
    Ingress<2> downstream;Dispatcher<1,8,1> dispatcher;
    assert(dispatcher.Subscribe(CriticalSink{&downstream,[](void* context,const Event& e){return static_cast<Ingress<2>*>(context)->Push(e);}}));
    auto to_dispatcher=[&](const Event& e){return dispatcher.Accept(e);};
    old_source=midi.Source();Input(3,burst,12);midi.Poll({210,210,1000000,0},to_dispatcher);dispatcher.Poll(210);
    assert(!midi.Faulted() && dispatcher.Failed(old_source) && downstream.Failed(old_source));
    if(dispatcher.Failed(midi.Source()))midi.MarkLost();
    midi.Poll({211,211,1000000,0},to_dispatcher);assert(midi.Faulted());
    resets=recovered_notes=0;
    downstream.Drain(211,[&](const Event& e){resets+=e.type==EventType::SourceReset;recovered_notes+=e.type==EventType::NoteOn;});
    dispatcher.Poll(211);midi.Poll({212,212,1000000,0},to_dispatcher);
    assert(!midi.Faulted() && midi.Source().session==old_source.session+1 && resets==1 && recovered_notes==0);
    Input(3,note,4);midi.Poll({213,213,1000000,0},to_dispatcher);dispatcher.Poll(213);
    downstream.Drain(213,[&](const Event& e){assert(e.source==midi.Source());recovered_notes+=e.type==EventType::NoteOn;});
    assert(recovered_notes==1 && !dispatcher.Failed(midi.Source()) && !dispatcher.Accept(Event::NoteOn(old_source,0,60,100)));

    // The USB sink advertises its actual atomic bundle limit before acceptance.
    Output<64,32> chords(midi.Sink());Event chord[17];
    for(unsigned i=0;i<17;++i)chord[i]=Event::NoteOn(midi.Source(),0,uint8_t(40+i),100);
    assert(chords.SendBundle(chord,17)==TxResult::Invalid && chords.Pending()==0);
    assert(chords.Send(Event::NoteOff(midi.Source(),0,60))==TxResult::Accepted);
    chords.Poll(213);assert(chords.Pending()==0 && Drain(3).size()==4);
    ReconnectWithDispatcher(midi, 1);
    ReconnectWithDispatcher(midi, 2);
    RestartDuringSysEx(midi);
    // Releasing a callback slot must not let its former owner's destructor clear a replacement.
    auto& device=usb::Device::Shared();
    assert(device.SetMidiReceiver({&midi,nullptr}));
    {
        DaisyUsbPort<> replacement;
        {
            DaisyUsbPort<> former;assert(former.Init({}, {0,20}));
            assert(device.SetMidiReceiver({&former,nullptr}));
            assert(replacement.Init({}, {0,21}));
        }
        Input(3,note,4);unsigned received=0;
        replacement.Poll({214,214,1000000,0},[&](const Event& e){received+=e.type==EventType::NoteOn;return true;});
        assert(received==1);
    }
    assert(device.SetCdcReceiver({&cdc,nullptr}));
    {
        hostlink::CdcUsbTransport replacement;
        {
            hostlink::CdcUsbTransport former;assert(former.Init());
            assert(device.SetCdcReceiver({&former,nullptr}));assert(replacement.Init());
        }
        Input(1,hello,5);assert(replacement.Read(bytes,sizeof(bytes))==5 && !std::memcmp(bytes,hello,5));
    }
    device.Stop();assert(!device.Initialized());
    assert(device.Init(daisy::UsbHandle::FS_EXTERNAL,"Test module"));
    Configure();device.Stop();assert(!device.Initialized());
    USBD_DescriptorsTypeDef foreign_descriptors{};usb::DeviceClass foreign_service;
    assert(USBD_Init(&hUsbDeviceHS,&foreign_descriptors,DEVICE_HS)==USBD_OK);
    foreign_service.Attach(hUsbDeviceHS);
    assert(USBD_RegisterClass(&hUsbDeviceHS,&usb::DeviceClass::Driver())==USBD_OK);
    assert(!device.Init(daisy::UsbHandle::FS_EXTERNAL,"Test module"));
    assert(hUsbDeviceHS.pUserData[0]==&foreign_service && hUsbDeviceHS.pDesc==&foreign_descriptors);
    assert(USBD_DeInit(&hUsbDeviceHS)==USBD_OK);
}
