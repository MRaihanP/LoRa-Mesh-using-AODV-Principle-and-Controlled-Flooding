#pragma once
#include <RP_TFTDisplay.h>
#include "OS.h"
#include "TopBar.h"
#include "MenuPage.h"
#include "MainPage.h"
#include "Page_SimpleChat.h"
#include "Page_Network.h"
#include "Page_LogPath.h"
#include "Page_LogData.h"
#include "Page_SensorMPU9250.h"
#include "Page_SensorBMP280.h"
#include "Page_SensorSHT30.h"
#include "Page_GNSS_M10.h"
#include "Page_ClockDS3231.h"
#include "Page_MicroSD.h"
#include "Page_Battery.h"
#include "Page_Wifi.h"
#include "IRouter.h"

class PageRouter : public IRouter {
public:
  void begin(AppOS* os, TopBar* top, QuickSettingsPanel* qs,
             MainPage* main, MenuPage* menu,
             PageSimpleChat* schat, PageNetwork* net,
             PageLogPath* lpath, PageLogData* ldata,
             PageSensorMPU9250* p_mpu, PageSensorBMP280* p_bmp,
             PageSensorSHT30* p_sht, PageGNSS_M10* p_m10,
             PageClockDS3231* p_clk, PageMicroSD* p_msd,
             PageBattery* p_bat, PageWifi* p_wifi) {
    _os=os; _top=top; _qs=qs; _main=main; _menu=menu;
    _schat=schat; _net=net; _lpath=lpath; _ldata=ldata;
    _p_mpu=p_mpu; _p_bmp=p_bmp; _p_sht=p_sht; _p_m10=p_m10;
    _p_clk=p_clk; _p_msd=p_msd; _p_bat=p_bat; _p_wifi=p_wifi;
    _state=Home; _pending=None;
  }

  // Requests
  void requestHome()            override { _pending = ToHome; }
  void requestMenu()            override { _pending = ToMenu; }
  void requestSimpleChat()      override { _pending = ToSimpleChat; }
  void requestNetwork()         override { _pending = ToNetwork; }
  void requestLogPath()         override { _pending = ToLogPath; }
  void requestLogData()         override { _pending = ToLogData; }
  void requestSensorMPU9250()   override { _pending = ToSensorMPU9250; }
  void requestSensorBMP280()    override { _pending = ToSensorBMP280; }
  void requestSensorSHT30()     override { _pending = ToSensorSHT30; }
  void requestGNSS_M10()        override { _pending = ToGNSS_M10; }
  void requestClockDS3231()     override { _pending = ToClockDS3231; }
  void requestMicroSD()         override { _pending = ToMicroSD; }
  void requestBattery()         override { _pending = ToBattery; }
  void requestWifi()            override { _pending = ToWifi; }

  // Commit switch
  void pumpAfterOverlayClosed() override {
    if (_pending == None) return;

    // full wipe
    Renderer& R = _os->ui.renderer();
    R.clear(rgb565(0,0,0));
    R.swapAndFlush();

    // redraw topbar
    if (_top) _top->redrawAll();

    // switch page
    switch (_pending){
      case ToMenu:             if (_menu)   _menu->onEnter();       _state = Menu;             break;
      case ToSimpleChat:       if (_schat)  _schat->onEnter();      _state = SimpleChat;       break;
      case ToNetwork:          if (_net)    _net->onEnter();        _state = Network;          break;
      case ToLogPath:          if (_lpath)  _lpath->onEnter();      _state = LogPath;          break;
      case ToLogData:          if (_ldata)  _ldata->onEnter();      _state = LogData;          break;
      case ToSensorMPU9250:    if (_p_mpu)  _p_mpu->onEnter();      _state = SensorMPU9250;    break;
      case ToSensorBMP280:     if (_p_bmp)  _p_bmp->onEnter();      _state = SensorBMP280;     break;
      case ToSensorSHT30:      if (_p_sht)  _p_sht->onEnter();      _state = SensorSHT30;      break;
      case ToGNSS_M10:         if (_p_m10)  _p_m10->onEnter();      _state = GNSS_M10;         break;
      case ToClockDS3231:      if (_p_clk)  _p_clk->onEnter();      _state = ClockDS3231;      break;
      case ToMicroSD:          if (_p_msd)  _p_msd->onEnter();      _state = MicroSD;          break;
      case ToBattery:          if (_p_bat)  _p_bat->onEnter();      _state = Battery;          break;
      case ToWifi:             if (_p_wifi) _p_wifi->onEnter();     _state = Wifi;             break;
      case ToHome:
      default:
        _state = Home;
        if (_main) _main->onQSClosed(); // redraw home
        break;
    }
    _pending = None;
  }

  // Queries
  bool inMenu()           const override { return _state == Menu; }
  bool inSimpleChat()     const override { return _state == SimpleChat; }
  bool inNetwork()        const override { return _state == Network; }
  bool inLogPath()        const override { return _state == LogPath; }
  bool inLogData()        const override { return _state == LogData; }
  bool inSensorMPU9250()  const override { return _state == SensorMPU9250; }
  bool inSensorBMP280()   const override { return _state == SensorBMP280; }
  bool inSensorSHT30()    const override { return _state == SensorSHT30; }
  bool inGNSS_M10()       const override { return _state == GNSS_M10; }
  bool inClockDS3231()    const override { return _state == ClockDS3231; }
  bool inMicroSD()        const override { return _state == MicroSD; }
  bool inBattery()        const override { return _state == Battery; }
  bool inWifi()           const override { return _state == Wifi; }

private:
  enum State   : uint8_t {
    Home=0, Menu, SimpleChat, Network, LogPath, LogData,
    SensorMPU9250, SensorBMP280, SensorSHT30, GNSS_M10,
    ClockDS3231, MicroSD, Battery, Wifi
  };
  enum Pending : uint8_t {
    None=0, ToHome, ToMenu, ToSimpleChat, ToNetwork, ToLogPath, ToLogData,
    ToSensorMPU9250, ToSensorBMP280, ToSensorSHT30, ToGNSS_M10,
    ToClockDS3231, ToMicroSD, ToBattery, ToWifi
  };

  AppOS* _os=nullptr; TopBar* _top=nullptr; QuickSettingsPanel* _qs=nullptr;
  MainPage* _main=nullptr; MenuPage* _menu=nullptr;
  PageSimpleChat* _schat=nullptr; PageNetwork* _net=nullptr;
  PageLogPath* _lpath=nullptr; PageLogData* _ldata=nullptr;
  PageSensorMPU9250* _p_mpu=nullptr; PageSensorBMP280* _p_bmp=nullptr;
  PageSensorSHT30* _p_sht=nullptr; PageGNSS_M10* _p_m10=nullptr;
  PageClockDS3231* _p_clk=nullptr; PageMicroSD* _p_msd=nullptr;
  PageBattery* _p_bat=nullptr; PageWifi* _p_wifi=nullptr;

  State _state=Home; Pending _pending=None;
};