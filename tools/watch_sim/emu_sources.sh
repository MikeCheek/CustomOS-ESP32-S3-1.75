# The in-app emulator builds every firmware/*.cpp except these: the hardware
# drivers and radio / SD / codec code, which sim_stubs.cpp replaces - the
# same list as run.sh's SKIP. Read by emu_host_test.sh and by
# companion_app/android/app/src/main/cpp/CMakeLists.txt.
EMU_SKIP=(
  diag hal_audio hal_bambu hal_ble hal_display hal_espnow hal_expander hal_fwupdate hal_gps hal_imu hal_ntp
  hal_ota hal_power hal_rtc hal_sd hal_touch hal_usb hal_wifi hal_wifi_xfer media_decode wifi_cfg
  app_bambu_printer app_espnow_status app_video app_wifi_setup
)
EMU_GFX=(Arduino_GFX Arduino_G Arduino_DataBus canvas/Arduino_Canvas)
