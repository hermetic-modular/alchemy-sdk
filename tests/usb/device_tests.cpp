#include "alchemy/usb/device_class.h"
#include "alchemy/midi/usb_packets.h"
extern "C" {
#include "usbd_ctlreq.h"
}
#include <cassert>
#include <vector>
#include <cstring>
using namespace alchemy::usb;
#include "fixture.h"
int main() {
    unsigned interfaces=0,endpoints=0,iads=0;
    for(size_t i=0;i<sizeof(kConfiguration);i+=kConfiguration[i]) {
        assert(kConfiguration[i]>=2 && i+kConfiguration[i]<=sizeof(kConfiguration));
        interfaces+=kConfiguration[i+1]==4; endpoints+=kConfiguration[i+1]==5; iads+=kConfiguration[i+1]==11;
    }
    assert(interfaces==4 && endpoints==5 && iads==2 && kConfiguration[2]==sizeof(kConfiguration));
    USBD_HandleTypeDef device{}; USBD_DescriptorsTypeDef desc{}; DeviceClass service;
    assert(USBD_Init(&device,&desc,0)==USBD_OK);
    service.Attach(device);
    assert(USBD_RegisterClass(&device,&DeviceClass::Driver())==USBD_OK);
    USBD_SetupReqTypedef get_configuration{0x80,USB_REQ_GET_DESCRIPTOR,uint16_t(USB_DESC_TYPE_CONFIGURATION<<8),0,175};
    assert(USBD_StdDevReq(&device,&get_configuration)==USBD_OK && in[0].size==175 && in[0].buffer[1]==2);
    assert(USBD_LL_Reset(&device)==USBD_OK);
    device.dev_state=USBD_STATE_ADDRESSED;
    USBD_SetupReqTypedef configure{0,USB_REQ_SET_CONFIGURATION,1,0,0};
    assert(USBD_StdDevReq(&device,&configure)==USBD_OK && service.Configured());
    for(uint8_t ep:{kCdcIn,kCdcNotify,kMidiIn}) assert(in[ep&15].open);
    assert(out[kCdcOut].open && out[kMidiOut].open);
    for(uint16_t iface=0;iface<4;++iface) {
        USBD_SetupReqTypedef get{0x81,USB_REQ_GET_INTERFACE,0,iface,1};
        assert(USBD_StdItfReq(&device,&get)==USBD_OK && in[0].size==1 && *in[0].buffer==0);
    }
    USBD_SetupReqTypedef invalid{0x81,USB_REQ_GET_INTERFACE,0,4,1};
    assert(USBD_StdItfReq(&device,&invalid)==USBD_FAIL && in[0].stalled);
    USBD_SetupReqTypedef line_get{0xa1,0x21,0,0,7};
    assert(USBD_StdItfReq(&device,&line_get)==USBD_OK && in[0].size==7 && in[0].buffer[6]==8);
    USBD_SetupReqTypedef line_set{0x21,0x20,0,0,7};
    const uint8_t coding[]={0x80,0x25,0,0,0,0,8};
    const uint8_t malformed[64]{};
    const auto line_generation=service.Generation();
    auto receive_line = [&](uint32_t size) {
        in[0].stalled=out[0].stalled=false;
        uint8_t setup[]={0x21,0x20,0,0,0,0,7,0};
        assert(USBD_LL_SetupStage(&device,setup)==USBD_OK);
        if(size)std::memcpy(out[0].buffer,size==7?coding:malformed,size);
        out[0].received=size;
        assert(USBD_LL_DataOutStage(&device,0,out[0].buffer)==USBD_OK);
    };
    receive_line(7);
    assert(!in[0].stalled && !out[0].stalled);
    assert(USBD_StdItfReq(&device,&line_get)==USBD_OK && !std::memcmp(in[0].buffer,coding,7));
    for(const auto size:{0u,6u,8u,64u}) {
        receive_line(size);
        assert(in[0].stalled && out[0].stalled);
        assert(service.Configured() && service.Generation()==line_generation);
        assert(USBD_StdItfReq(&device,&line_get)==USBD_OK && !std::memcmp(in[0].buffer,coding,7));
    }
    line_set.wLength=64;assert(USBD_StdItfReq(&device,&line_set)==USBD_FAIL);
    std::vector<uint8_t> cdc,midi;
    service.SetCdcReceiver({&cdc,[](void* p,const uint8_t* bytes,size_t size){auto& v=*static_cast<std::vector<uint8_t>*>(p);v.insert(v.end(),bytes,bytes+size);}});
    service.SetMidiReceiver({&midi,[](void* p,const uint8_t* bytes,size_t size){auto& v=*static_cast<std::vector<uint8_t>*>(p);v.insert(v.end(),bytes,bytes+size);}});
    std::memcpy(out[kCdcOut].buffer,"abc",3);out[kCdcOut].received=3;
    assert(USBD_LL_DataOutStage(&device,kCdcOut,out[kCdcOut].buffer)==USBD_OK && cdc.size()==3 && midi.empty());
    const uint8_t packet[]={9,0x90,60,100};std::memcpy(out[kMidiOut].buffer,packet,4);out[kMidiOut].received=4;
    assert(USBD_LL_DataOutStage(&device,kMidiOut,out[kMidiOut].buffer)==USBD_OK && midi.size()==4 && cdc.size()==3);
    uint8_t transfer[64];std::memset(transfer,42,64);
    assert(service.SendCdc(transfer,64) && service.SendMidi(packet,4));
    busy=true;service.Pump();assert(in[1].size==0 && in[3].size==0);
    busy=false;service.Pump();assert(in[1].size==64 && in[3].size==4);
    std::memset(transfer,99,64);assert(in[1].buffer[0]==42 && in[3].buffer[1]==0x90);
    assert(service.SendMidi(packet,4));service.Pump();assert(service.MidiCompleted()==0);
    CompleteIn(device,3);assert(service.MidiCompleted()==1);
    service.Pump();assert(service.MidiCompleted()==1);
    auto* cdc_buffer=in[1].buffer;
    busy_endpoint=kCdcIn;CompleteIn(device,1);
    assert(!in[1].active && in[1].buffer==cdc_buffer && cdc_buffer[0]==42);
    const uint8_t next_cdc[]={1,2,3};assert(service.SendCdc(next_cdc,sizeof(next_cdc)));
    assert(service.SendMidi(packet,4));CompleteIn(device,3);
    assert(in[3].active && service.MidiCompleted()==2);
    service.Pump();assert(!in[1].active && cdc_buffer[0]==42);
    busy_endpoint=0;service.Pump();assert(in[1].active && in[1].size==0);
    CompleteIn(device,1);assert(in[1].active && in[1].size==sizeof(next_cdc));
    assert(!std::memcmp(in[1].buffer,next_cdc,sizeof(next_cdc)));
    CompleteIn(device,1);
    USBD_SetupReqTypedef dtr{0x21,0x22,1,0,0};assert(USBD_StdItfReq(&device,&dtr)==USBD_OK);
    service.Pump();assert(in[2].size==10 && in[2].buffer[8]==3);
    CompleteIn(device,2);
    const auto generation=service.Generation();
    assert(USBD_LL_Reset(&device)==USBD_OK && !service.Configured() && service.Generation()!=generation);
    assert(service.MidiCancelled()==1 && !service.SendMidi(packet,4));
    device.dev_state=USBD_STATE_ADDRESSED;assert(USBD_StdDevReq(&device,&configure)==USBD_OK && service.Configured());
    service.Pump();assert(service.MidiCompleted()==2);
    for(unsigned i=0;i<32;++i)assert(service.SendMidi(packet,4));
    assert(!service.SendMidi(packet,4));
    service.Pump();assert(service.SendMidi(packet,4));
    const auto receive_epoch=service.Generation(), tx_epoch=service.MidiGeneration();
    USBD_SetupReqTypedef clear{2,USB_REQ_CLEAR_FEATURE,USB_FEATURE_EP_HALT,kMidiIn,0};
    assert(USBD_StdEPReq(&device,&clear)==USBD_OK);
    assert(service.Generation()==receive_epoch && service.MidiGeneration()!=tx_epoch);
    assert(service.SendMidi(packet,4));service.Pump();assert(in[3].active);
    const auto before_suspend=service.MidiGeneration();
    assert(USBD_LL_Suspend(&device)==USBD_OK && service.Configured() && service.Suspended() && !service.Available());
    service.Pump();assert(!service.SendMidi(packet,4) && !service.SendCdc(transfer,64));
    assert(service.MidiGeneration()!=before_suspend && service.Generation()!=receive_epoch);
    const auto after_suspend=service.Generation();service.Pump();assert(service.Generation()==after_suspend);
    assert(USBD_LL_Resume(&device)==USBD_OK);service.Pump();
    assert(service.Available() && service.Generation()==after_suspend && in[3].active);
    auto* active_buffer=in[3].buffer;
    assert(service.SendMidi(packet,4));service.Pump();assert(in[3].buffer==active_buffer);
    CompleteIn(device,3);
    assert(service.SendMidi(packet,4));service.Pump();assert(in[3].active);
    USBD_LL_DevDisconnected(&device);assert(!service.Configured() && !service.Available());
}
