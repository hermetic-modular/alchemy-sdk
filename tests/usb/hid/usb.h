#pragma once
namespace daisy {
class UsbHandle {
  public:
    enum class Result { OK,ERR };
    enum UsbPeriph { FS_INTERNAL,FS_EXTERNAL,FS_BOTH };
};
}
