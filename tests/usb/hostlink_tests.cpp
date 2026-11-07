#include "alchemy/host_link/host.h"
#include "alchemy/midi/usb_transport.h"
#include "fixture.h"
extern "C" {
#include "usbd_ctlreq.h"
extern USBD_HandleTypeDef hUsbDeviceFS;
}
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
using namespace alchemy;

static void ConfigureBus() {
    assert(USBD_LL_Reset(&hUsbDeviceFS)==USBD_OK);
    hUsbDeviceFS.dev_state=USBD_STATE_ADDRESSED;
    USBD_SetupReqTypedef request{0,USB_REQ_SET_CONFIGURATION,1,0,0};
    assert(USBD_StdDevReq(&hUsbDeviceFS,&request)==USBD_OK);
}
static void Input(uint8_t endpoint,const uint8_t* bytes,size_t size) {
    assert(size<=64);
    std::memcpy(out[endpoint].buffer,bytes,size);out[endpoint].received=uint32_t(size);
    assert(USBD_LL_DataOutStage(&hUsbDeviceFS,endpoint,out[endpoint].buffer)==USBD_OK);
}
static std::vector<uint8_t> Drain(uint8_t endpoint) {
    usb::Device::Shared().Poll();
    std::vector<uint8_t> bytes;
    while(in[endpoint].active) {
        if(in[endpoint].size)bytes.insert(bytes.end(),in[endpoint].buffer,in[endpoint].buffer+in[endpoint].size);
        CompleteIn(hUsbDeviceFS,endpoint);
    }
    return bytes;
}
static void Product(const char* expected) {
    uint16_t size=0;
    const auto* descriptor=hUsbDeviceFS.pDesc->GetProductStrDescriptor(USBD_SPEED_FULL,&size);
    assert(size==2+2*std::strlen(expected));
    for(size_t i=0;expected[i];++i)assert(descriptor[2+2*i]==expected[i] && descriptor[3+2*i]==0);
}
static void Hello(hostlink::Host& host,uint16_t sequence) {
    uint8_t decoded[hostlink::kMaxDecoded],wire[hostlink::kMaxWire];
    hostlink::FrameWriter writer(decoded);writer.Begin(uint8_t(hostlink::Cmd::Hello),sequence);
    Input(1,wire,writer.Encode(wire));
    host.Poll(0);
    hostlink::FrameParser parser;hostlink::ParsedFrame response{};
    unsigned responses=0;
    for(const auto byte:Drain(1))if(parser.Push(byte,response)) {
        assert(response.ok && response.seq==sequence && response.type==(uint8_t(hostlink::Cmd::Hello)|hostlink::kRespFlag));
        assert(response.len>=45 && response.body[0]==uint8_t(hostlink::Status::Ok));
        assert(response.body[37]==7 && !std::memcmp(response.body+38,"startup",7));
        ++responses;
    }
    assert(responses==1);
}
static void SerialOpen(bool open) {
    USBD_SetupReqTypedef request{0x21,0x22,uint16_t(open),0,0};
    assert(USBD_StdItfReq(&hUsbDeviceFS,&request)==USBD_OK);
}
static void InterruptHello(hostlink::Host& host) {
    uint8_t decoded[hostlink::kMaxDecoded],wire[hostlink::kMaxWire];
    hostlink::FrameWriter interrupted(decoded);
    interrupted.Begin(uint8_t(hostlink::Cmd::Hello),99);
    Input(1,wire,interrupted.Encode(wire)-1);
    host.Poll(0);
    assert(Drain(1).empty());
}
static void ReconnectDuringResponse(hostlink::Host& host, midi::DaisyUsbPort<>& midi,
                                    const std::string& descriptor) {
    const auto source=midi.Source();
    const auto generation=midi.DestinationGeneration();
    uint8_t decoded[hostlink::kMaxDecoded],wire[hostlink::kMaxWire];
    hostlink::FrameWriter request(decoded);
    request.Begin(uint8_t(hostlink::Cmd::GetDescriptor),70);request.U32(0);request.U16(500);
    Input(1,wire,request.Encode(wire));host.Poll(0);
    assert(midi.Submit({{0x90,64,100},3})==midi::TxResult::Accepted);
    usb::Device::Shared().Poll();
    assert(in[1].active && in[1].size==256 && in[3].active);
    SerialOpen(false);SerialOpen(true);
    request.Begin(uint8_t(hostlink::Cmd::Hello),71);
    Input(1,wire,request.Encode(wire));host.Poll(0);
    hostlink::FrameParser parser;hostlink::ParsedFrame response{};
    unsigned descriptors=0,hellos=0;
    for(const auto byte:Drain(1))if(parser.Push(byte,response)) {
        assert(response.ok && response.body[0]==uint8_t(hostlink::Status::Ok));
        if(response.seq==70) {
            assert(response.type==(uint8_t(hostlink::Cmd::GetDescriptor)|hostlink::kRespFlag));
            assert(response.len==507 && hostlink::RdU16(response.body+5)==500);
            assert(!std::memcmp(response.body+7,descriptor.data(),500));
            ++descriptors;
        } else {
            assert(response.seq==71 && response.type==(uint8_t(hostlink::Cmd::Hello)|hostlink::kRespFlag));
            ++hellos;
        }
    }
    assert(descriptors==1 && hellos==1);
    const uint8_t note[]={9,0x90,62,100};Input(3,note,sizeof(note));
    unsigned notes=0;
    midi.Poll({100,100,1000000,0},[&](const midi::Event& e){
        assert(e.type==midi::EventType::NoteOn && e.source==source && e.data1==62);++notes;return true;
    });
    const auto packets=Drain(3);
    assert(notes==1 && midi.Source()==source && midi.DestinationGeneration()==generation);
    assert(packets.size()==4 && packets[1]==0x90 && packets[2]==64);
}
int main(int argc,char** argv) {
    assert(argc==2);
    const std::string scenario=argv[1];
    auto& device=usb::Device::Shared();
    hostlink::Host host("startup","HostLink Demo","test","git");
    midi::DaisyUsbPort<> midi;
    const std::string descriptor="{\"data\":\""+std::string(1000,'x')+"\"}";
    host.Descriptor(descriptor.data(),uint32_t(descriptor.size()));
    const bool configured=scenario=="configured" || scenario=="explicit-match";
    if(configured)assert(device.Configure(daisy::UsbHandle::FS_INTERNAL,"Chosen USB Name"));
    if(scenario=="explicit-match")host.Product("Chosen USB Name");
    if(scenario=="explicit-conflict")host.Product("Different Explicit Name");
    if(scenario=="host-first" || scenario=="configured") {
        host.Poll(0);
        assert(midi.Init({daisy::UsbHandle::FS_INTERNAL}, {0,1}));
    } else {
        assert(midi.Init({daisy::UsbHandle::FS_INTERNAL}, {0,1}));
        host.Poll(0);
    }
    assert(host.Engine());
    ConfigureBus();
    SerialOpen(true);
    Product(configured?"Chosen USB Name":scenario=="host-first"?"HostLink Demo":"Alchemy");
    // A fallback is ignored once identity exists; an explicit conflict still fails.
    assert(device.Init(daisy::UsbHandle::FS_INTERNAL,nullptr,""));
    assert(!device.Init(daisy::UsbHandle::FS_INTERNAL,"Different Explicit Name"));
    const uint8_t note[]={9,0x90,60,100};
    Input(3,note,sizeof(note));
    unsigned notes=0;
    midi.Poll({100,100,1000000,0},[&](const midi::Event& e){notes+=e.type==midi::EventType::NoteOn;return true;});
    assert(notes==1);
    if(scenario=="explicit-conflict") { assert(!host.ConfigurationOk());return 0; }
    assert(host.ConfigurationOk());
    const midi::ShortMessage output{{0x90,64,100},3};
    assert(midi.Submit(output)==midi::TxResult::Accepted);
    Hello(host,7);
    const auto packets=Drain(3);assert(packets.size()==4 && packets[1]==0x90 && packets[2]==64);
    if(scenario=="midi-first")ReconnectDuringResponse(host,midi,descriptor);
    InterruptHello(host);
    const auto source=midi.Source();
    const auto output_generation=midi.DestinationGeneration();
    assert(midi.Submit(output)==midi::TxResult::Accepted);
    SerialOpen(false);SerialOpen(true);
    Hello(host,8);
    Input(3,note,sizeof(note));
    midi.Poll({101,101,1000000,0},[&](const midi::Event& e){notes+=e.type==midi::EventType::NoteOn;return true;});
    assert(notes==2 && midi.Source()==source && midi.DestinationGeneration()==output_generation);
    assert(Drain(3).size()==4);
    InterruptHello(host);
    assert(USBD_LL_DevDisconnected(&hUsbDeviceFS)==USBD_OK);
    ConfigureBus();
    SerialOpen(true);
    Hello(host,9);
    Input(3,note,sizeof(note));
    midi.Poll({102,102,1000000,0},[&](const midi::Event& e){notes+=e.type==midi::EventType::NoteOn;return true;});
    assert(notes==3 && host.ConfigurationOk());
}
