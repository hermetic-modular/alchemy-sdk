set(USB_CORE "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/libDaisy/Middlewares/ST/STM32_USB_Device_Library/Core")
add_executable(usb_device_tests device_tests.cpp fixture.cpp ../../framework/src/usb/device_class.cpp
    ${USB_CORE}/Src/usbd_core.c ${USB_CORE}/Src/usbd_ctlreq.c ${USB_CORE}/Src/usbd_ioreq.c)
target_include_directories(usb_device_tests PRIVATE . ${USB_CORE}/Inc ../../framework/include)
target_compile_options(usb_device_tests PRIVATE -UNDEBUG -Werror)
add_test(NAME usb_device COMMAND usb_device_tests)

add_executable(usb_adapter_tests adapter_tests.cpp fixture.cpp ../../framework/src/usb/device_class.cpp
    ../../framework/src/usb/device.cpp ../../framework/src/host_link/cdc_transport.cpp
    ${USB_CORE}/Src/usbd_core.c ${USB_CORE}/Src/usbd_ctlreq.c ${USB_CORE}/Src/usbd_ioreq.c)
target_include_directories(usb_adapter_tests PRIVATE . ${USB_CORE}/Inc ../../framework/include ../../stubs)
target_compile_options(usb_adapter_tests PRIVATE -UNDEBUG -Werror)
add_test(NAME usb_adapter COMMAND usb_adapter_tests)

# Real Host startup and HELLO over shared CDC/MIDI, with each startup order in a fresh process.
add_executable(usb_hostlink_tests hostlink_tests.cpp fixture.cpp
    ../../framework/src/usb/device_class.cpp ../../framework/src/usb/device.cpp
    ../../framework/src/host_link/cdc_transport.cpp ../../framework/src/host_link/host.cpp
    ../../framework/src/host_link/host_link.cpp ../../framework/src/host_link/fast_values.cpp
    ../../framework/src/host_link/descriptor.cpp
    ../../framework/src/host_link/describe.cpp ../../framework/src/host_link/json_check.cpp
    ../../framework/src/surface/presets.cpp ../../framework/src/surface/virtual_knob.cpp
    ../../framework/src/surface/pager.cpp ../../framework/src/surface/settings.cpp
    ../../framework/src/surface/preset_gesture_ui.cpp ../../framework/src/surface/button_bank.cpp
    ../../hardware/alchemy-lab/v1/src/alchemy_lab_v1_layout.cpp
    ${USB_CORE}/Src/usbd_core.c ${USB_CORE}/Src/usbd_ctlreq.c ${USB_CORE}/Src/usbd_ioreq.c)
target_include_directories(usb_hostlink_tests PRIVATE . ${USB_CORE}/Inc
    ../../hardware/alchemy-lab/v1/include)
target_link_libraries(usb_hostlink_tests PRIVATE alchemy::primitives)
target_compile_definitions(usb_hostlink_tests PRIVATE DSY_SDRAM_BSS=)
target_compile_options(usb_hostlink_tests PRIVATE -UNDEBUG -Werror)
foreach(STARTUP IN ITEMS host-first midi-first configured explicit-match explicit-conflict host-first-fast midi-first-fast)
    add_test(NAME usb_hostlink_${STARTUP} COMMAND usb_hostlink_tests ${STARTUP})
endforeach()
